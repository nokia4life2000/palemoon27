/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <CoreFoundation/CFString.h>

#include "AppleUtils.h"
#include "AppleVDADecoder.h"
#include "AppleVDALinker.h"
#include "MediaInfo.h"
#include "mp4_demuxer/H264.h"
#include "MP4Decoder.h"
#include "MediaData.h"
#include "MacIOSurfaceImage.h"
#include "mozilla/ArrayUtils.h"
#include "nsAutoPtr.h"
#include "nsCocoaFeatures.h"
#include "nsThreadUtils.h"
#include "mozilla/Logging.h"
#include "VideoUtils.h"
#include <algorithm>
#include "gfxPlatform.h"

extern PRLogModuleInfo* GetPDMLog();
#define LOG(...) MOZ_LOG(GetPDMLog(), mozilla::LogLevel::Debug, (__VA_ARGS__))
//#define LOG_MEDIA_SHA1

namespace mozilla {

AppleVDADecoder::AppleVDADecoder(const VideoInfo& aConfig,
                               FlushableTaskQueue* aVideoTaskQueue,
                               MediaDataDecoderCallback* aCallback,
                               layers::ImageContainer* aImageContainer)
  : mTaskQueue(aVideoTaskQueue)
  , mCallback(aCallback)
  , mImageContainer(aImageContainer)
  , mPictureWidth(aConfig.mImage.width)
  , mPictureHeight(aConfig.mImage.height)
  , mDisplayWidth(aConfig.mDisplay.width)
  , mDisplayHeight(aConfig.mDisplay.height)
  , mInputIncoming(0)
  , mIsShutDown(false)
  , mUseSoftwareImages(false)
  , mIs106(!nsCocoaFeatures::OnLionOrLater())
  , mMonitor("AppleVideoDecoder")
  , mIsFlushing(false)
  , mDecoder(nullptr)
{
  MOZ_COUNT_CTOR(AppleVDADecoder);
  // TODO: Verify aConfig.mime_type.

  mExtraData = aConfig.mExtraData;
  mMaxRefFrames = 4;
  // Retrieve video dimensions from H264 SPS NAL.
  mp4_demuxer::SPSData spsdata;
  if (mp4_demuxer::H264::DecodeSPSFromExtraData(mExtraData, spsdata)) {
    // max_num_ref_frames determines the size of the sliding window
    // we need to queue that many frames in order to guarantee proper
    // pts frames ordering. Use a minimum of 4 to ensure proper playback of
    // non compliant videos.
    mMaxRefFrames =
      std::min(std::max(mMaxRefFrames, spsdata.max_num_ref_frames + 1), 16u);
  }

  LOG("Creating AppleVDADecoder for %dx%d (%dx%d) h.264 video",
      mPictureWidth,
      mPictureHeight,
      mDisplayWidth,
      mDisplayHeight
     );
}

AppleVDADecoder::~AppleVDADecoder()
{
  MOZ_COUNT_DTOR(AppleVDADecoder);
}

RefPtr<MediaDataDecoder::InitPromise>
AppleVDADecoder::Init()
{
  return InitPromise::CreateAndResolve(TrackType::kVideoTrack, __func__);
}

nsresult
AppleVDADecoder::Shutdown()
{
  MOZ_DIAGNOSTIC_ASSERT(!mIsShutDown);
  mIsShutDown = true;
  if (mTaskQueue) {
    nsCOMPtr<nsIRunnable> runnable =
      NS_NewRunnableMethod(this, &AppleVDADecoder::ProcessShutdown);
    mTaskQueue->Dispatch(runnable.forget());
  } else {
    ProcessShutdown();
  }
  return NS_OK;
}

void
AppleVDADecoder::ProcessShutdown()
{
  if (mDecoder) {
    LOG("%s: cleaning up decoder %p", __func__, mDecoder);
    VDADecoderDestroy(mDecoder);
    mDecoder = nullptr;
  }
}

nsresult
AppleVDADecoder::Input(MediaRawData* aSample)
{
  MOZ_ASSERT(mCallback->OnReaderTaskQueue());

  LOG("mp4 input sample %p pts %lld duration %lld us%s %d bytes",
      aSample,
      aSample->mTime,
      aSample->mDuration,
      aSample->mKeyframe ? " keyframe" : "",
      aSample->Size());

  mInputIncoming++;

  nsCOMPtr<nsIRunnable> runnable =
      NS_NewRunnableMethodWithArg<RefPtr<MediaRawData>>(
          this,
          &AppleVDADecoder::SubmitFrame,
          RefPtr<MediaRawData>(aSample));
  mTaskQueue->Dispatch(runnable.forget());
  return NS_OK;
}

nsresult
AppleVDADecoder::Flush()
{
  MOZ_ASSERT(mCallback->OnReaderTaskQueue());
  mIsFlushing = true;
  mTaskQueue->Flush();
  nsCOMPtr<nsIRunnable> runnable =
    NS_NewRunnableMethod(this, &AppleVDADecoder::ProcessFlush);
  MonitorAutoLock mon(mMonitor);
  mTaskQueue->Dispatch(runnable.forget());
  while (mIsFlushing) {
    mon.Wait();
  }
  mInputIncoming = 0;
  return NS_OK;
}

nsresult
AppleVDADecoder::Drain()
{
  MOZ_ASSERT(mCallback->OnReaderTaskQueue());
  nsCOMPtr<nsIRunnable> runnable =
    NS_NewRunnableMethod(this, &AppleVDADecoder::ProcessDrain);
  mTaskQueue->Dispatch(runnable.forget());
  return NS_OK;
}

void
AppleVDADecoder::ProcessFlush()
{
  MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());

