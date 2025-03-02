/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "VideoUtils.h"

#include "mozilla/Preferences.h"
#include "mozilla/Base64.h"
#include "mozilla/TaskQueue.h"
#include "mozilla/Telemetry.h"
#include "mozilla/Function.h"

#include "MediaResource.h"
#include "TimeUnits.h"
#include "nsMathUtils.h"
#include "nsSize.h"
#include "VorbisUtils.h"
#include "ImageContainer.h"
#include "mozilla/SharedThreadPool.h"
#include "nsIRandomGenerator.h"
#include "nsIServiceManager.h"
#include "nsServiceManagerUtils.h"
#include "nsIConsoleService.h"
#include "nsThreadUtils.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsContentTypeParser.h"

#include <stdint.h>

namespace mozilla {

using layers::PlanarYCbCrImage;

static inline CheckedInt64 SaferMultDiv(int64_t aValue, uint32_t aMul, uint32_t aDiv) {
  int64_t major = aValue / aDiv;
  int64_t remainder = aValue % aDiv;
  return CheckedInt64(remainder) * aMul / aDiv + major * aMul;
}

// Converts from number of audio frames to microseconds, given the specified
// audio rate.
CheckedInt64 FramesToUsecs(int64_t aFrames, uint32_t aRate) {
  return SaferMultDiv(aFrames, USECS_PER_S, aRate);
}

media::TimeUnit FramesToTimeUnit(int64_t aFrames, uint32_t aRate) {
  int64_t major = aFrames / aRate;
  int64_t remainder = aFrames % aRate;
  return media::TimeUnit::FromMicroseconds(major) * USECS_PER_S +
    (media::TimeUnit::FromMicroseconds(remainder) * USECS_PER_S) / aRate;
}

// Converts from microseconds to number of audio frames, given the specified
// audio rate.
CheckedInt64 UsecsToFrames(int64_t aUsecs, uint32_t aRate) {
  return SaferMultDiv(aUsecs, aRate, USECS_PER_S);
}

// Format TimeUnit as number of frames at given rate.
CheckedInt64 TimeUnitToFrames(const media::TimeUnit& aTime, uint32_t aRate) {
  return UsecsToFrames(aTime.ToMicroseconds(), aRate);
}

nsresult SecondsToUsecs(double aSeconds, int64_t& aOutUsecs) {
  if (aSeconds * double(USECS_PER_S) > INT64_MAX) {
    return NS_ERROR_FAILURE;
  }
  aOutUsecs = int64_t(aSeconds * double(USECS_PER_S));
  return NS_OK;
}

static int32_t ConditionDimension(float aValue)
{
  // This will exclude NaNs and too-big values.
  if (aValue > 1.0 && aValue <= INT32_MAX)
    return int32_t(NS_round(aValue));
  return 0;
}

void ScaleDisplayByAspectRatio(nsIntSize& aDisplay, float aAspectRatio)
{
  if (aAspectRatio > 1.0) {
    // Increase the intrinsic width
    aDisplay.width = ConditionDimension(aAspectRatio * aDisplay.width);
  } else {
    // Increase the intrinsic height
    aDisplay.height = ConditionDimension(aDisplay.height / aAspectRatio);
  }
}

static int64_t BytesToTime(int64_t offset, int64_t length, int64_t durationUs) {
  NS_ASSERTION(length > 0, "Must have positive length");
  double r = double(offset) / double(length);
  if (r > 1.0)
    r = 1.0;
  return int64_t(double(durationUs) * r);
}

media::TimeIntervals GetEstimatedBufferedTimeRanges(mozilla::MediaResource* aStream,
                                                    int64_t aDurationUsecs)
{
  media::TimeIntervals buffered;
  // Nothing to cache if the media takes 0us to play.
  if (aDurationUsecs <= 0 || !aStream)
    return buffered;

  // Special case completely cached files.  This also handles local files.
  if (aStream->IsDataCachedToEndOfResource(0)) {
    buffered +=
      media::TimeInterval(media::TimeUnit::FromMicroseconds(0),
                          media::TimeUnit::FromMicroseconds(aDurationUsecs));
    return buffered;
  }

  int64_t totalBytes = aStream->GetLength();

  // If we can't determine the total size, pretend that we have nothing
  // buffered. This will put us in a state of eternally-low-on-undecoded-data
  // which is not great, but about the best we can do.
  if (totalBytes <= 0)
    return buffered;

  int64_t startOffset = aStream->GetNextCachedData(0);
  while (startOffset >= 0) {
    int64_t endOffset = aStream->GetCachedDataEnd(startOffset);
    // Bytes [startOffset..endOffset] are cached.
    NS_ASSERTION(startOffset >= 0, "Integer underflow in GetBuffered");
    NS_ASSERTION(endOffset >= 0, "Integer underflow in GetBuffered");

    int64_t startUs = BytesToTime(startOffset, totalBytes, aDurationUsecs);
    int64_t endUs = BytesToTime(endOffset, totalBytes, aDurationUsecs);
    if (startUs != endUs) {
      buffered +=
        media::TimeInterval(media::TimeUnit::FromMicroseconds(startUs),

                              media::TimeUnit::FromMicroseconds(endUs));
    }
    startOffset = aStream->GetNextCachedData(endOffset);
  }
  return buffered;
}

int DownmixAudioToStereo(mozilla::AudioDataValue* buffer,
                         int channels, uint32_t frames)
{
  int outChannels;
  outChannels = 2;
#ifdef MOZ_SAMPLE_TYPE_FLOAT32
  // Downmix matrix. Per-row normalization 1 for rows 3,4 and 2 for rows 5-8.
  static const float dmatrix[6][8][2]= {
      /*3*/{{0.5858f,0},{0.4142f,0.4142f},{0,     0.5858f}},
      /*4*/{{0.4226f,0},{0,      0.4226f},{0.366f,0.2114f},{0.2114f,0.366f}},
      /*5*/{{0.6510f,0},{0.4600f,0.4600f},{0,     0.6510f},{0.5636f,0.3254f},{0.3254f,0.5636f}},
      /*6*/{{0.5290f,0},{0.3741f,0.3741f},{0,     0.5290f},{0.4582f,0.2645f},{0.2645f,0.4582f},{0.3741f,0.3741f}},
      /*7*/{{0.4553f,0},{0.3220f,0.3220f},{0,     0.4553f},{0.3943f,0.2277f},{0.2277f,0.3943f},{0.2788f,0.2788f},{0.3220f,0.3220f}},
      /*8*/{{0.3886f,0},{0.2748f,0.2748f},{0,     0.3886f},{0.3366f,0.1943f},{0.1943f,0.3366f},{0.3366f,0.1943f},{0.1943f,0.3366f},{0.2748f,0.2748f}},
  };
  // Re-write the buffer with downmixed data
  for (uint32_t i = 0; i < frames; i++) {
    float sampL = 0.0;
    float sampR = 0.0;
    for (int j = 0; j < channels; j++) {
      sampL+=buffer[i*channels+j]*dmatrix[channels-3][j][0];
      sampR+=buffer[i*channels+j]*dmatrix[channels-3][j][1];
    }
    buffer[i*outChannels]=sampL;
    buffer[i*outChannels+1]=sampR;
  }
#else
  // Downmix matrix. Per-row normalization 1 for rows 3,4 and 2 for rows 5-8.
  // Coefficients in Q14.
  static const int16_t dmatrix[6][8][2]= {
      /*3*/{{9598, 0},{6786,6786},{0,   9598}},
      /*4*/{{6925, 0},{0,   6925},{5997,3462},{3462,5997}},
      /*5*/{{10663,0},{7540,7540},{0,  10663},{9234,5331},{5331,9234}},
      /*6*/{{8668, 0},{6129,6129},{0,   8668},{7507,4335},{4335,7507},{6129,6129}},
      /*7*/{{7459, 0},{5275,5275},{0,   7459},{6460,3731},{3731,6460},{4568,4568},{5275,5275}},
      /*8*/{{6368, 0},{4502,4502},{0,   6368},{5514,3184},{3184,5514},{5514,3184},{3184,5514},{4502,4502}}
  };
  // Re-write the buffer with downmixed data
  for (uint32_t i = 0; i < frames; i++) {
    int32_t sampL = 0;
    int32_t sampR = 0;
    for (int j = 0; j < channels; j++) {
      sampL+=buffer[i*channels+j]*dmatrix[channels-3][j][0];
      sampR+=buffer[i*channels+j]*dmatrix[channels-3][j][1];
    }
    sampL = (sampL + 8192)>>14;
    buffer[i*outChannels] = static_cast<mozilla::AudioDataValue>(MOZ_CLIP_TO_15(sampL));
    sampR = (sampR + 8192)>>14;
    buffer[i*outChannels+1] = static_cast<mozilla::AudioDataValue>(MOZ_CLIP_TO_15(sampR));
  }
#endif
  return outChannels;
}

void DownmixStereoToMono(mozilla::AudioDataValue* aBuffer,
                         uint32_t aFrames)
{
  MOZ_ASSERT(aBuffer);
  const int channels = 2;
  for (uint32_t fIdx = 0; fIdx < aFrames; ++fIdx) {
#ifdef MOZ_SAMPLE_TYPE_FLOAT32
    float sample = 0.0;
#else
    int sample = 0;
#endif
    // The sample of the buffer would be interleaved.
    sample = (aBuffer[fIdx*channels] + aBuffer[fIdx*channels + 1]) * 0.5;
    aBuffer[fIdx*channels] = aBuffer[fIdx*channels + 1] = sample;
  }
}

bool
IsVideoContentType(const nsCString& aContentType)
{
  NS_NAMED_LITERAL_CSTRING(video, "video");
  if (FindInReadable(video, aContentType)) {
    return true;
  }
  return false;
}

bool
IsValidVideoRegion(const nsIntSize& aFrame, const nsIntRect& aPicture,
                   const nsIntSize& aDisplay)
{
  return
    aFrame.width <= PlanarYCbCrImage::MAX_DIMENSION &&
    aFrame.height <= PlanarYCbCrImage::MAX_DIMENSION &&
    aFrame.width * aFrame.height <= MAX_VIDEO_WIDTH * MAX_VIDEO_HEIGHT &&
    aFrame.width * aFrame.height != 0 &&
    aPicture.width <= PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.x < PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.x + aPicture.width < PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.height <= PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.y < PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.y + aPicture.height < PlanarYCbCrImage::MAX_DIMENSION &&
    aPicture.width * aPicture.height <= MAX_VIDEO_WIDTH * MAX_VIDEO_HEIGHT &&
    aPicture.width * aPicture.height != 0 &&
    aDisplay.width <= PlanarYCbCrImage::MAX_DIMENSION &&
    aDisplay.height <= PlanarYCbCrImage::MAX_DIMENSION &&
    aDisplay.width * aDisplay.height <= MAX_VIDEO_WIDTH * MAX_VIDEO_HEIGHT &&
    aDisplay.width * aDisplay.height != 0;
}

already_AddRefed<SharedThreadPool> GetMediaThreadPool(MediaThreadType aType)
{
  const char *name;
  switch (aType) {
    case MediaThreadType::PLATFORM_DECODER:
      name = "MediaPDecoder";
      break;
    default:
      MOZ_ASSERT(false);
    case MediaThreadType::PLAYBACK:
      name = "MediaPlayback";
      break;
  }
  return SharedThreadPool::
    Get(nsDependentCString(name),
        Preferences::GetUint("media.num-decode-threads", 12));
}

bool
ExtractH264CodecDetails(const nsAString& aCodec,
                        int16_t& aProfile,
                        int16_t& aLevel)
{
  // H.264 codecs parameters have a type defined as avcN.PPCCLL, where
  // N = avc type. avc3 is avcc with SPS & PPS implicit (within stream)
  // PP = profile_idc, CC = constraint_set flags, LL = level_idc.
  // We ignore the constraint_set flags, as it's not clear from any
  // documentation what constraints the platform decoders support.
  // See http://blog.pearce.org.nz/2013/11/what-does-h264avc1-codecs-parameters.html
  // for more details.
  if (aCodec.Length() != strlen("avc1.PPCCLL")) {
    return false;
  }

  // Verify the codec starts with "avc1." or "avc3.".
  const nsAString& sample = Substring(aCodec, 0, 5);
  if (!sample.EqualsASCII("avc1.") && !sample.EqualsASCII("avc3.")) {
    return false;
  }

  // Extract the profile_idc and level_idc.
  nsresult rv = NS_OK;
  aProfile = PromiseFlatString(Substring(aCodec, 5, 2)).ToInteger(&rv, 16);
  NS_ENSURE_SUCCESS(rv, false);

  aLevel = PromiseFlatString(Substring(aCodec, 9, 2)).ToInteger(&rv, 16);
  NS_ENSURE_SUCCESS(rv, false);

  if (aLevel == 9) {
    aLevel = H264_LEVEL_1_b;
  } else if (aLevel <= 5) {
    aLevel *= 10;
  }

  return true;
}

nsresult
GenerateRandomName(nsCString& aOutSalt, uint32_t aLength)
{
  nsresult rv;
  nsCOMPtr<nsIRandomGenerator> rg =
    do_GetService("@mozilla.org/security/random-generator;1", &rv);
  if (NS_FAILED(rv)) return rv;

  // For each three bytes of random data we will get four bytes of ASCII.
  const uint32_t requiredBytesLength =
    static_cast<uint32_t>((aLength + 3) / 4 * 3);

  uint8_t* buffer;
  rv = rg->GenerateRandomBytes(requiredBytesLength, &buffer);
  if (NS_FAILED(rv)) return rv;

  nsAutoCString temp;
  nsDependentCSubstring randomData(reinterpret_cast<const char*>(buffer),
                                   requiredBytesLength);
  rv = Base64Encode(randomData, temp);
  NS_Free(buffer);
  buffer = nullptr;
  if (NS_FAILED (rv)) return rv;

  aOutSalt = temp;
  return NS_OK;
}

nsresult
GenerateRandomPathName(nsCString& aOutSalt, uint32_t aLength)
{
  nsresult rv = GenerateRandomName(aOutSalt, aLength);
  if (NS_FAILED(rv)) return rv;

  // Base64 characters are alphanumeric (a-zA-Z0-9) and '+' and '/', so we need
  // to replace illegal characters -- notably '/'
  aOutSalt.ReplaceChar(FILE_PATH_SEPARATOR FILE_ILLEGAL_CHARACTERS, '_');
  return NS_OK;
}

already_AddRefed<TaskQueue>
CreateMediaDecodeTaskQueue()
{
  RefPtr<TaskQueue> queue = new TaskQueue(
    GetMediaThreadPool(MediaThreadType::PLATFORM_DECODER));
  return queue.forget();
}

already_AddRefed<FlushableTaskQueue>
CreateFlushableMediaDecodeTaskQueue()
{
  RefPtr<FlushableTaskQueue> queue = new FlushableTaskQueue(
    GetMediaThreadPool(MediaThreadType::PLATFORM_DECODER));
  return queue.forget();
}

void
SimpleTimer::Cancel() {
  if (mTimer) {
#ifdef DEBUG
    nsCOMPtr<nsIEventTarget> target;
    mTimer->GetTarget(getter_AddRefs(target));
    nsCOMPtr<nsIThread> thread(do_QueryInterface(target));
    MOZ_ASSERT(NS_GetCurrentThread() == thread);
#endif
    mTimer->Cancel();
    mTimer = nullptr;
  }
  mTask = nullptr;
}

NS_IMETHODIMP
SimpleTimer::Notify(nsITimer *timer) {
  RefPtr<SimpleTimer> deathGrip(this);
  if (mTask) {
    mTask->Run();
    mTask = nullptr;
  }
  return NS_OK;
}

nsresult
SimpleTimer::Init(nsIRunnable* aTask, uint32_t aTimeoutMs, nsIThread* aTarget)
{
  nsresult rv;

  // Get target thread first, so we don't have to cancel the timer if it fails.
  nsCOMPtr<nsIThread> target;
  if (aTarget) {
    target = aTarget;
  } else {
    rv = NS_GetMainThread(getter_AddRefs(target));
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  nsCOMPtr<nsITimer> timer = do_CreateInstance(NS_TIMER_CONTRACTID, &rv);
  if (NS_FAILED(rv)) {
    return rv;
  }
  // Note: set target before InitWithCallback in case the timer fires before
  // we change the event target.
  rv = timer->SetTarget(aTarget);
  if (NS_FAILED(rv)) {
    timer->Cancel();
    return rv;
  }
  rv = timer->InitWithCallback(this, aTimeoutMs, nsITimer::TYPE_ONE_SHOT);
  if (NS_FAILED(rv)) {
    return rv;
  }

  mTimer = timer.forget();
  mTask = aTask;
  return NS_OK;
}

NS_IMPL_ISUPPORTS(SimpleTimer, nsITimerCallback)

already_AddRefed<SimpleTimer>
SimpleTimer::Create(nsIRunnable* aTask, uint32_t aTimeoutMs, nsIThread* aTarget)
{
  RefPtr<SimpleTimer> t(new SimpleTimer());
  if (NS_FAILED(t->Init(aTask, aTimeoutMs, aTarget))) {
    return nullptr;
  }
  return t.forget();
}

void
LogToBrowserConsole(const nsAString& aMsg)
{
  if (!NS_IsMainThread()) {
    nsAutoString msg(aMsg);
    nsCOMPtr<nsIRunnable> task =
      NS_NewRunnableFunction([msg]() { LogToBrowserConsole(msg); });
    NS_DispatchToMainThread(task.forget(), NS_DISPATCH_NORMAL);
    return;
  }
  nsCOMPtr<nsIConsoleService> console(
    do_GetService("@mozilla.org/consoleservice;1"));
  if (!console) {
    NS_WARNING("Failed to log message to console.");
    return;
  }
  nsAutoString msg(aMsg);
  console->LogStringMessage(msg.get());
}

bool
IsAACCodecString(const nsAString& aCodec)
{
  return
    aCodec.EqualsLiteral("mp4a.40.2") || // MPEG4 AAC-LC
    aCodec.EqualsLiteral("mp4a.40.5") || // MPEG4 HE-AAC
    aCodec.EqualsLiteral("mp4a.67"); // MPEG2 AAC-LC}
}

bool
ParseCodecsString(const nsAString& aCodecs, nsTArray<nsString>& aOutCodecs)
{
  aOutCodecs.Clear();
  bool expectMoreTokens = false;
  nsCharSeparatedTokenizer tokenizer(aCodecs, ',');
  while (tokenizer.hasMoreTokens()) {
    const nsSubstring& token = tokenizer.nextToken();
    expectMoreTokens = tokenizer.separatorAfterCurrentToken();
    aOutCodecs.AppendElement(token);
  }
  if (expectMoreTokens) {
    // Last codec name was empty
    return false;
  }
  return true;
}

static bool
CheckContentType(const nsAString& aContentType,
                 mozilla::Function<bool(const nsAString&)> aSubtypeFilter,
                 mozilla::Function<bool(const nsAString&)> aCodecFilter)
{
  nsContentTypeParser parser(aContentType);
  nsAutoString mimeType;
  nsresult rv = parser.GetType(mimeType);
  if (NS_FAILED(rv) || !aSubtypeFilter(mimeType)) {
    return false;
  }

  nsString codecsStr;
  parser.GetParameter("codecs", codecsStr);
  nsTArray<nsString> codecs;
  if (!ParseCodecsString(codecsStr, codecs)) {
    return false;
  }
  for (const nsString& codec : codecs) {
    if (!aCodecFilter(codec)) {
      return false;
    }
  }
  return true;
}

bool
IsH264ContentType(const nsAString& aContentType)
{
  return CheckContentType(aContentType,
    [](const nsAString& type) {
      return type.EqualsLiteral("video/mp4");
    },
    [](const nsAString& codec) {
      int16_t profile = 0;
      int16_t level = 0;
      return ExtractH264CodecDetails(codec, profile, level);
    }
  );
}

bool
IsAACContentType(const nsAString& aContentType)
{
  return CheckContentType(aContentType,
    [](const nsAString& type) {
      return type.EqualsLiteral("audio/mp4") ||
             type.EqualsLiteral("audio/x-m4a");
    },
    [](const nsAString& codec) {
      return codec.EqualsLiteral("mp4a.40.2") || // MPEG4 AAC-LC
             codec.EqualsLiteral("mp4a.40.5") || // MPEG4 HE-AAC
             codec.EqualsLiteral("mp4a.67");     // MPEG2 AAC-LC
    });
}

} // end namespace mozilla
