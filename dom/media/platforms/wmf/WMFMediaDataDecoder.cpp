/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WMFMediaDataDecoder.h"
#include "VideoUtils.h"
#include "WMFUtils.h"
#include "nsTArray.h"

#include "mozilla/Logging.h"

extern PRLogModuleInfo* GetPDMLog();
#define LOG(...) MOZ_LOG(GetPDMLog(), mozilla::LogLevel::Debug, (__VA_ARGS__))

namespace mozilla {

WMFMediaDataDecoder::WMFMediaDataDecoder(MFTManager* aMFTManager,
                                         FlushableTaskQueue* aTaskQueue,
                                         MediaDataDecoderCallback* aCallback)
  : mTaskQueue(aTaskQueue)
  , mCallback(aCallback)
  , mMFTManager(aMFTManager)
  , mMonitor("WMFMediaDataDecoder")
  , mIsFlushing(false)
  , mIsShutDown(false)
{
}

WMFMediaDataDecoder::~WMFMediaDataDecoder()
{
}

RefPtr<MediaDataDecoder::InitPromise>
WMFMediaDataDecoder::Init()
{
  MOZ_ASSERT(!mIsShutDown);
  return InitPromise::CreateAndResolve(mMFTManager->GetType(), __func__);
}

nsresult
WMFMediaDataDecoder::Shutdown()
{
  MOZ_DIAGNOSTIC_ASSERT(!mIsShutDown);

  if (mTaskQueue) {
    nsCOMPtr<nsIRunnable> runnable =
      NS_NewRunnableMethod(this, &WMFMediaDataDecoder::ProcessShutdown);
    mTaskQueue->Dispatch(runnable.forget());
  } else {
    ProcessShutdown();
  }
  mIsShutDown = true;
  return NS_OK;
}

void
WMFMediaDataDecoder::ProcessShutdown()
{
  if (mMFTManager) {
    mMFTManager->Shutdown();
    mMFTManager = nullptr;
    if (!mRecordedError && mHasSuccessfulOutput) {
      //SendTelemetry(S_OK);
    }
  }
}

// Inserts data into the decoder's pipeline.
nsresult
WMFMediaDataDecoder::Input(MediaRawData* aSample)
{
  MOZ_ASSERT(mCallback->OnReaderTaskQueue());
  MOZ_DIAGNOSTIC_ASSERT(!mIsShutDown);

  nsCOMPtr<nsIRunnable> runnable =
    NS_NewRunnableMethodWithArg<RefPtr<MediaRawData>>(
      this,
      &WMFMediaDataDecoder::ProcessDecode,
      RefPtr<MediaRawData>(aSample));
  mTaskQueue->Dispatch(runnable.forget());
  return NS_OK;
}

void
WMFMediaDataDecoder::ProcessDecode(MediaRawData* aSample)
{
  {
    MonitorAutoLock mon(mMonitor);
    if (mIsFlushing) {
      // Skip sample, to be released by runnable.
      return;
    }
  }

  HRESULT hr = mMFTManager->Input(aSample);
  if (FAILED(hr)) {
    NS_WARNING("MFTManager rejected sample");
    mCallback->Error();
    if (!mRecordedError) {
      //SendTelemetry(hr);
      mRecordedError = true;
    }
    return;
  }

  mLastStreamOffset = aSample->mOffset;

  ProcessOutput();
}

void
WMFMediaDataDecoder::ProcessOutput()
{
  RefPtr<MediaData> output;
  HRESULT hr = S_OK;
  while (SUCCEEDED(hr = mMFTManager->Output(mLastStreamOffset, output)) &&
         output) {
    mHasSuccessfulOutput = true;
    mCallback->Output(output);
  }
  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    if (mTaskQueue->IsEmpty()) {
      mCallback->InputExhausted();
    }
  } else if (FAILED(hr)) {
    NS_WARNING("WMFMediaDataDecoder failed to output data");
    mCallback->Error();
    if (!mRecordedError) {
      //SendTelemetry(hr);
      mRecordedError = true;
    }
  }
}

void
WMFMediaDataDecoder::ProcessFlush()
{
  if (mMFTManager) {
    mMFTManager->Flush();
  }
  MonitorAutoLock mon(mMonitor);
  mIsFlushing = false;
  mon.NotifyAll();
}

nsresult
WMFMediaDataDecoder::Flush()
{
  MOZ_ASSERT(mCallback->OnReaderTaskQueue());
  MOZ_DIAGNOSTIC_ASSERT(!mIsShutDown);

  nsCOMPtr<nsIRunnable> runnable =
    NS_NewRunnableMethod(this, &WMFMediaDataDecoder::ProcessFlush);
  MonitorAutoLock mon(mMonitor);
  mIsFlushing = true;
  mTaskQueue->Dispatch(runnable.forget());
  while (mIsFlushing) {
    mon.Wait();
  }
  return NS_OK;
}

void
WMFMediaDataDecoder::ProcessDrain()
{
  bool isFlushing;
  {
    MonitorAutoLock mon(mMonitor);
    isFlushing = mIsFlushing;
  }
  if (!isFlushing && mMFTManager) {
    // Order the decoder to drain...
    mMFTManager->Drain();
    // Then extract all available output.
    ProcessOutput();
  }
  mCallback->DrainComplete();
}

nsresult
WMFMediaDataDecoder::Drain()
{
  MOZ_ASSERT(mCallback->OnReaderTaskQueue());
  MOZ_DIAGNOSTIC_ASSERT(!mIsShutDown);

  nsCOMPtr<nsIRunnable> runnable =
  NS_NewRunnableMethod(this, &WMFMediaDataDecoder::ProcessDrain);
  mTaskQueue->Dispatch(runnable.forget());
  return NS_OK;
}

bool
WMFMediaDataDecoder::IsHardwareAccelerated(nsACString& aFailureReason) const {
  MOZ_ASSERT(!mIsShutDown);

  return mMFTManager && mMFTManager->IsHardwareAccelerated(aFailureReason);
}

} // namespace mozilla