  OSStatus rv = VDADecoderFlush(mDecoder, 0 /*dont emit*/);
  if (rv != noErr) {
    LOG("AppleVDADecoder::Flush failed waiting for platform decoder "
        "with error:%d.", rv);
  }
  ClearReorderedFrames();
  MonitorAutoLock mon(mMonitor);
  mIsFlushing = false;
  mon.NotifyAll();
}

void
AppleVDADecoder::ProcessDrain()
{
  MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());

  OSStatus rv = VDADecoderFlush(mDecoder, kVDADecoderFlush_EmitFrames);
  if (rv != noErr) {
    LOG("AppleVDADecoder::Drain failed waiting for platform decoder "
        "with error:%d.", rv);
  }
  DrainReorderedFrames();
  mCallback->DrainComplete();
}

//
// Implementation details.
//

// Callback passed to the VideoToolbox decoder for returning data.
// This needs to be static because the API takes a C-style pair of
// function and userdata pointers. This validates parameters and
// forwards the decoded image back to an object method.
static void
PlatformCallback(void* decompressionOutputRefCon,
                 CFDictionaryRef frameInfo,
                 OSStatus status,
                 VDADecodeInfoFlags infoFlags,
                 CVImageBufferRef image)
{
  LOG("AppleVDADecoder[%s] status %d flags %d retainCount %ld",
      __func__, status, infoFlags, CFGetRetainCount(frameInfo));

  // Validate our arguments.
  // According to Apple's TN2267
  // The output callback is still called for all flushed frames,
  // but no image buffers will be returned.
  // FIXME: Distinguish between errors and empty flushed frames.
  if (status != noErr || !image) {
    NS_WARNING("AppleVDADecoder decoder returned no data");
    return;
  }
  MOZ_ASSERT(CFGetTypeID(image) == CVPixelBufferGetTypeID(),
             "AppleVDADecoder returned an unexpected image type");

  if (infoFlags & kVDADecodeInfo_FrameDropped)
  {
    NS_WARNING("  ...frame dropped...");
    return;
  }

  AppleVDADecoder* decoder =
    static_cast<AppleVDADecoder*>(decompressionOutputRefCon);

  AutoCFRelease<CFNumberRef> ptsref =
    (CFNumberRef)CFDictionaryGetValue(frameInfo, CFSTR("FRAME_PTS"));
  AutoCFRelease<CFNumberRef> dtsref =
    (CFNumberRef)CFDictionaryGetValue(frameInfo, CFSTR("FRAME_DTS"));
  AutoCFRelease<CFNumberRef> durref =
    (CFNumberRef)CFDictionaryGetValue(frameInfo, CFSTR("FRAME_DURATION"));
  AutoCFRelease<CFNumberRef> boref =
    (CFNumberRef)CFDictionaryGetValue(frameInfo, CFSTR("FRAME_OFFSET"));
  AutoCFRelease<CFNumberRef> kfref =
    (CFNumberRef)CFDictionaryGetValue(frameInfo, CFSTR("FRAME_KEYFRAME"));

  int64_t dts;
  int64_t pts;
  int64_t duration;
  int64_t byte_offset;
  char is_sync_point;

  CFNumberGetValue(ptsref, kCFNumberSInt64Type, &pts);
  CFNumberGetValue(dtsref, kCFNumberSInt64Type, &dts);
  CFNumberGetValue(durref, kCFNumberSInt64Type, &duration);
  CFNumberGetValue(boref, kCFNumberSInt64Type, &byte_offset);
  CFNumberGetValue(kfref, kCFNumberSInt8Type, &is_sync_point);

  AppleVDADecoder::AppleFrameRef frameRef(
      media::TimeUnit::FromMicroseconds(dts),
      media::TimeUnit::FromMicroseconds(pts),
      media::TimeUnit::FromMicroseconds(duration),
      byte_offset,
      is_sync_point == 1);

  // Forward the data back to an object method which can access
  // the correct reader's callback.
  nsCOMPtr<nsIRunnable> task =
    NS_NewRunnableMethodWithArgs<CFRefPtr<CVPixelBufferRef>, AppleVDADecoder::AppleFrameRef>(
      decoder, &AppleVDADecoder::OutputFrame, image, frameRef);
  decoder->DispatchOutputTask(task.forget());
}

