/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(MediaSourceDemuxer_h_)
#define MediaSourceDemuxer_h_

#include "mozilla/Atomics.h"
#include "mozilla/Maybe.h"
#include "mozilla/Monitor.h"
#include "mozilla/TaskQueue.h"

#include "MediaDataDemuxer.h"
#include "MediaDecoderReader.h"
#include "MediaResource.h"
#include "MediaSource.h"
#include "TrackBuffersManager.h"

namespace mozilla {

class MediaSourceTrackDemuxer;

class MediaSourceDemuxer : public MediaDataDemuxer
{
public:
  explicit MediaSourceDemuxer();

  RefPtr<InitPromise> Init() override;

  bool HasTrackType(TrackInfo::TrackType aType) const override;

  uint32_t GetNumberTracks(TrackInfo::TrackType aType) const override;

  already_AddRefed<MediaTrackDemuxer> GetTrackDemuxer(TrackInfo::TrackType aType,
                                                              uint32_t aTrackNumber) override;

  bool IsSeekable() const override;

  UniquePtr<EncryptionInfo> GetCrypto() override;

  bool ShouldComputeStartTime() const override { return false; }

  void NotifyDataArrived() override;

  /* interface for TrackBuffersManager */
  void AttachSourceBuffer(TrackBuffersManager* aSourceBuffer);
  void DetachSourceBuffer(TrackBuffersManager* aSourceBuffer);
  TaskQueue* GetTaskQueue() { return mTaskQueue; }

  // Returns a string describing the state of the MediaSource internal
  // buffered data. Used for debugging purposes.
  void GetMozDebugReaderData(nsAString& aString);

  void AddSizeOfResources(MediaSourceDecoder::ResourceSizes* aSizes);

  // Gap allowed between frames.
  static const media::TimeUnit EOS_FUZZ;

private:
  ~MediaSourceDemuxer();
  friend class MediaSourceTrackDemuxer;
  // Scan source buffers and update information.
  bool ScanSourceBuffersForContent();
  RefPtr<InitPromise> AttemptInit();
  TrackBuffersManager* GetManager(TrackInfo::TrackType aType);
  TrackInfo* GetTrackInfo(TrackInfo::TrackType);
  void DoAttachSourceBuffer(TrackBuffersManager* aSourceBuffer);
  void DoDetachSourceBuffer(TrackBuffersManager* aSourceBuffer);
  bool OnTaskQueue()
  {
    return !GetTaskQueue() || GetTaskQueue()->IsCurrentThreadIn();
  }

  RefPtr<TaskQueue> mTaskQueue;
  nsTArray<RefPtr<MediaSourceTrackDemuxer>> mDemuxers;

  nsTArray<RefPtr<TrackBuffersManager>> mSourceBuffers;

  MozPromiseHolder<InitPromise> mInitPromise;
  bool mInitDone;

  // Monitor to protect members below across multiple threads.
  mutable Monitor mMonitor;
  RefPtr<TrackBuffersManager> mAudioTrack;
  RefPtr<TrackBuffersManager> mVideoTrack;
  MediaInfo mInfo;
};

class MediaSourceTrackDemuxer : public MediaTrackDemuxer
{
public:
  MediaSourceTrackDemuxer(MediaSourceDemuxer* aParent,
                          TrackInfo::TrackType aType,
                          TrackBuffersManager* aManager);

  UniquePtr<TrackInfo> GetInfo() const override;

  RefPtr<SeekPromise> Seek(media::TimeUnit aTime) override;

  RefPtr<SamplesPromise> GetSamples(int32_t aNumSamples = 1) override;

  void Reset() override;

  nsresult GetNextRandomAccessPoint(media::TimeUnit* aTime) override;

  RefPtr<SkipAccessPointPromise> SkipToNextRandomAccessPoint(media::TimeUnit aTimeThreshold) override;

  media::TimeIntervals GetBuffered() override;

  void BreakCycles() override;

  bool GetSamplesMayBlock() const override
  {
    return false;
  }

private:
  RefPtr<SeekPromise> DoSeek(media::TimeUnit aTime);
  RefPtr<SamplesPromise> DoGetSamples(int32_t aNumSamples);
  RefPtr<SkipAccessPointPromise> DoSkipToNextRandomAccessPoint(media::TimeUnit aTimeThreadshold);
  already_AddRefed<MediaRawData> GetSample(DemuxerFailureReason& aFailure);
  // Return the timestamp of the next keyframe after mLastSampleIndex.
  media::TimeUnit GetNextRandomAccessPoint();

  RefPtr<MediaSourceDemuxer> mParent;
  RefPtr<TrackBuffersManager> mManager;
  TrackInfo::TrackType mType;
  // Monitor protecting members below accessed from multiple threads.
  Monitor mMonitor;
  media::TimeUnit mNextRandomAccessPoint;
};

} // namespace mozilla

#endif
