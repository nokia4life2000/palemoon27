/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(GonkVideoDecoderManager_h_)
#define GonkVideoDecoderManager_h_

#include <set>
#include "nsRect.h"
#include "GonkMediaDataDecoder.h"
#include "mozilla/RefPtr.h"
#include "I420ColorConverterHelper.h"
#include "MediaCodecProxy.h"
#include <stagefright/foundation/AHandler.h>
#include "GonkNativeWindow.h"
#include "GonkNativeWindowClient.h"

using namespace android;

namespace android {
struct ALooper;
class MediaBuffer;
struct MOZ_EXPORT AString;
class GonkNativeWindow;
} // namespace android

namespace mozilla {

namespace layers {
class TextureClient;
} // namespace mozilla::layers

class GonkVideoDecoderManager : public GonkDecoderManager {
typedef android::MediaCodecProxy MediaCodecProxy;
typedef mozilla::layers::TextureClient TextureClient;

public:
  GonkVideoDecoderManager(mozilla::layers::ImageContainer* aImageContainer,
		                      const VideoInfo& aConfig);

  virtual ~GonkVideoDecoderManager() override;

  RefPtr<InitPromise> Init(MediaDataDecoderCallback* aCallback) override;

  nsresult Input(MediaRawData* aSample) override;

  nsresult Output(int64_t aStreamOffset,
                          RefPtr<MediaData>& aOutput) override;

  nsresult Flush() override;

  bool HasQueuedSample() override;

  static void RecycleCallback(TextureClient* aClient, void* aClosure);

private:
  struct FrameInfo
  {
    int32_t mWidth = 0;
    int32_t mHeight = 0;
    int32_t mStride = 0;
    int32_t mSliceHeight = 0;
    int32_t mColorFormat = 0;
    int32_t mCropLeft = 0;
    int32_t mCropTop = 0;
    int32_t mCropRight = 0;
    int32_t mCropBottom = 0;
  };
  class MessageHandler : public android::AHandler
  {
  public:
    MessageHandler(GonkVideoDecoderManager *aManager);
    ~MessageHandler();

    void onMessageReceived(const android::sp<android::AMessage> &aMessage) override;

  private:
    // Forbidden
    MessageHandler() = delete;
    MessageHandler(const MessageHandler &rhs) = delete;
    const MessageHandler &operator=(const MessageHandler &rhs) = delete;

    GonkVideoDecoderManager *mManager;
  };
  friend class MessageHandler;

  class VideoResourceListener : public android::MediaCodecProxy::CodecResourceListener
  {
  public:
    VideoResourceListener(GonkVideoDecoderManager *aManager);
    ~VideoResourceListener();

    void codecReserved() override;
    void codecCanceled() override;

  private:
    // Forbidden
    VideoResourceListener() = delete;
    VideoResourceListener(const VideoResourceListener &rhs) = delete;
    const VideoResourceListener &operator=(const VideoResourceListener &rhs) = delete;

    GonkVideoDecoderManager *mManager;
  };
  friend class VideoResourceListener;

  // FrameTimeInfo keeps the presentation time stamp (pts) and its duration.
  // On MediaDecoderStateMachine, it needs pts and duration to display decoded
  // frame correctly. But OMX can carry one field of time info (kKeyTime) so
  // we use FrameTimeInfo to keep pts and duration.
  struct FrameTimeInfo {
    int64_t pts;       // presentation time stamp of this frame.
    int64_t duration;  // the playback duration.
  };

  bool SetVideoFormat();

  nsresult CreateVideoData(int64_t aStreamOffset, VideoData** aOutData);
  void ReleaseVideoBuffer();
  uint8_t* GetColorConverterBuffer(int32_t aWidth, int32_t aHeight);

  // For codec resource management
  void codecReserved();
  void codecCanceled();
  void onMessageReceived(const sp<AMessage> &aMessage);

  void ReleaseAllPendingVideoBuffers();
  void PostReleaseVideoBuffer(android::MediaBuffer *aBuffer);

  void QueueFrameTimeIn(int64_t aPTS, int64_t aDuration);
  nsresult QueueFrameTimeOut(int64_t aPTS, int64_t& aDuration);
  void ClearQueueFrameTime();

  uint32_t mVideoWidth;
  uint32_t mVideoHeight;
  uint32_t mDisplayWidth;
  uint32_t mDisplayHeight;
  nsIntRect mPicture;
  nsIntSize mInitialFrame;

  RefPtr<layers::ImageContainer> mImageContainer;

  android::MediaBuffer* mVideoBuffer;

  RefPtr<MediaByteBuffer>  mCodecSpecificData;
  MediaDataDecoderCallback*  mReaderCallback;
  MediaInfo mInfo;
  android::sp<VideoResourceListener> mVideoListener;
  android::sp<MessageHandler> mHandler;
  android::sp<ALooper> mLooper;
  android::sp<ALooper> mManagerLooper;
  FrameInfo mFrameInfo;

  // Array of FrameTimeInfo whose corresponding frames are sent to OMX.
  // Ideally, it is a FIFO. Input() adds the entry to the end element and
  // CreateVideoData() takes the first entry. However, there are exceptions
  // due to MediaCodec error or seeking.
  nsTArray<FrameTimeInfo> mFrameTimeInfo;

  // color converter
  android::I420ColorConverterHelper mColorConverter;
  nsAutoArrayPtr<uint8_t> mColorConverterBuffer;
  size_t mColorConverterBufferSize;

  android::sp<android::GonkNativeWindow> mNativeWindow;
  enum {
    kNotifyPostReleaseBuffer = 'nprb',
  };

  // Hold video's MediaBuffers that are released.
  // The holded MediaBuffers are released soon after flush.
  Vector<android::MediaBuffer*> mPendingVideoBuffers;
  // The lock protects mPendingVideoBuffers.
  Mutex mPendingVideoBuffersLock;

  nsAutoCString mMimeType;

  // This monitor protects mQueueSample.
  Monitor mMonitor;

  // This TaskQueue should be the same one in mReaderCallback->OnReaderTaskQueue().
  // It is for codec resource mangement, decoding task should not dispatch to it.
  RefPtr<TaskQueue> mReaderTaskQueue;

  // An queue with the MP4 samples which are waiting to be sent into OMX.
  // If an element is an empty MP4Sample, that menas EOS. There should not
  // any sample be queued after EOS.
  nsTArray<RefPtr<MediaRawData>> mQueueSample;
};

} // namespace mozilla

#endif // GonkVideoDecoderManager_h_