AppleVDADecoder::AppleFrameRef*
AppleVDADecoder::CreateAppleFrameRef(const MediaRawData* aSample)
{
  MOZ_ASSERT(aSample);
  return new AppleFrameRef(*aSample);
}

void
AppleVDADecoder::DrainReorderedFrames()
{
  while (!mReorderQueue.IsEmpty()) {
    mCallback->Output(mReorderQueue.Pop().get());
  }
}

void
AppleVDADecoder::ClearReorderedFrames()
{
  while (!mReorderQueue.IsEmpty()) {
    mReorderQueue.Pop();
  }
}

// Copy and return a decoded frame.
nsresult
AppleVDADecoder::OutputFrame(CFRefPtr<CVPixelBufferRef> aImage,
                             AppleVDADecoder::AppleFrameRef aFrameRef)
{
  MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());

  if (mIsFlushing) {
    // We are in the process of flushing; ignore frame.
    return NS_OK;
  }

  LOG("mp4 output frame %lld dts %lld pts %lld duration %lld us%s",
      aFrameRef.byte_offset,
      aFrameRef.decode_timestamp.ToMicroseconds(),
      aFrameRef.composition_timestamp.ToMicroseconds(),
      aFrameRef.duration.ToMicroseconds(),
      aFrameRef.is_sync_point ? " keyframe" : ""
  );

  // Where our resulting image will end up.
  RefPtr<VideoData> data;
  // Bounds.
  VideoInfo info;
  info.mDisplay = nsIntSize(mDisplayWidth, mDisplayHeight);
  gfx::IntRect visible = gfx::IntRect(0,
                                      0,
                                      mPictureWidth,
                                      mPictureHeight);

  if (mUseSoftwareImages) {
    size_t width = CVPixelBufferGetWidth(aImage);
    size_t height = CVPixelBufferGetHeight(aImage);
    DebugOnly<size_t> planes = CVPixelBufferGetPlaneCount(aImage);
    MOZ_ASSERT(planes == 2, "Likely not NV12 format and it must be.");

    VideoData::YCbCrBuffer buffer;

    // Lock the returned image data.
    CVReturn rv = CVPixelBufferLockBaseAddress(aImage, kCVPixelBufferLock_ReadOnly);
    if (rv != kCVReturnSuccess) {
      NS_ERROR("error locking pixel data");
      mCallback->Error();
      return NS_ERROR_FAILURE;
    }
    // Y plane.
    buffer.mPlanes[0].mData =
      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(aImage, 0));
    buffer.mPlanes[0].mStride = CVPixelBufferGetBytesPerRowOfPlane(aImage, 0);
    buffer.mPlanes[0].mWidth = width;
    buffer.mPlanes[0].mHeight = height;
    buffer.mPlanes[0].mOffset = 0;
    buffer.mPlanes[0].mSkip = 0;
    // Cb plane.
    buffer.mPlanes[1].mData =
      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(aImage, 1));
    buffer.mPlanes[1].mStride = CVPixelBufferGetBytesPerRowOfPlane(aImage, 1);
    buffer.mPlanes[1].mWidth = (width+1) / 2;
    buffer.mPlanes[1].mHeight = (height+1) / 2;
    buffer.mPlanes[1].mOffset = 0;
    buffer.mPlanes[1].mSkip = 1;
    // Cr plane.
    buffer.mPlanes[2].mData =
      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(aImage, 1));
    buffer.mPlanes[2].mStride = CVPixelBufferGetBytesPerRowOfPlane(aImage, 1);
    buffer.mPlanes[2].mWidth = (width+1) / 2;
    buffer.mPlanes[2].mHeight = (height+1) / 2;
    buffer.mPlanes[2].mOffset = 1;
    buffer.mPlanes[2].mSkip = 1;

    // Copy the image data into our own format.
    data =
      VideoData::Create(info,
                        mImageContainer,
                        nullptr,
                        aFrameRef.byte_offset,
                        aFrameRef.composition_timestamp.ToMicroseconds(),
                        aFrameRef.duration.ToMicroseconds(),
                        buffer,
                        aFrameRef.is_sync_point,
                        aFrameRef.decode_timestamp.ToMicroseconds(),
                        visible);
    // Unlock the returned image data.
    CVPixelBufferUnlockBaseAddress(aImage, kCVPixelBufferLock_ReadOnly);
  } else {
    IOSurfacePtr surface = MacIOSurfaceLib::CVPixelBufferGetIOSurface(aImage);
    MOZ_ASSERT(surface, "Decoder didn't return an IOSurface backed buffer");

    RefPtr<MacIOSurface> macSurface = new MacIOSurface(surface);

    RefPtr<layers::Image> image =
      mImageContainer->CreateImage(ImageFormat::MAC_IOSURFACE);
    layers::MacIOSurfaceImage* videoImage =
      static_cast<layers::MacIOSurfaceImage*>(image.get());
    videoImage->SetSurface(macSurface);

    data =
      VideoData::CreateFromImage(info,
                                 mImageContainer,
                                 aFrameRef.byte_offset,
                                 aFrameRef.composition_timestamp.ToMicroseconds(),
                                 aFrameRef.duration.ToMicroseconds(),
                                 image.forget(),
                                 aFrameRef.is_sync_point,
                                 aFrameRef.decode_timestamp.ToMicroseconds(),
                                 visible);
  }

  if (!data) {
    NS_ERROR("Couldn't create VideoData for frame");
    mCallback->Error();
    return NS_ERROR_FAILURE;
  }

  // Frames come out in DTS order but we need to output them
  // in composition order.
  mReorderQueue.Push(data);
  while (mReorderQueue.Length() > mMaxRefFrames) {
    mCallback->Output(mReorderQueue.Pop().get());
  }
  LOG("%llu decoded frames queued",
      static_cast<unsigned long long>(mReorderQueue.Length()));

  return NS_OK;
}

