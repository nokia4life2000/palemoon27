/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(GonkAudioDecoderManager_h_)
#define GonkAudioDecoderManager_h_

#include "mozilla/RefPtr.h"
#include "GonkMediaDataDecoder.h"

using namespace android;

namespace android {
struct MOZ_EXPORT ALooper;
class MOZ_EXPORT MediaBuffer;
} // namespace android

namespace mozilla {

class GonkAudioDecoderManager : public GonkDecoderManager {
typedef android::MediaCodecProxy MediaCodecProxy;
public:
  GonkAudioDecoderManager(const AudioInfo& aConfig);

  virtual ~GonkAudioDecoderManager() override;

  RefPtr<InitPromise> Init(MediaDataDecoderCallback* aCallback) override;

  nsresult Input(MediaRawData* aSample) override;

  nsresult Output(int64_t aStreamOffset,
                          RefPtr<MediaData>& aOutput) override;

  nsresult Flush() override;

  bool HasQueuedSample() override;

private:
  bool InitMediaCodecProxy(MediaDataDecoderCallback* aCallback);

  nsresult CreateAudioData(int64_t aStreamOffset,
                              AudioData** aOutData);

  void ReleaseAudioBuffer();

  int64_t mLastDecodedTime;

  uint32_t mAudioChannels;
  uint32_t mAudioRate;
  const uint32_t mAudioProfile;
  nsTArray<uint8_t> mUserData;

  MediaDataDecoderCallback*  mReaderCallback;
  android::MediaBuffer* mAudioBuffer;
  android::sp<ALooper> mLooper;

  // This monitor protects mQueueSample.
  Monitor mMonitor;

  // An queue with the MP4 samples which are waiting to be sent into OMX.
  // If an element is an empty MP4Sample, that menas EOS. There should not
  // any sample be queued after EOS.
  nsTArray<RefPtr<MediaRawData>> mQueueSample;
};

} // namespace mozilla

#endif // GonkAudioDecoderManager_h_
