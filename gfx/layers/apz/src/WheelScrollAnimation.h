/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_WheelScrollAnimation_h_
#define mozilla_layers_WheelScrollAnimation_h_

#include "AsyncPanZoomAnimation.h"
#include "AsyncPanZoomController.h"
#include "AsyncScrollBase.h"
#include "gfxPrefs.h"

namespace mozilla {
namespace layers {

class AsyncPanZoomController;

class WheelScrollAnimation
  : public AsyncPanZoomAnimation,
    public AsyncScrollBase
{
public:
  WheelScrollAnimation(AsyncPanZoomController& aApzc, const nsPoint& aInitialPosition);

  bool DoSample(FrameMetrics& aFrameMetrics, const TimeDuration& aDelta) override;
  void Update(TimeStamp aTime, nsPoint aDelta, const nsSize& aCurrentVelocity);

  WheelScrollAnimation* AsWheelScrollAnimation() override {
    return this;
  }

private:
  void InitPreferences(TimeStamp aTime);

private:
  AsyncPanZoomController& mApzc;
  nsPoint mFinalDestination;
};

} // namespace layers
} // namespace mozilla

#endif // mozilla_layers_WheelScrollAnimation_h_