nsresult
AppleVDADecoder::SubmitFrame(MediaRawData* aSample)
{
  MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());

  mInputIncoming--;

  AutoCFRelease<CFDataRef> block =
    CFDataCreate(kCFAllocatorDefault, aSample->Data(), aSample->Size());
  if (!block) {
    NS_ERROR("Couldn't create CFData");
    return NS_ERROR_FAILURE;
  }

  AutoCFRelease<CFNumberRef> pts =
    CFNumberCreate(kCFAllocatorDefault,
                   kCFNumberSInt64Type,
                   &aSample->mTime);
  AutoCFRelease<CFNumberRef> dts =
    CFNumberCreate(kCFAllocatorDefault,
                   kCFNumberSInt64Type,
                   &aSample->mTimecode);
  AutoCFRelease<CFNumberRef> duration =
    CFNumberCreate(kCFAllocatorDefault,
                   kCFNumberSInt64Type,
                   &aSample->mDuration);
  AutoCFRelease<CFNumberRef> byte_offset =
    CFNumberCreate(kCFAllocatorDefault,
                   kCFNumberSInt64Type,
                   &aSample->mOffset);
  char keyframe = aSample->mKeyframe ? 1 : 0;
  AutoCFRelease<CFNumberRef> cfkeyframe =
    CFNumberCreate(kCFAllocatorDefault,
                   kCFNumberSInt8Type,
                   &keyframe);

  const void* keys[] = { CFSTR("FRAME_PTS"),
                         CFSTR("FRAME_DTS"),
                         CFSTR("FRAME_DURATION"),
                         CFSTR("FRAME_OFFSET"),
                         CFSTR("FRAME_KEYFRAME") };
  const void* values[] = { pts,
                           dts,
                           duration,
                           byte_offset,
                           cfkeyframe };
  static_assert(ArrayLength(keys) == ArrayLength(values),
                "Non matching keys/values array size");

  AutoCFRelease<CFDictionaryRef> frameInfo =
    CFDictionaryCreate(kCFAllocatorDefault,
                       keys,
                       values,
                       ArrayLength(keys),
                       &kCFTypeDictionaryKeyCallBacks,
                       &kCFTypeDictionaryValueCallBacks);

  OSStatus rv = VDADecoderDecode(mDecoder,
                                 0,
                                 block,
                                 frameInfo);

  if (rv != noErr) {
    NS_WARNING("AppleVDADecoder: Couldn't pass frame to decoder");
    return NS_ERROR_FAILURE;
  }

  if (mIs106) {
    // TN2267:
    // frameInfo: A CFDictionaryRef containing information to be returned in
    // the output callback for this frame.
    // This dictionary can contain client provided information associated with
    // the frame being decoded, for example presentation time.
    // The CFDictionaryRef will be retained by the framework.
    // In 10.6, it is released one too many. So retain it.
    CFRetain(frameInfo);
  }

  // Ask for more data.
  if (!mInputIncoming) {
    LOG("AppleVDADecoder task queue empty; requesting more data");
    mCallback->InputExhausted();
  }

  return NS_OK;
}

