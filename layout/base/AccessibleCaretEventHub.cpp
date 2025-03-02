/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AccessibleCaretEventHub.h"

#include "AccessibleCaretLogger.h"
#include "AccessibleCaretManager.h"
#include "Layers.h"
#include "gfxPrefs.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/TextEvents.h"
#include "mozilla/TouchEvents.h"
#include "nsDocShell.h"
#include "nsFocusManager.h"
#include "nsFrameSelection.h"
#include "nsITimer.h"
#include "nsPresContext.h"

namespace mozilla {

#undef AC_LOG
#define AC_LOG(message, ...)                                                   \
  AC_LOG_BASE("AccessibleCaretEventHub (%p): " message, this, ##__VA_ARGS__);

#undef AC_LOGV
#define AC_LOGV(message, ...)                                                  \
  AC_LOGV_BASE("AccessibleCaretEventHub (%p): " message, this, ##__VA_ARGS__);

NS_IMPL_ISUPPORTS(AccessibleCaretEventHub, nsIReflowObserver, nsIScrollObserver,
                  nsISelectionListener, nsISupportsWeakReference);

// -----------------------------------------------------------------------------
// NoActionState
//
class AccessibleCaretEventHub::NoActionState
  : public AccessibleCaretEventHub::State
{
public:
  NS_IMPL_STATE_UTILITIES(NoActionState)

  virtual nsEventStatus OnPress(AccessibleCaretEventHub* aContext,
                                const nsPoint& aPoint,
                                int32_t aTouchId) override
  {
    nsEventStatus rv = nsEventStatus_eIgnore;

    if (NS_SUCCEEDED(aContext->mManager->PressCaret(aPoint))) {
      aContext->SetState(aContext->PressCaretState());
      rv = nsEventStatus_eConsumeNoDefault;
    } else {
      aContext->SetState(aContext->PressNoCaretState());
    }

    aContext->mPressPoint = aPoint;
    aContext->mActiveTouchId = aTouchId;

    return rv;
  }

  virtual void OnScrollStart(AccessibleCaretEventHub* aContext) override
  {
    aContext->mManager->OnScrollStart();
    aContext->SetState(aContext->ScrollState());
  }

  virtual void OnScrollPositionChanged(AccessibleCaretEventHub* aContext) override
  {
    aContext->mManager->OnScrollPositionChanged();
  }

  virtual void OnSelectionChanged(AccessibleCaretEventHub* aContext,
                                  nsIDOMDocument* aDoc, nsISelection* aSel,
                                  int16_t aReason) override
  {
    aContext->mManager->OnSelectionChanged(aDoc, aSel, aReason);
  }

  virtual void OnBlur(AccessibleCaretEventHub* aContext,
                      bool aIsLeavingDocument) override
  {
    aContext->mManager->OnBlur();
  }

  virtual void OnReflow(AccessibleCaretEventHub* aContext) override
  {
    aContext->mManager->OnReflow();
  }

  virtual void Enter(AccessibleCaretEventHub* aContext) override
  {
    aContext->mPressPoint = nsPoint(NS_UNCONSTRAINEDSIZE, NS_UNCONSTRAINEDSIZE);
    aContext->mActiveTouchId = kInvalidTouchId;
  }
};

// -----------------------------------------------------------------------------
// PressCaretState: Always consume the event since we've pressed on the caret.
//
class AccessibleCaretEventHub::PressCaretState
  : public AccessibleCaretEventHub::State
{
public:
  NS_IMPL_STATE_UTILITIES(PressCaretState)

  virtual nsEventStatus OnMove(AccessibleCaretEventHub* aContext,
                               const nsPoint& aPoint) override
  {
    if (aContext->MoveDistanceIsLarge(aPoint)) {
      if (NS_SUCCEEDED(aContext->mManager->DragCaret(aPoint))) {
        aContext->SetState(aContext->DragCaretState());
      }
    }

    return nsEventStatus_eConsumeNoDefault;
  }

  virtual nsEventStatus OnRelease(AccessibleCaretEventHub* aContext) override
  {
    aContext->mManager->ReleaseCaret();
    aContext->mManager->TapCaret(aContext->mPressPoint);
    aContext->SetState(aContext->NoActionState());

    return nsEventStatus_eConsumeNoDefault;
  }

  virtual nsEventStatus OnLongTap(AccessibleCaretEventHub* aContext,
                                  const nsPoint& aPoint) override
  {
    return nsEventStatus_eConsumeNoDefault;
  }
};

// -----------------------------------------------------------------------------
// DragCaretState: Always consume the event since we've pressed on the caret.
//
class AccessibleCaretEventHub::DragCaretState
  : public AccessibleCaretEventHub::State
{
public:
  NS_IMPL_STATE_UTILITIES(DragCaretState)

  virtual nsEventStatus OnMove(AccessibleCaretEventHub* aContext,
                               const nsPoint& aPoint) override
  {
    aContext->mManager->DragCaret(aPoint);

    return nsEventStatus_eConsumeNoDefault;
  }

  virtual nsEventStatus OnRelease(AccessibleCaretEventHub* aContext) override
  {
    aContext->mManager->ReleaseCaret();
    aContext->SetState(aContext->NoActionState());

    return nsEventStatus_eConsumeNoDefault;
  }
};

// -----------------------------------------------------------------------------
// PressNoCaretState
//
class AccessibleCaretEventHub::PressNoCaretState
  : public AccessibleCaretEventHub::State
{
public:
  NS_IMPL_STATE_UTILITIES(PressNoCaretState)

  virtual nsEventStatus OnMove(AccessibleCaretEventHub* aContext,
                               const nsPoint& aPoint) override
  {
    if (aContext->MoveDistanceIsLarge(aPoint)) {
      aContext->SetState(aContext->NoActionState());
    }

    return nsEventStatus_eIgnore;
  }

  virtual nsEventStatus OnRelease(AccessibleCaretEventHub* aContext) override
  {
    aContext->SetState(aContext->NoActionState());

    return nsEventStatus_eIgnore;
  }

  virtual nsEventStatus OnLongTap(AccessibleCaretEventHub* aContext,
                                  const nsPoint& aPoint) override
  {
    aContext->SetState(aContext->LongTapState());

    return aContext->GetState()->OnLongTap(aContext, aPoint);
  }

  virtual void OnScrollStart(AccessibleCaretEventHub* aContext) override
  {
    aContext->mManager->OnScrollStart();
    aContext->SetState(aContext->ScrollState());
  }

  virtual void OnBlur(AccessibleCaretEventHub* aContext,
                      bool aIsLeavingDocument) override
  {
    aContext->mManager->OnBlur();
    if (aIsLeavingDocument) {
      aContext->SetState(aContext->NoActionState());
    }
  }

  virtual void OnSelectionChanged(AccessibleCaretEventHub* aContext,
                                  nsIDOMDocument* aDoc, nsISelection* aSel,
                                  int16_t aReason) override
  {
    aContext->mManager->OnSelectionChanged(aDoc, aSel, aReason);
  }

  virtual void OnReflow(AccessibleCaretEventHub* aContext) override
  {
    aContext->mManager->OnReflow();
  }

  virtual void Enter(AccessibleCaretEventHub* aContext) override
  {
    aContext->LaunchLongTapInjector();
  }

  virtual void Leave(AccessibleCaretEventHub* aContext) override
  {
    aContext->CancelLongTapInjector();
  }
};

// -----------------------------------------------------------------------------
// ScrollState
//
class AccessibleCaretEventHub::ScrollState
  : public AccessibleCaretEventHub::State
{
public:
  NS_IMPL_STATE_UTILITIES(ScrollState)

  virtual void OnScrollEnd(AccessibleCaretEventHub* aContext) override
  {
    aContext->SetState(aContext->PostScrollState());
  }

  virtual void OnBlur(AccessibleCaretEventHub* aContext,
                      bool aIsLeavingDocument) override
  {
    aContext->mManager->OnBlur();
    if (aIsLeavingDocument) {
      aContext->SetState(aContext->NoActionState());
    }
  }
};

// -----------------------------------------------------------------------------
// PostScrollState: In this state, we are waiting for another APZ start, press
// event, or momentum wheel scroll.
//
class AccessibleCaretEventHub::PostScrollState
  : public AccessibleCaretEventHub::State
{
public:
  NS_IMPL_STATE_UTILITIES(PostScrollState)

  virtual nsEventStatus OnPress(AccessibleCaretEventHub* aContext,
                                const nsPoint& aPoint,
                                int32_t aTouchId) override
  {
    aContext->mManager->OnScrollEnd();
    aContext->SetState(aContext->NoActionState());

    return aContext->GetState()->OnPress(aContext, aPoint, aTouchId);
  }

  virtual void OnScrollStart(AccessibleCaretEventHub* aContext) override
  {
    aContext->SetState(aContext->ScrollState());
  }

  virtual void OnScrollEnd(AccessibleCaretEventHub* aContext) override
  {
    aContext->mManager->OnScrollEnd();
    aContext->SetState(aContext->NoActionState());
  }

  virtual void OnScrolling(AccessibleCaretEventHub* aContext) override
  {
    // Momentum scroll by wheel event.
    aContext->LaunchScrollEndInjector();
  }

  virtual void OnBlur(AccessibleCaretEventHub* aContext,
                      bool aIsLeavingDocument) override
  {
    aContext->mManager->OnBlur();
    if (aIsLeavingDocument) {
      aContext->SetState(aContext->NoActionState());
    }
  }

  virtual void Enter(AccessibleCaretEventHub* aContext) override
  {
    // Launch the injector to leave PostScrollState.
    aContext->LaunchScrollEndInjector();
  }

  virtual void Leave(AccessibleCaretEventHub* aContext) override
  {
    aContext->CancelScrollEndInjector();
  }
};

// -----------------------------------------------------------------------------
// LongTapState
//
class AccessibleCaretEventHub::LongTapState
  : public AccessibleCaretEventHub::State
{
public:
  NS_IMPL_STATE_UTILITIES(LongTapState)

  virtual nsEventStatus OnLongTap(AccessibleCaretEventHub* aContext,
                                  const nsPoint& aPoint) override
  {
    nsEventStatus rv = nsEventStatus_eIgnore;

    if (NS_SUCCEEDED(aContext->mManager->SelectWordOrShortcut(aPoint))) {
      rv = nsEventStatus_eConsumeNoDefault;
    }

    aContext->SetState(aContext->NoActionState());

    return rv;
  }

  virtual void OnReflow(AccessibleCaretEventHub* aContext) override
  {
    aContext->mManager->OnReflow();
  }
};

// -----------------------------------------------------------------------------
// Implementation of AccessibleCaretEventHub methods
//
AccessibleCaretEventHub::State*
AccessibleCaretEventHub::GetState() const
{
  return mState;
}

void
AccessibleCaretEventHub::SetState(State* aState)
{
  MOZ_ASSERT(aState);

  AC_LOG("%s -> %s", mState->Name(), aState->Name());

  mState->Leave(this);
  mState = aState;
  mState->Enter(this);
}

NS_IMPL_STATE_CLASS_GETTER(NoActionState)
NS_IMPL_STATE_CLASS_GETTER(PressCaretState)
NS_IMPL_STATE_CLASS_GETTER(DragCaretState)
NS_IMPL_STATE_CLASS_GETTER(PressNoCaretState)
NS_IMPL_STATE_CLASS_GETTER(ScrollState)
NS_IMPL_STATE_CLASS_GETTER(PostScrollState)
NS_IMPL_STATE_CLASS_GETTER(LongTapState)

AccessibleCaretEventHub::AccessibleCaretEventHub()
{
}

AccessibleCaretEventHub::~AccessibleCaretEventHub()
{
}

void
AccessibleCaretEventHub::Init(nsIPresShell* aPresShell)
{
  if (mInitialized || !aPresShell || !aPresShell->GetCanvasFrame() ||
      !aPresShell->GetCanvasFrame()->GetCustomContentContainer()) {
    return;
  }

  // Without nsAutoScriptBlocker, the script might be run after constructing
  // mFirstCaret in AccessibleCaretManager's constructor, which might destructs
  // the whole frame tree. Therefore we'll fail to construct mSecondCaret
  // because we cannot get root frame or canvas frame from mPresShell to inject
  // anonymous content. To avoid that, we protect Init() by nsAutoScriptBlocker.
  // To reproduce, run "./mach crashtest layout/base/crashtests/897852.html"
  // without the following scriptBlocker.
  nsAutoScriptBlocker scriptBlocker;

  mPresShell = aPresShell;

  nsPresContext* presContext = mPresShell->GetPresContext();
  MOZ_ASSERT(presContext, "PresContext should be given in PresShell::Init()");

  nsIDocShell* docShell = presContext->GetDocShell();
  if (!docShell) {
    return;
  }

#if defined(MOZ_WIDGET_GONK)
  mUseAsyncPanZoom = mPresShell->AsyncPanZoomEnabled();
#endif

  docShell->AddWeakReflowObserver(this);
  docShell->AddWeakScrollObserver(this);

  mDocShell = static_cast<nsDocShell*>(docShell);

  mLongTapInjectorTimer = do_CreateInstance("@mozilla.org/timer;1");
  mScrollEndInjectorTimer = do_CreateInstance("@mozilla.org/timer;1");

  mManager = MakeUnique<AccessibleCaretManager>(mPresShell);

  mInitialized = true;
}

void
AccessibleCaretEventHub::Terminate()
{
  if (!mInitialized) {
    return;
  }

  RefPtr<nsDocShell> docShell(mDocShell.get());
  if (docShell) {
    docShell->RemoveWeakReflowObserver(this);
    docShell->RemoveWeakScrollObserver(this);
  }

  if (mLongTapInjectorTimer) {
    mLongTapInjectorTimer->Cancel();
  }

  if (mScrollEndInjectorTimer) {
    mScrollEndInjectorTimer->Cancel();
  }

  mManager = nullptr;
  mPresShell = nullptr;
  mInitialized = false;
}

nsEventStatus
AccessibleCaretEventHub::HandleEvent(WidgetEvent* aEvent)
{
  nsEventStatus status = nsEventStatus_eIgnore;

  if (!mInitialized) {
    return status;
  }

  switch (aEvent->mClass) {
  case eMouseEventClass:
    status = HandleMouseEvent(aEvent->AsMouseEvent());
    break;

  case eWheelEventClass:
    status = HandleWheelEvent(aEvent->AsWheelEvent());
    break;

  case eTouchEventClass:
    status = HandleTouchEvent(aEvent->AsTouchEvent());
    break;

  case eKeyboardEventClass:
    status = HandleKeyboardEvent(aEvent->AsKeyboardEvent());
    break;

  default:
    break;
  }

  return status;
}

nsEventStatus
AccessibleCaretEventHub::HandleMouseEvent(WidgetMouseEvent* aEvent)
{
  nsEventStatus rv = nsEventStatus_eIgnore;

  if (aEvent->button != WidgetMouseEvent::eLeftButton) {
    return rv;
  }

  int32_t id = (mActiveTouchId == kInvalidTouchId ?
                kDefaultTouchId : mActiveTouchId);
  nsPoint point = GetMouseEventPosition(aEvent);

  switch (aEvent->mMessage) {
  case eMouseDown:
    AC_LOGV("Before eMouseDown, state: %s", mState->Name());
    rv = mState->OnPress(this, point, id);
    AC_LOGV("After eMouseDown, state: %s, consume: %d",
            mState->Name(), rv);
    break;

  case eMouseMove:
    AC_LOGV("Before eMouseMove, state: %s", mState->Name());
    rv = mState->OnMove(this, point);
    AC_LOGV("After eMouseMove, state: %s, consume: %d", mState->Name(), rv);
    break;

  case eMouseUp:
    AC_LOGV("Before eMouseUp, state: %s", mState->Name());
    rv = mState->OnRelease(this);
    AC_LOGV("After eMouseUp, state: %s, consume: %d", mState->Name(),
            rv);
    break;

  case eMouseLongTap:
    AC_LOGV("Before eMouseLongTap, state: %s", mState->Name());
    rv = mState->OnLongTap(this, point);
    AC_LOGV("After eMouseLongTap, state: %s, consume: %d", mState->Name(),
            rv);
    break;

  default:
    break;
  }

  return rv;
}

nsEventStatus
AccessibleCaretEventHub::HandleWheelEvent(WidgetWheelEvent* aEvent)
{
  switch (aEvent->mMessage) {
  case eWheel:
    AC_LOGV("eWheel, isMomentum %d, state: %s", aEvent->isMomentum,
            mState->Name());
    mState->OnScrolling(this);
    break;

  case eWheelOperationStart:
    AC_LOGV("eWheelOperationStart, state: %s", mState->Name());
    mState->OnScrollStart(this);
    break;

  case eWheelOperationEnd:
    AC_LOGV("eWheelOperationEnd, state: %s", mState->Name());
    mState->OnScrollEnd(this);
    break;

  default:
    break;
  }

  // Always ignore this event since we only want to know scroll start and scroll
  // end, not to consume it.
  return nsEventStatus_eIgnore;
}

nsEventStatus
AccessibleCaretEventHub::HandleTouchEvent(WidgetTouchEvent* aEvent)
{
  nsEventStatus rv = nsEventStatus_eIgnore;

  int32_t id = (mActiveTouchId == kInvalidTouchId ?
                aEvent->touches[0]->Identifier() : mActiveTouchId);
  nsPoint point = GetTouchEventPosition(aEvent, id);

  switch (aEvent->mMessage) {
  case eTouchStart:
    AC_LOGV("Before eTouchStart, state: %s", mState->Name());
    rv = mState->OnPress(this, point, id);
    AC_LOGV("After eTouchStart, state: %s, consume: %d", mState->Name(), rv);
    break;

  case eTouchMove:
    AC_LOGV("Before eTouchMove, state: %s", mState->Name());
    rv = mState->OnMove(this, point);
    AC_LOGV("After eTouchMove, state: %s, consume: %d", mState->Name(), rv);
    break;

  case eTouchEnd:
    AC_LOGV("Before eTouchEnd, state: %s", mState->Name());
    rv = mState->OnRelease(this);
    AC_LOGV("After eTouchEnd, state: %s, consume: %d", mState->Name(), rv);
    break;

  case eTouchCancel:
    AC_LOGV("Before eTouchCancel, state: %s", mState->Name());
    rv = mState->OnRelease(this);
    AC_LOGV("After eTouchCancel, state: %s, consume: %d", mState->Name(),
            rv);
    break;

  default:
    break;
  }

  return rv;
}

nsEventStatus
AccessibleCaretEventHub::HandleKeyboardEvent(WidgetKeyboardEvent* aEvent)
{
  switch (aEvent->mMessage) {
  case eKeyUp:
  case eKeyDown:
  case eKeyPress:
    mManager->OnKeyboardEvent();
    break;

  default:
    break;
  }

  return nsEventStatus_eIgnore;
}

bool
AccessibleCaretEventHub::MoveDistanceIsLarge(const nsPoint& aPoint) const
{
  nsPoint delta = aPoint - mPressPoint;
  return NS_hypot(delta.x, delta.y) >
         nsPresContext::AppUnitsPerCSSPixel() * kMoveStartToleranceInPixel;
}

void
AccessibleCaretEventHub::LaunchLongTapInjector()
{
  if (mUseAsyncPanZoom) {
    return;
  }

  if (!mLongTapInjectorTimer) {
    return;
  }

  int32_t longTapDelay = gfxPrefs::UiClickHoldContextMenusDelay();
  mLongTapInjectorTimer->InitWithFuncCallback(FireLongTap, this, longTapDelay,
                                              nsITimer::TYPE_ONE_SHOT);
}

void
AccessibleCaretEventHub::CancelLongTapInjector()
{
  if (mUseAsyncPanZoom) {
    return;
  }

  if (!mLongTapInjectorTimer) {
    return;
  }

  mLongTapInjectorTimer->Cancel();
}

/* static */ void
AccessibleCaretEventHub::FireLongTap(nsITimer* aTimer,
                                     void* aAccessibleCaretEventHub)
{
  auto self = static_cast<AccessibleCaretEventHub*>(aAccessibleCaretEventHub);
  self->mState->OnLongTap(self, self->mPressPoint);
}

NS_IMETHODIMP
AccessibleCaretEventHub::Reflow(DOMHighResTimeStamp aStart,
                                DOMHighResTimeStamp aEnd)
{
  if (!mInitialized) {
    return NS_OK;
  }

  AC_LOG("%s, state: %s", __FUNCTION__, mState->Name());
  mState->OnReflow(this);
  return NS_OK;
}

NS_IMETHODIMP
AccessibleCaretEventHub::ReflowInterruptible(DOMHighResTimeStamp aStart,
                                             DOMHighResTimeStamp aEnd)
{
  if (!mInitialized) {
    return NS_OK;
  }

  return Reflow(aStart, aEnd);
}

void
AccessibleCaretEventHub::AsyncPanZoomStarted()
{
  if (!mInitialized) {
    return;
  }

  AC_LOG("%s, state: %s", __FUNCTION__, mState->Name());
  mState->OnScrollStart(this);
}

void
AccessibleCaretEventHub::AsyncPanZoomStopped()
{
  if (!mInitialized) {
    return;
  }

  AC_LOG("%s, state: %s", __FUNCTION__, mState->Name());
  mState->OnScrollEnd(this);
}

void
AccessibleCaretEventHub::ScrollPositionChanged()
{
  if (!mInitialized) {
    return;
  }

  AC_LOG("%s, state: %s", __FUNCTION__, mState->Name());
  mState->OnScrollPositionChanged(this);
}

void
AccessibleCaretEventHub::LaunchScrollEndInjector()
{
  if (!mScrollEndInjectorTimer) {
    return;
  }

  mScrollEndInjectorTimer->InitWithFuncCallback(
    FireScrollEnd, this, kScrollEndTimerDelay, nsITimer::TYPE_ONE_SHOT);
}

void
AccessibleCaretEventHub::CancelScrollEndInjector()
{
  if (!mScrollEndInjectorTimer) {
    return;
  }

  mScrollEndInjectorTimer->Cancel();
}

/* static */ void
AccessibleCaretEventHub::FireScrollEnd(nsITimer* aTimer,
                                       void* aAccessibleCaretEventHub)
{
  auto self = static_cast<AccessibleCaretEventHub*>(aAccessibleCaretEventHub);
  self->mState->OnScrollEnd(self);
}

nsresult
AccessibleCaretEventHub::NotifySelectionChanged(nsIDOMDocument* aDoc,
                                                nsISelection* aSel,
                                                int16_t aReason)
{
  if (!mInitialized) {
    return NS_OK;
  }

  AC_LOG("%s, state: %s, reason: %d", __FUNCTION__, mState->Name(), aReason);
  mState->OnSelectionChanged(this, aDoc, aSel, aReason);
  return NS_OK;
}

void
AccessibleCaretEventHub::NotifyBlur(bool aIsLeavingDocument)
{
  if (!mInitialized) {
    return;
  }

  AC_LOG("%s, state: %s", __FUNCTION__, mState->Name());
  mState->OnBlur(this, aIsLeavingDocument);
}

nsPoint
AccessibleCaretEventHub::GetTouchEventPosition(WidgetTouchEvent* aEvent,
                                               int32_t aIdentifier) const
{
  for (dom::Touch* touch : aEvent->touches) {
    if (touch->Identifier() == aIdentifier) {
      LayoutDeviceIntPoint touchIntPoint = touch->mRefPoint;

      // Get event coordinate relative to root frame.
      nsIFrame* rootFrame = mPresShell->GetRootFrame();
      return nsLayoutUtils::GetEventCoordinatesRelativeTo(aEvent, touchIntPoint,
                                                          rootFrame);
    }
  }
  return nsPoint(NS_UNCONSTRAINEDSIZE, NS_UNCONSTRAINEDSIZE);
}

nsPoint
AccessibleCaretEventHub::GetMouseEventPosition(WidgetMouseEvent* aEvent) const
{
  LayoutDeviceIntPoint mouseIntPoint = aEvent->AsGUIEvent()->refPoint;

  // Get event coordinate relative to root frame.
  nsIFrame* rootFrame = mPresShell->GetRootFrame();
  return nsLayoutUtils::GetEventCoordinatesRelativeTo(aEvent, mouseIntPoint,
                                                      rootFrame);
}

} // namespace mozilla