nsresult
AppleVDADecoder::InitializeSession()
{
  OSStatus rv;

  AutoCFRelease<CFDictionaryRef> decoderConfig =
    CreateDecoderSpecification();

  AutoCFRelease<CFDictionaryRef> outputConfiguration =
    CreateOutputConfiguration();

  rv =
    VDADecoderCreate(decoderConfig,
                     outputConfiguration,
                     (VDADecoderOutputCallback*)PlatformCallback,
                     this,
                     &mDecoder);

  if (rv != noErr) {
    NS_ERROR("AppleVDADecoder: Couldn't create decoder!");
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

CFDictionaryRef
AppleVDADecoder::CreateDecoderSpecification()
{
  const uint8_t* extradata = mExtraData->Elements();
  int extrasize = mExtraData->Length();

  OSType format = 'avc1';
  AutoCFRelease<CFNumberRef> avc_width  =
    CFNumberCreate(kCFAllocatorDefault,
                   kCFNumberSInt32Type,
                   &mPictureWidth);
  AutoCFRelease<CFNumberRef> avc_height =
    CFNumberCreate(kCFAllocatorDefault,
                   kCFNumberSInt32Type,
                   &mPictureHeight);
  AutoCFRelease<CFNumberRef> avc_format =
    CFNumberCreate(kCFAllocatorDefault,
                   kCFNumberSInt32Type,
                   &format);
  AutoCFRelease<CFDataRef> avc_data =
    CFDataCreate(kCFAllocatorDefault,
                 extradata,
                 extrasize);

  const void* decoderKeys[] = { AppleVDALinker::skPropWidth,
                                AppleVDALinker::skPropHeight,
                                AppleVDALinker::skPropSourceFormat,
                                AppleVDALinker::skPropAVCCData };
  const void* decoderValue[] = { avc_width,
                                 avc_height,
                                 avc_format,
                                 avc_data };
  static_assert(ArrayLength(decoderKeys) == ArrayLength(decoderValue),
                "Non matching keys/values array size");

  return CFDictionaryCreate(kCFAllocatorDefault,
                            decoderKeys,
                            decoderValue,
                            ArrayLength(decoderKeys),
                            &kCFTypeDictionaryKeyCallBacks,
                            &kCFTypeDictionaryValueCallBacks);
}

CFDictionaryRef
AppleVDADecoder::CreateOutputConfiguration()
{
  // Output format type:
  SInt32 PixelFormatTypeValue = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  AutoCFRelease<CFNumberRef> PixelFormatTypeNumber =
    CFNumberCreate(kCFAllocatorDefault,
                   kCFNumberSInt32Type,
                   &PixelFormatTypeValue);

  if (mUseSoftwareImages) {
    const void* outputKeys[] = { kCVPixelBufferPixelFormatTypeKey };
    const void* outputValues[] = { PixelFormatTypeNumber };
    static_assert(ArrayLength(outputKeys) == ArrayLength(outputValues),
                  "Non matching keys/values array size");

    return CFDictionaryCreate(kCFAllocatorDefault,
                              outputKeys,
                              outputValues,
                              ArrayLength(outputKeys),
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
  }

  // Construct IOSurface Properties
  const void* IOSurfaceKeys[] = { MacIOSurfaceLib::kPropIsGlobal };
  const void* IOSurfaceValues[] = { kCFBooleanTrue };
  static_assert(ArrayLength(IOSurfaceKeys) == ArrayLength(IOSurfaceValues),
                "Non matching keys/values array size");

  // Contruct output configuration.
  AutoCFRelease<CFDictionaryRef> IOSurfaceProperties =
    CFDictionaryCreate(kCFAllocatorDefault,
                       IOSurfaceKeys,
                       IOSurfaceValues,
                       ArrayLength(IOSurfaceKeys),
                       &kCFTypeDictionaryKeyCallBacks,
                       &kCFTypeDictionaryValueCallBacks);

  const void* outputKeys[] = { kCVPixelBufferIOSurfacePropertiesKey,
                               kCVPixelBufferPixelFormatTypeKey,
                               kCVPixelBufferOpenGLCompatibilityKey };
  const void* outputValues[] = { IOSurfaceProperties,
                                 PixelFormatTypeNumber,
                                 kCFBooleanTrue };
  static_assert(ArrayLength(outputKeys) == ArrayLength(outputValues),
                "Non matching keys/values array size");

  return CFDictionaryCreate(kCFAllocatorDefault,
                            outputKeys,
                            outputValues,
                            ArrayLength(outputKeys),
                            &kCFTypeDictionaryKeyCallBacks,
                            &kCFTypeDictionaryValueCallBacks);
}

/* static */
already_AddRefed<AppleVDADecoder>
AppleVDADecoder::CreateVDADecoder(
  const VideoInfo& aConfig,
  FlushableTaskQueue* aVideoTaskQueue,
  MediaDataDecoderCallback* aCallback,
  layers::ImageContainer* aImageContainer)
{
  if (!gfxPlatform::GetPlatform()->CanUseHardwareVideoDecoding()) {
    // This GPU is blacklisted for hardware decoding.
    return nullptr;
  }

  RefPtr<AppleVDADecoder> decoder =
    new AppleVDADecoder(aConfig, aVideoTaskQueue, aCallback, aImageContainer);

  if (NS_FAILED(decoder->InitializeSession())) {
    return nullptr;
  }

  return decoder.forget();
}

} // namespace mozilla
