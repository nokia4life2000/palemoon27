/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsComboboxControlFrame.h"

#include "gfxUtils.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/PathHelpers.h"
#include "nsCOMPtr.h"
#include "nsFocusManager.h"
#include "nsFormControlFrame.h"
#include "nsGkAtoms.h"
#include "nsCSSAnonBoxes.h"
#include "nsHTMLParts.h"
#include "nsIFormControl.h"
#include "nsNameSpaceManager.h"
#include "nsIListControlFrame.h"
#include "nsPIDOMWindow.h"
#include "nsIPresShell.h"
#include "nsContentList.h"
#include "nsView.h"
#include "nsViewManager.h"
#include "nsIDOMEventListener.h"
#include "nsIDOMNode.h"
#include "nsISelectControlFrame.h"
#include "nsContentUtils.h"
#include "nsIDocument.h"
#include "nsIScrollableFrame.h"
#include "nsListControlFrame.h"
#include "nsAutoPtr.h"
#include "nsStyleSet.h"
#include "nsNodeInfoManager.h"
#include "nsContentCreatorFunctions.h"
#include "nsLayoutUtils.h"
#include "nsDisplayList.h"
#include "nsITheme.h"
#include "nsThemeConstants.h"
#include "nsRenderingContext.h"
#include "mozilla/Likely.h"
#include <algorithm>
#include "nsTextNode.h"
#include "nsIContentInlines.h"
#include "mozilla/AsyncEventDispatcher.h"
#include "mozilla/EventStates.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/unused.h"
#include "gfx2DGlue.h"

#ifdef XP_WIN
#define COMBOBOX_ROLLUP_CONSUME_EVENT 0
#else
#define COMBOBOX_ROLLUP_CONSUME_EVENT 1
#endif

using namespace mozilla;
using namespace mozilla::gfx;

NS_IMETHODIMP
nsComboboxControlFrame::RedisplayTextEvent::Run()
{
  if (mControlFrame)
    mControlFrame->HandleRedisplayTextEvent();
  return NS_OK;
}

class nsPresState;

#define FIX_FOR_BUG_53259

// Drop down list event management.
// The combo box uses the following strategy for managing the drop-down list.
// If the combo box or its arrow button is clicked on the drop-down list is displayed
// If mouse exits the combo box with the drop-down list displayed the drop-down list
// is asked to capture events
// The drop-down list will capture all events including mouse down and up and will always
// return with ListWasSelected method call regardless of whether an item in the list was
// actually selected.
// The ListWasSelected code will turn off mouse-capture for the drop-down list.
// The drop-down list does not explicitly set capture when it is in the drop-down mode.


/**
 * Helper class that listens to the combo boxes button. If the button is pressed the
 * combo box is toggled to open or close. this is used by Accessibility which presses
 * that button Programmatically.
 */
class nsComboButtonListener : public nsIDOMEventListener
{
private:
  virtual ~nsComboButtonListener() {}

public:
  NS_DECL_ISUPPORTS

  NS_IMETHOD HandleEvent(nsIDOMEvent*) override
  {
    mComboBox->ShowDropDown(!mComboBox->IsDroppedDown());
    return NS_OK;
  }

  explicit nsComboButtonListener(nsComboboxControlFrame* aCombobox)
  {
    mComboBox = aCombobox;
  }

  nsComboboxControlFrame* mComboBox;
};

NS_IMPL_ISUPPORTS(nsComboButtonListener,
                  nsIDOMEventListener)

// static class data member for Bug 32920
nsComboboxControlFrame* nsComboboxControlFrame::sFocused = nullptr;

nsContainerFrame*
NS_NewComboboxControlFrame(nsIPresShell* aPresShell, nsStyleContext* aContext, nsFrameState aStateFlags)
{
  nsComboboxControlFrame* it = new (aPresShell) nsComboboxControlFrame(aContext);

  if (it) {
    // set the state flags (if any are provided)
    it->AddStateBits(aStateFlags);
  }

  return it;
}

NS_IMPL_FRAMEARENA_HELPERS(nsComboboxControlFrame)

//-----------------------------------------------------------
// Reflow Debugging Macros
// These let us "see" how many reflow counts are happening
//-----------------------------------------------------------
#ifdef DO_REFLOW_COUNTER

#define MAX_REFLOW_CNT 1024
static int32_t gTotalReqs    = 0;;
static int32_t gTotalReflows = 0;;
static int32_t gReflowControlCntRQ[MAX_REFLOW_CNT];
static int32_t gReflowControlCnt[MAX_REFLOW_CNT];
static int32_t gReflowInx = -1;

#define REFLOW_COUNTER() \
  if (mReflowId > -1) \
    gReflowControlCnt[mReflowId]++;

#define REFLOW_COUNTER_REQUEST() \
  if (mReflowId > -1) \
    gReflowControlCntRQ[mReflowId]++;

#define REFLOW_COUNTER_DUMP(__desc) \
  if (mReflowId > -1) {\
    gTotalReqs    += gReflowControlCntRQ[mReflowId];\
    gTotalReflows += gReflowControlCnt[mReflowId];\
    printf("** Id:%5d %s RF: %d RQ: %d   %d/%d  %5.2f\n", \
           mReflowId, (__desc), \
           gReflowControlCnt[mReflowId], \
           gReflowControlCntRQ[mReflowId],\
           gTotalReflows, gTotalReqs, float(gTotalReflows)/float(gTotalReqs)*100.0f);\
  }

#define REFLOW_COUNTER_INIT() \
  if (gReflowInx < MAX_REFLOW_CNT) { \
    gReflowInx++; \
    mReflowId = gReflowInx; \
    gReflowControlCnt[mReflowId] = 0; \
    gReflowControlCntRQ[mReflowId] = 0; \
  } else { \
    mReflowId = -1; \
  }

// reflow messages
#define REFLOW_DEBUG_MSG(_msg1) printf((_msg1))
#define REFLOW_DEBUG_MSG2(_msg1, _msg2) printf((_msg1), (_msg2))
#define REFLOW_DEBUG_MSG3(_msg1, _msg2, _msg3) printf((_msg1), (_msg2), (_msg3))
#define REFLOW_DEBUG_MSG4(_msg1, _msg2, _msg3, _msg4) printf((_msg1), (_msg2), (_msg3), (_msg4))

#else //-------------

#define REFLOW_COUNTER_REQUEST()
#define REFLOW_COUNTER()
#define REFLOW_COUNTER_DUMP(__desc)
#define REFLOW_COUNTER_INIT()

#define REFLOW_DEBUG_MSG(_msg)
#define REFLOW_DEBUG_MSG2(_msg1, _msg2)
#define REFLOW_DEBUG_MSG3(_msg1, _msg2, _msg3)
#define REFLOW_DEBUG_MSG4(_msg1, _msg2, _msg3, _msg4)


#endif

//------------------------------------------
// This is for being VERY noisy
//------------------------------------------
#ifdef DO_VERY_NOISY
#define REFLOW_NOISY_MSG(_msg1) printf((_msg1))
#define REFLOW_NOISY_MSG2(_msg1, _msg2) printf((_msg1), (_msg2))
#define REFLOW_NOISY_MSG3(_msg1, _msg2, _msg3) printf((_msg1), (_msg2), (_msg3))
#define REFLOW_NOISY_MSG4(_msg1, _msg2, _msg3, _msg4) printf((_msg1), (_msg2), (_msg3), (_msg4))
#else
#define REFLOW_NOISY_MSG(_msg)
#define REFLOW_NOISY_MSG2(_msg1, _msg2)
#define REFLOW_NOISY_MSG3(_msg1, _msg2, _msg3)
#define REFLOW_NOISY_MSG4(_msg1, _msg2, _msg3, _msg4)
#endif

//------------------------------------------
// Displays value in pixels or twips
//------------------------------------------
#ifdef DO_PIXELS
#define PX(__v) __v / 15
#else
#define PX(__v) __v
#endif

//------------------------------------------------------
//-- Done with macros
//------------------------------------------------------

nsComboboxControlFrame::nsComboboxControlFrame(nsStyleContext* aContext)
  : nsBlockFrame(aContext)
  , mDisplayFrame(nullptr)
  , mButtonFrame(nullptr)
  , mDropdownFrame(nullptr)
  , mListControlFrame(nullptr)
  , mDisplayISize(0)
  , mRecentSelectedIndex(NS_SKIP_NOTIFY_INDEX)
  , mDisplayedIndex(-1)
  , mLastDropDownBeforeScreenBCoord(nscoord_MIN)
  , mLastDropDownAfterScreenBCoord(nscoord_MIN)
  , mDroppedDown(false)
  , mInRedisplayText(false)
  , mDelayedShowDropDown(false)
{
  REFLOW_COUNTER_INIT()
}

//--------------------------------------------------------------
nsComboboxControlFrame::~nsComboboxControlFrame()
{
  REFLOW_COUNTER_DUMP("nsCCF");
}

//--------------------------------------------------------------

NS_QUERYFRAME_HEAD(nsComboboxControlFrame)
  NS_QUERYFRAME_ENTRY(nsIComboboxControlFrame)
  NS_QUERYFRAME_ENTRY(nsIFormControlFrame)
  NS_QUERYFRAME_ENTRY(nsIAnonymousContentCreator)
  NS_QUERYFRAME_ENTRY(nsISelectControlFrame)
  NS_QUERYFRAME_ENTRY(nsIStatefulFrame)
NS_QUERYFRAME_TAIL_INHERITING(nsBlockFrame)

#ifdef ACCESSIBILITY
a11y::AccType
nsComboboxControlFrame::AccessibleType()
{
  return a11y::eHTMLComboboxType;
}
#endif

void
nsComboboxControlFrame::SetFocus(bool aOn, bool aRepaint)
{
  nsWeakFrame weakFrame(this);
  if (aOn) {
    nsListControlFrame::ComboboxFocusSet();
    sFocused = this;
    if (mDelayedShowDropDown) {
      ShowDropDown(true); // might destroy us
      if (!weakFrame.IsAlive()) {
        return;
      }
    }
  } else {
    sFocused = nullptr;
    mDelayedShowDropDown = false;
    if (mDroppedDown) {
      mListControlFrame->ComboboxFinish(mDisplayedIndex); // might destroy us
      if (!weakFrame.IsAlive()) {
        return;
      }
    }
    // May delete |this|.
    mListControlFrame->FireOnChange();
  }

  if (!weakFrame.IsAlive()) {
    return;
  }

  // This is needed on a temporary basis. It causes the focus
  // rect to be drawn. This is much faster than ReResolvingStyle
  // Bug 32920
  InvalidateFrame();
}

void
nsComboboxControlFrame::ShowPopup(bool aShowPopup)
{
  nsView* view = mDropdownFrame->GetView();
  nsViewManager* viewManager = view->GetViewManager();

  if (aShowPopup) {
    nsRect rect = mDropdownFrame->GetRect();
    rect.x = rect.y = 0;
    viewManager->ResizeView(view, rect);
    viewManager->SetViewVisibility(view, nsViewVisibility_kShow);
  } else {
    viewManager->SetViewVisibility(view, nsViewVisibility_kHide);
    nsRect emptyRect(0, 0, 0, 0);
    viewManager->ResizeView(view, emptyRect);
  }

  // fire a popup dom event
  nsEventStatus status = nsEventStatus_eIgnore;
  WidgetMouseEvent event(true, aShowPopup ? eXULPopupShowing : eXULPopupHiding,
                         nullptr, WidgetMouseEvent::eReal);

  nsCOMPtr<nsIPresShell> shell = PresContext()->GetPresShell();
  if (shell)
    shell->HandleDOMEventWithTarget(mContent, &event, &status);
}

bool
nsComboboxControlFrame::ShowList(bool aShowList)
{
  nsView* view = mDropdownFrame->GetView();
  if (aShowList) {
    NS_ASSERTION(!view->HasWidget(),
                 "We shouldn't have a widget before we need to display the popup");

    // Create the widget for the drop-down list
    view->GetViewManager()->SetViewFloating(view, true);

    nsWidgetInitData widgetData;
    widgetData.mWindowType  = eWindowType_popup;
    widgetData.mBorderStyle = eBorderStyle_default;
    view->CreateWidgetForPopup(&widgetData);
  } else {
    nsIWidget* widget = view->GetWidget();
    if (widget) {
      // We must do this before ShowPopup in case it destroys us (bug 813442).
      widget->CaptureRollupEvents(this, false);
    }
  }

  nsWeakFrame weakFrame(this);
  ShowPopup(aShowList);  // might destroy us
  if (!weakFrame.IsAlive()) {
    return false;
  }

  mDroppedDown = aShowList;
  nsIWidget* widget = view->GetWidget();
  if (mDroppedDown) {
    // The listcontrol frame will call back to the nsComboboxControlFrame's
    // ListWasSelected which will stop the capture.
    mListControlFrame->AboutToDropDown();
    mListControlFrame->CaptureMouseEvents(true);
    if (widget) {
      widget->CaptureRollupEvents(this, true);
    }
  } else {
    if (widget) {
      view->DestroyWidget();
    }
  }

  return weakFrame.IsAlive();
}

class nsResizeDropdownAtFinalPosition final
  : public nsIReflowCallback, public nsRunnable
{
public:
  explicit nsResizeDropdownAtFinalPosition(nsComboboxControlFrame* aFrame)
    : mFrame(aFrame)
  {
    MOZ_COUNT_CTOR(nsResizeDropdownAtFinalPosition);
  }

protected:
  ~nsResizeDropdownAtFinalPosition()
  {
    MOZ_COUNT_DTOR(nsResizeDropdownAtFinalPosition);
  }

public:
  virtual bool ReflowFinished() override
  {
    Run();
    NS_RELEASE_THIS();
    return false;
  }

  virtual void ReflowCallbackCanceled() override
  {
    NS_RELEASE_THIS();
  }

  NS_IMETHODIMP Run() override
  {
    if (mFrame.IsAlive()) {
      static_cast<nsComboboxControlFrame*>(mFrame.GetFrame())->
        AbsolutelyPositionDropDown();
    }
    return NS_OK;
  }

  nsWeakFrame mFrame;
};

void
nsComboboxControlFrame::ReflowDropdown(nsPresContext*  aPresContext,
                                       const nsHTMLReflowState& aReflowState)
{
  // All we want out of it later on, really, is the block size of a row, so we
  // don't even need to cache mDropdownFrame's ascent or anything.  If we don't
  // need to reflow it, just bail out here.
  if (!aReflowState.ShouldReflowAllKids() &&
      !NS_SUBTREE_DIRTY(mDropdownFrame)) {
    return;
  }

  // XXXbz this will, for small-block-size dropdowns, have extra space
  // on the appropriate edge for the scrollbar we don't show... but
  // that's the best we can do here for now.
  WritingMode wm = mDropdownFrame->GetWritingMode();
  LogicalSize availSize = aReflowState.AvailableSize(wm);
  availSize.BSize(wm) = NS_UNCONSTRAINEDSIZE;
  nsHTMLReflowState kidReflowState(aPresContext, aReflowState, mDropdownFrame,
                                   availSize);

  // If the dropdown's intrinsic inline size is narrower than our
  // specified inline size, then expand it out.  We want our border-box
  // inline size to end up the same as the dropdown's so account for
  // both sets of mComputedBorderPadding.
  nscoord forcedISize = aReflowState.ComputedISize() +
    aReflowState.ComputedLogicalBorderPadding().IStartEnd(wm) -
    kidReflowState.ComputedLogicalBorderPadding().IStartEnd(wm);
  kidReflowState.SetComputedISize(std::max(kidReflowState.ComputedISize(),
                                         forcedISize));

  // ensure we start off hidden
  if (GetStateBits() & NS_FRAME_FIRST_REFLOW) {
    nsView* view = mDropdownFrame->GetView();
    nsViewManager* viewManager = view->GetViewManager();
    viewManager->SetViewVisibility(view, nsViewVisibility_kHide);
    nsRect emptyRect(0, 0, 0, 0);
    viewManager->ResizeView(view, emptyRect);
  }

  // Allow the child to move/size/change-visibility its view if it's currently
  // dropped down
  int32_t flags = mDroppedDown ? 0
                               : NS_FRAME_NO_MOVE_FRAME |
                                 NS_FRAME_NO_VISIBILITY |
                                 NS_FRAME_NO_SIZE_VIEW;

  //XXX Can this be different from the dropdown's writing mode?
  // That would be odd!
  // Note that we don't need to pass the true frame position or container size
  // to ReflowChild or FinishReflowChild here; it will be positioned as needed
  // by AbsolutelyPositionDropDown().
  WritingMode outerWM = GetWritingMode();
  const nsSize dummyContainerSize;
  nsHTMLReflowMetrics desiredSize(aReflowState);
  nsReflowStatus ignoredStatus;
  ReflowChild(mDropdownFrame, aPresContext, desiredSize,
              kidReflowState, outerWM, LogicalPoint(outerWM),
              dummyContainerSize, flags, ignoredStatus);

   // Set the child's width and height to its desired size
  FinishReflowChild(mDropdownFrame, aPresContext, desiredSize, &kidReflowState,
                    outerWM, LogicalPoint(outerWM), dummyContainerSize, flags);
}

nsPoint
nsComboboxControlFrame::GetCSSTransformTranslation()
{
  nsIFrame* frame = this;
  bool is3DTransform = false;
  Matrix transform;
  while (frame) {
    nsIFrame* parent;
    Matrix4x4 ctm = frame->GetTransformMatrix(nullptr, &parent);
    Matrix matrix;
    if (ctm.Is2D(&matrix)) {
      transform = transform * matrix;
    } else {
      is3DTransform = true;
      break;
    }
    frame = parent;
  }
  nsPoint translation;
  if (!is3DTransform && !transform.HasNonTranslation()) {
    nsPresContext* pc = PresContext();
    // To get the translation introduced only by transforms we subtract the
    // regular non-transform translation.
    nsRootPresContext* rootPC = pc->GetRootPresContext();
    if (rootPC) {
      int32_t apd = pc->AppUnitsPerDevPixel();
      translation.x = NSFloatPixelsToAppUnits(transform._31, apd);
      translation.y = NSFloatPixelsToAppUnits(transform._32, apd);
      translation -= GetOffsetToCrossDoc(rootPC->PresShell()->GetRootFrame());
    }
  }
  return translation;
}

class nsAsyncRollup : public nsRunnable
{
public:
  explicit nsAsyncRollup(nsComboboxControlFrame* aFrame) : mFrame(aFrame) {}
  NS_IMETHODIMP Run()
  {
    if (mFrame.IsAlive()) {
      static_cast<nsComboboxControlFrame*>(mFrame.GetFrame())
        ->RollupFromList();
    }
    return NS_OK;
  }
  nsWeakFrame mFrame;
};

class nsAsyncResize : public nsRunnable
{
public:
  explicit nsAsyncResize(nsComboboxControlFrame* aFrame) : mFrame(aFrame) {}
  NS_IMETHODIMP Run()
  {
    if (mFrame.IsAlive()) {
      nsComboboxControlFrame* combo =
        static_cast<nsComboboxControlFrame*>(mFrame.GetFrame());
      static_cast<nsListControlFrame*>(combo->mDropdownFrame)->
        SetSuppressScrollbarUpdate(true);
      nsCOMPtr<nsIPresShell> shell = mFrame->PresContext()->PresShell();
      shell->FrameNeedsReflow(combo->mDropdownFrame, nsIPresShell::eResize,
                              NS_FRAME_IS_DIRTY);
      shell->FlushPendingNotifications(Flush_Layout);
      if (mFrame.IsAlive()) {
        combo = static_cast<nsComboboxControlFrame*>(mFrame.GetFrame());
        static_cast<nsListControlFrame*>(combo->mDropdownFrame)->
          SetSuppressScrollbarUpdate(false);
        if (combo->mDelayedShowDropDown) {
          combo->ShowDropDown(true);
        }
      }
    }
    return NS_OK;
  }
  nsWeakFrame mFrame;
};

void
nsComboboxControlFrame::GetAvailableDropdownSpace(WritingMode aWM,
                                                  nscoord* aBefore,
                                                  nscoord* aAfter,
                                                  LogicalPoint* aTranslation)
{
  // Note: At first glance, it appears that you could simply get the
  // absolute bounding box for the dropdown list by first getting its
  // view, then getting the view's nsIWidget, then asking the nsIWidget
  // for its AbsoluteBounds.
  // The problem with this approach, is that the dropdown list's bcoord
  // location can change based on whether the dropdown is placed after
  // or before the display frame.  The approach taken here is to get the
  // absolute position of the display frame and use its location to
  // determine if the dropdown will go offscreen.

  // Normal frame geometry (eg GetOffsetTo, mRect) doesn't include transforms.
  // In the special case that our transform is only a 2D translation we
  // introduce this hack so that the dropdown will show up in the right place.
  // Use null container size when converting a vector from logical to physical.
  const nsSize nullContainerSize;
  *aTranslation = LogicalPoint(aWM, GetCSSTransformTranslation(),
                               nullContainerSize);
  *aBefore = 0;
  *aAfter = 0;

  nsRect screen = nsFormControlFrame::GetUsableScreenRect(PresContext());
  nsSize containerSize = screen.Size();
  LogicalRect logicalScreen(aWM, screen, containerSize);
  if (mLastDropDownAfterScreenBCoord == nscoord_MIN) {
    LogicalRect thisScreenRect(aWM, GetScreenRectInAppUnits(),
                               containerSize);
    mLastDropDownAfterScreenBCoord = thisScreenRect.BEnd(aWM) +
                                     aTranslation->B(aWM);
    mLastDropDownBeforeScreenBCoord = thisScreenRect.BEnd(aWM) +
                                     aTranslation->B(aWM);
  }

  nscoord minBCoord;
  nsPresContext* pc = PresContext()->GetToplevelContentDocumentPresContext();
  nsIFrame* root = pc ? pc->PresShell()->GetRootFrame() : nullptr;
  if (root) {
    minBCoord = LogicalRect(aWM,
                            root->GetScreenRectInAppUnits(),
                            containerSize).BStart(aWM);
    if (mLastDropDownAfterScreenBCoord < minBCoord) {
      // Don't allow the drop-down to be placed before the content area.
      return;
    }
  } else {
    minBCoord = logicalScreen.BStart(aWM);
  }

  nscoord after = logicalScreen.BEnd(aWM) - mLastDropDownAfterScreenBCoord;
  nscoord before = mLastDropDownBeforeScreenBCoord - minBCoord;

  // If the difference between the space before and after is less
  // than a row-block-size, then we favor the space after.
  if (before >= after) {
    nsListControlFrame* lcf = static_cast<nsListControlFrame*>(mDropdownFrame);
    nscoord rowBSize = lcf->GetBSizeOfARow();
    if (before < after + rowBSize) {
      before -= rowBSize;
    }
  }

  *aAfter = after;
  *aBefore = before;
}

nsComboboxControlFrame::DropDownPositionState
nsComboboxControlFrame::AbsolutelyPositionDropDown()
{
  WritingMode wm = GetWritingMode();
  LogicalPoint translation(wm);
  nscoord before, after;
  mLastDropDownAfterScreenBCoord = nscoord_MIN;
  GetAvailableDropdownSpace(wm, &before, &after, &translation);
  if (before <= 0 && after <= 0) {
    if (IsDroppedDown()) {
      // Hide the view immediately to minimize flicker.
      nsView* view = mDropdownFrame->GetView();
      view->GetViewManager()->SetViewVisibility(view, nsViewVisibility_kHide);
      NS_DispatchToCurrentThread(new nsAsyncRollup(this));
    }
    return eDropDownPositionSuppressed;
  }

  LogicalSize dropdownSize = mDropdownFrame->GetLogicalSize(wm);
  nscoord bSize = std::max(before, after);
  nsListControlFrame* lcf = static_cast<nsListControlFrame*>(mDropdownFrame);
  if (bSize < dropdownSize.BSize(wm)) {
    if (lcf->GetNumDisplayRows() > 1) {
      // The drop-down doesn't fit and currently shows more than 1 row -
      // schedule a resize to show fewer rows.
      NS_DispatchToCurrentThread(new nsAsyncResize(this));
      return eDropDownPositionPendingResize;
    }
  } else if (bSize > (dropdownSize.BSize(wm) + lcf->GetBSizeOfARow() * 1.5) &&
             lcf->GetDropdownCanGrow()) {
    // The drop-down fits but there is room for at least 1.5 more rows -
    // schedule a resize to show more rows if it has more rows to show.
    // (1.5 rows for good measure to avoid any rounding issues that would
    // lead to a loop of reflow requests)
    NS_DispatchToCurrentThread(new nsAsyncResize(this));
    return eDropDownPositionPendingResize;
  }

  // Position the drop-down after if there is room, otherwise place it before
  // if there is room.  If there is no room for it on either side then place
  // it after (to avoid overlapping UI like the URL bar).
  bool b = dropdownSize.BSize(wm)<= after || dropdownSize.BSize(wm) > before;
  LogicalPoint dropdownPosition(wm, 0, b ? BSize(wm) : -dropdownSize.BSize(wm));

  // Don't position the view unless the position changed since it might cause
  // a call to NotifyGeometryChange() and an infinite loop here.
  nsSize containerSize = GetSize();
  const LogicalPoint currentPos =
    mDropdownFrame->GetLogicalPosition(containerSize);
  const LogicalPoint newPos = dropdownPosition + translation;
  if (currentPos != newPos) {
    mDropdownFrame->SetPosition(wm, newPos, containerSize);
    nsContainerFrame::PositionFrameView(mDropdownFrame);
  }
  return eDropDownPositionFinal;
}

void
nsComboboxControlFrame::NotifyGeometryChange()
{
  // We don't need to resize if we're not dropped down since ShowDropDown
  // does that, or if we're dirty then the reflow callback does it,
  // or if we have a delayed ShowDropDown pending.
  if (IsDroppedDown() &&
      !(GetStateBits() & NS_FRAME_IS_DIRTY) &&
      !mDelayedShowDropDown) {
    // Async because we're likely in a middle of a scroll here so
    // frame/view positions are in flux.
    RefPtr<nsResizeDropdownAtFinalPosition> resize =
      new nsResizeDropdownAtFinalPosition(this);
    NS_DispatchToCurrentThread(resize);
  }
}

//----------------------------------------------------------
//
//----------------------------------------------------------
#ifdef DO_REFLOW_DEBUG
static int myCounter = 0;

static void printSize(char * aDesc, nscoord aSize)
{
  printf(" %s: ", aDesc);
  if (aSize == NS_UNCONSTRAINEDSIZE) {
    printf("UC");
  } else {
    printf("%d", PX(aSize));
  }
}
#endif

//-------------------------------------------------------------------
//-- Main Reflow for the Combobox
//-------------------------------------------------------------------

nscoord
nsComboboxControlFrame::GetIntrinsicISize(nsRenderingContext* aRenderingContext,
                                          nsLayoutUtils::IntrinsicISizeType aType)
{
  // get the scrollbar width, we'll use this later
  nscoord scrollbarWidth = 0;
  nsPresContext* presContext = PresContext();
  if (mListControlFrame) {
    nsIScrollableFrame* scrollable = do_QueryFrame(mListControlFrame);
    NS_ASSERTION(scrollable, "List must be a scrollable frame");
    scrollbarWidth = scrollable->GetNondisappearingScrollbarWidth(
      presContext, aRenderingContext, GetWritingMode());
  }

  nscoord displayISize = 0;
  if (MOZ_LIKELY(mDisplayFrame)) {
    displayISize = nsLayoutUtils::IntrinsicForContainer(aRenderingContext,
                                                        mDisplayFrame,
                                                        aType);
  }

  if (mDropdownFrame) {
    nscoord dropdownContentISize;
    bool isUsingOverlayScrollbars =
      LookAndFeel::GetInt(LookAndFeel::eIntID_UseOverlayScrollbars) != 0;
    if (aType == nsLayoutUtils::MIN_ISIZE) {
      dropdownContentISize = mDropdownFrame->GetMinISize(aRenderingContext);
      if (isUsingOverlayScrollbars) {
        dropdownContentISize += scrollbarWidth;
      }
    } else {
      NS_ASSERTION(aType == nsLayoutUtils::PREF_ISIZE, "Unexpected type");
      dropdownContentISize = mDropdownFrame->GetPrefISize(aRenderingContext);
      if (isUsingOverlayScrollbars) {
        dropdownContentISize += scrollbarWidth;
      }
    }
    dropdownContentISize = NSCoordSaturatingSubtract(dropdownContentISize,
                                                     scrollbarWidth,
                                                     nscoord_MAX);

    displayISize = std::max(dropdownContentISize, displayISize);
  }

  // add room for the dropmarker button if there is one
  if ((!IsThemed() ||
       presContext->GetTheme()->ThemeNeedsComboboxDropmarker()) &&
      StyleDisplay()->mAppearance != NS_THEME_NONE) {
    displayISize += scrollbarWidth;
  }

  return displayISize;

}

nscoord
nsComboboxControlFrame::GetMinISize(nsRenderingContext *aRenderingContext)
{
  nscoord minISize;
  DISPLAY_MIN_WIDTH(this, minISize);
  minISize = GetIntrinsicISize(aRenderingContext, nsLayoutUtils::MIN_ISIZE);
  return minISize;
}

nscoord
nsComboboxControlFrame::GetPrefISize(nsRenderingContext *aRenderingContext)
{
  nscoord prefISize;
  DISPLAY_PREF_WIDTH(this, prefISize);
  prefISize = GetIntrinsicISize(aRenderingContext, nsLayoutUtils::PREF_ISIZE);
  return prefISize;
}

void
nsComboboxControlFrame::Reflow(nsPresContext*          aPresContext,
                               nsHTMLReflowMetrics&     aDesiredSize,
                               const nsHTMLReflowState& aReflowState,
                               nsReflowStatus&          aStatus)
{
  MarkInReflow();
  // Constraints we try to satisfy:

  // 1) Default inline size of button is the vertical scrollbar size
  // 2) If the inline size of button is bigger than our inline size, set
  //    inline size of button to 0.
  // 3) Default block size of button is block size of display area
  // 4) Inline size of display area is whatever is left over from our
  //    inline size after allocating inline size for the button.
  // 5) Block Size of display area is GetBSizeOfARow() on the
  //    mListControlFrame.

  if (!mDisplayFrame || !mButtonFrame || !mDropdownFrame) {
    NS_ERROR("Why did the frame constructor allow this to happen?  Fix it!!");
    return;
  }

  // Make sure the displayed text is the same as the selected option, bug 297389.
  int32_t selectedIndex;
  nsAutoString selectedOptionText;
  if (!mDroppedDown) {
    selectedIndex = mListControlFrame->GetSelectedIndex();
  }
  else {
    // In dropped down mode the "selected index" is the hovered menu item,
    // we want the last selected item which is |mDisplayedIndex| in this case.
    selectedIndex = mDisplayedIndex;
  }
  if (selectedIndex != -1) {
    mListControlFrame->GetOptionText(selectedIndex, selectedOptionText);
  }
  if (mDisplayedOptionText != selectedOptionText) {
    RedisplayText(selectedIndex);
  }

  // First reflow our dropdown so that we know how tall we should be.
  ReflowDropdown(aPresContext, aReflowState);
  RefPtr<nsResizeDropdownAtFinalPosition> resize =
    new nsResizeDropdownAtFinalPosition(this);
  if (NS_SUCCEEDED(aPresContext->PresShell()->PostReflowCallback(resize))) {
    // The reflow callback queue doesn't AddRef so we keep it alive until
    // it's released in its ReflowFinished / ReflowCallbackCanceled.
    Unused << resize.forget();
  }

  // Get the width of the vertical scrollbar.  That will be the inline
  // size of the dropdown button.
  WritingMode wm = aReflowState.GetWritingMode();
  nscoord buttonISize;
  const nsStyleDisplay *disp = StyleDisplay();
  if ((IsThemed(disp) && !aPresContext->GetTheme()->ThemeNeedsComboboxDropmarker()) ||
      StyleDisplay()->mAppearance == NS_THEME_NONE) {
    buttonISize = 0;
  }
  else {
    nsIScrollableFrame* scrollable = do_QueryFrame(mListControlFrame);
    NS_ASSERTION(scrollable, "List must be a scrollable frame");
    buttonISize = scrollable->GetNondisappearingScrollbarWidth(
      PresContext(), aReflowState.rendContext, wm);
    if (buttonISize > aReflowState.ComputedISize()) {
      buttonISize = 0;
    }
  }

  mDisplayISize = aReflowState.ComputedISize() - buttonISize;

  nsBlockFrame::Reflow(aPresContext, aDesiredSize, aReflowState, aStatus);

  // The button should occupy the same space as a scrollbar
  nsSize containerSize = aDesiredSize.PhysicalSize();
  LogicalRect buttonRect = mButtonFrame->GetLogicalRect(containerSize);

  buttonRect.IStart(wm) =
    aReflowState.ComputedLogicalBorderPadding().IStartEnd(wm) +
    mDisplayISize -
    (aReflowState.ComputedLogicalBorderPadding().IEnd(wm) -
     aReflowState.ComputedLogicalPadding().IEnd(wm));
  buttonRect.ISize(wm) = buttonISize;

  buttonRect.BStart(wm) = this->GetLogicalUsedBorder(wm).BStart(wm);
  buttonRect.BSize(wm) = mDisplayFrame->BSize(wm) +
                         this->GetLogicalUsedPadding(wm).BStartEnd(wm);

  mButtonFrame->SetRect(buttonRect, containerSize);

  if (!NS_INLINE_IS_BREAK_BEFORE(aStatus) &&
      !NS_FRAME_IS_FULLY_COMPLETE(aStatus)) {
    // This frame didn't fit inside a fragmentation container.  Splitting
    // a nsComboboxControlFrame makes no sense, so we override the status here.
    aStatus = NS_FRAME_COMPLETE;
  }
}

//--------------------------------------------------------------

nsIAtom*
nsComboboxControlFrame::GetType() const
{
  return nsGkAtoms::comboboxControlFrame;
}

#ifdef DEBUG_FRAME_DUMP
nsresult
nsComboboxControlFrame::GetFrameName(nsAString& aResult) const
{
  return MakeFrameName(NS_LITERAL_STRING("ComboboxControl"), aResult);
}
#endif


//----------------------------------------------------------------------
// nsIComboboxControlFrame
//----------------------------------------------------------------------
void
nsComboboxControlFrame::ShowDropDown(bool aDoDropDown)
{
  mDelayedShowDropDown = false;
  EventStates eventStates = mContent->AsElement()->State();
  if (aDoDropDown && eventStates.HasState(NS_EVENT_STATE_DISABLED)) {
    return;
  }

  if (!mDroppedDown && aDoDropDown) {
    nsFocusManager* fm = nsFocusManager::GetFocusManager();
    if (!fm || fm->GetFocusedContent() == GetContent()) {
      DropDownPositionState state = AbsolutelyPositionDropDown();
      if (state == eDropDownPositionFinal) {
        ShowList(aDoDropDown); // might destroy us
      } else if (state == eDropDownPositionPendingResize) {
        // Delay until after the resize reflow, see nsAsyncResize.
        mDelayedShowDropDown = true;
      }
    } else {
      // Delay until we get focus, see SetFocus().
      mDelayedShowDropDown = true;
    }
  } else if (mDroppedDown && !aDoDropDown) {
    ShowList(aDoDropDown); // might destroy us
  }
}

void
nsComboboxControlFrame::SetDropDown(nsIFrame* aDropDownFrame)
{
  mDropdownFrame = aDropDownFrame;
  mListControlFrame = do_QueryFrame(mDropdownFrame);
}

nsIFrame*
nsComboboxControlFrame::GetDropDown()
{
  return mDropdownFrame;
}

///////////////////////////////////////////////////////////////

NS_IMETHODIMP
nsComboboxControlFrame::RedisplaySelectedText()
{
  nsAutoScriptBlocker scriptBlocker;
  return RedisplayText(mListControlFrame->GetSelectedIndex());
}

nsresult
nsComboboxControlFrame::RedisplayText(int32_t aIndex)
{
  // Get the text to display
  if (aIndex != -1) {
    mListControlFrame->GetOptionText(aIndex, mDisplayedOptionText);
  } else {
    mDisplayedOptionText.Truncate();
  }
  mDisplayedIndex = aIndex;

  REFLOW_DEBUG_MSG2("RedisplayText \"%s\"\n",
                    NS_LossyConvertUTF16toASCII(mDisplayedOptionText).get());

  // Send reflow command because the new text maybe larger
  nsresult rv = NS_OK;
  if (mDisplayContent) {
    // Don't call ActuallyDisplayText(true) directly here since that
    // could cause recursive frame construction. See bug 283117 and the comment in
    // HandleRedisplayTextEvent() below.

    // Revoke outstanding events to avoid out-of-order events which could mean
    // displaying the wrong text.
    mRedisplayTextEvent.Revoke();

    NS_ASSERTION(!nsContentUtils::IsSafeToRunScript(),
                 "If we happen to run our redisplay event now, we might kill "
                 "ourselves!");

    RefPtr<RedisplayTextEvent> event = new RedisplayTextEvent(this);
    mRedisplayTextEvent = event;
    if (!nsContentUtils::AddScriptRunner(event))
      mRedisplayTextEvent.Forget();
  }
  return rv;
}

void
nsComboboxControlFrame::HandleRedisplayTextEvent()
{
  // First, make sure that the content model is up to date and we've
  // constructed the frames for all our content in the right places.
  // Otherwise they'll end up under the wrong insertion frame when we
  // ActuallyDisplayText, since that flushes out the content sink by
  // calling SetText on a DOM node with aNotify set to true.  See bug
  // 289730.
  nsWeakFrame weakThis(this);
  PresContext()->Document()->
    FlushPendingNotifications(Flush_ContentAndNotify);
  if (!weakThis.IsAlive())
    return;

  // Redirect frame insertions during this method (see GetContentInsertionFrame())
  // so that any reframing that the frame constructor forces upon us is inserted
  // into the correct parent (mDisplayFrame). See bug 282607.
  NS_PRECONDITION(!mInRedisplayText, "Nested RedisplayText");
  mInRedisplayText = true;
  mRedisplayTextEvent.Forget();

  ActuallyDisplayText(true);
  if (!weakThis.IsAlive()) {
    return;
  }
  // XXXbz This should perhaps be eResize.  Check.
  PresContext()->PresShell()->FrameNeedsReflow(mDisplayFrame,
                                               nsIPresShell::eStyleChange,
                                               NS_FRAME_IS_DIRTY);

  mInRedisplayText = false;
}

void
nsComboboxControlFrame::ActuallyDisplayText(bool aNotify)
{
  nsCOMPtr<nsIContent> displayContent = mDisplayContent;
  if (mDisplayedOptionText.IsEmpty()) {
    // Have to use a non-breaking space for line-block-size calculations
    // to be right
    static const char16_t space = 0xA0;
    displayContent->SetText(&space, 1, aNotify);
  } else {
    displayContent->SetText(mDisplayedOptionText, aNotify);
  }
}

int32_t
nsComboboxControlFrame::GetIndexOfDisplayArea()
{
  return mDisplayedIndex;
}

//----------------------------------------------------------------------
// nsISelectControlFrame
//----------------------------------------------------------------------
NS_IMETHODIMP
nsComboboxControlFrame::DoneAddingChildren(bool aIsDone)
{
  nsISelectControlFrame* listFrame = do_QueryFrame(mDropdownFrame);
  if (!listFrame)
    return NS_ERROR_FAILURE;

  return listFrame->DoneAddingChildren(aIsDone);
}

NS_IMETHODIMP
nsComboboxControlFrame::AddOption(int32_t aIndex)
{
  if (aIndex <= mDisplayedIndex) {
    ++mDisplayedIndex;
  }

  nsListControlFrame* lcf = static_cast<nsListControlFrame*>(mDropdownFrame);
  return lcf->AddOption(aIndex);
}


NS_IMETHODIMP
nsComboboxControlFrame::RemoveOption(int32_t aIndex)
{
  nsWeakFrame weakThis(this);
  if (mListControlFrame->GetNumberOfOptions() > 0) {
    if (aIndex < mDisplayedIndex) {
      --mDisplayedIndex;
    } else if (aIndex == mDisplayedIndex) {
      mDisplayedIndex = 0; // IE6 compat
      RedisplayText(mDisplayedIndex);
    }
  }
  else {
    // If we removed the last option, we need to blank things out
    RedisplayText(-1);
  }

  if (!weakThis.IsAlive())
    return NS_OK;

  nsListControlFrame* lcf = static_cast<nsListControlFrame*>(mDropdownFrame);
  return lcf->RemoveOption(aIndex);
}

NS_IMETHODIMP
nsComboboxControlFrame::OnSetSelectedIndex(int32_t aOldIndex, int32_t aNewIndex)
{
  nsAutoScriptBlocker scriptBlocker;
  RedisplayText(aNewIndex);
  NS_ASSERTION(mDropdownFrame, "No dropdown frame!");

  nsISelectControlFrame* listFrame = do_QueryFrame(mDropdownFrame);
  NS_ASSERTION(listFrame, "No list frame!");

  return listFrame->OnSetSelectedIndex(aOldIndex, aNewIndex);
}

// End nsISelectControlFrame
//----------------------------------------------------------------------

nsresult
nsComboboxControlFrame::HandleEvent(nsPresContext* aPresContext,
                                    WidgetGUIEvent* aEvent,
                                    nsEventStatus* aEventStatus)
{
  NS_ENSURE_ARG_POINTER(aEventStatus);

  if (nsEventStatus_eConsumeNoDefault == *aEventStatus) {
    return NS_OK;
  }

  EventStates eventStates = mContent->AsElement()->State();
  if (eventStates.HasState(NS_EVENT_STATE_DISABLED)) {
    return NS_OK;
  }

#if COMBOBOX_ROLLUP_CONSUME_EVENT == 0
  if (aEvent->mMessage == eMouseDown) {
    nsIWidget* widget = GetNearestWidget();
    if (widget && GetContent() == widget->GetLastRollup()) {
      // This event did a Rollup on this control - prevent it from opening
      // the dropdown again!
      *aEventStatus = nsEventStatus_eConsumeNoDefault;
      return NS_OK;
    }
  }
#endif

  // If we have style that affects how we are selected, feed event down to
  // nsFrame::HandleEvent so that selection takes place when appropriate.
  const nsStyleUserInterface* uiStyle = StyleUserInterface();
  if (uiStyle->mUserInput == NS_STYLE_USER_INPUT_NONE || uiStyle->mUserInput == NS_STYLE_USER_INPUT_DISABLED)
    return nsBlockFrame::HandleEvent(aPresContext, aEvent, aEventStatus);

  return NS_OK;
}


nsresult
nsComboboxControlFrame::SetFormProperty(nsIAtom* aName, const nsAString& aValue)
{
  nsIFormControlFrame* fcFrame = do_QueryFrame(mDropdownFrame);
  if (!fcFrame) {
    return NS_NOINTERFACE;
  }

  return fcFrame->SetFormProperty(aName, aValue);
}

nsContainerFrame*
nsComboboxControlFrame::GetContentInsertionFrame() {
  return mInRedisplayText ? mDisplayFrame : mDropdownFrame->GetContentInsertionFrame();
}

nsresult
nsComboboxControlFrame::CreateAnonymousContent(nsTArray<ContentInfo>& aElements)
{
  // The frames used to display the combo box and the button used to popup the dropdown list
  // are created through anonymous content. The dropdown list is not created through anonymous
  // content because its frame is initialized specifically for the drop-down case and it is placed
  // a special list referenced through NS_COMBO_FRAME_POPUP_LIST_INDEX to keep separate from the
  // layout of the display and button.
  //
  // Note: The value attribute of the display content is set when an item is selected in the dropdown list.
  // If the content specified below does not honor the value attribute than nothing will be displayed.

  // For now the content that is created corresponds to two input buttons. It would be better to create the
  // tag as something other than input, but then there isn't any way to create a button frame since it
  // isn't possible to set the display type in CSS2 to create a button frame.

    // create content used for display
  //nsIAtom* tag = NS_NewAtom("mozcombodisplay");

  // Add a child text content node for the label

  nsNodeInfoManager *nimgr = mContent->NodeInfo()->NodeInfoManager();

  mDisplayContent = new nsTextNode(nimgr);

  // set the value of the text node
  mDisplayedIndex = mListControlFrame->GetSelectedIndex();
  if (mDisplayedIndex != -1) {
    mListControlFrame->GetOptionText(mDisplayedIndex, mDisplayedOptionText);
  }
  ActuallyDisplayText(false);

  if (!aElements.AppendElement(mDisplayContent))
    return NS_ERROR_OUT_OF_MEMORY;

  mButtonContent = mContent->OwnerDoc()->CreateHTMLElement(nsGkAtoms::button);
  if (!mButtonContent)
    return NS_ERROR_OUT_OF_MEMORY;

  // make someone to listen to the button. If its pressed by someone like Accessibility
  // then open or close the combo box.
  mButtonListener = new nsComboButtonListener(this);
  mButtonContent->AddEventListener(NS_LITERAL_STRING("click"), mButtonListener,
                                   false, false);

  mButtonContent->SetAttr(kNameSpaceID_None, nsGkAtoms::type,
                          NS_LITERAL_STRING("button"), false);
  // Set tabindex="-1" so that the button is not tabbable
  mButtonContent->SetAttr(kNameSpaceID_None, nsGkAtoms::tabindex,
                          NS_LITERAL_STRING("-1"), false);

  if (!aElements.AppendElement(mButtonContent))
    return NS_ERROR_OUT_OF_MEMORY;

  return NS_OK;
}

void
nsComboboxControlFrame::AppendAnonymousContentTo(nsTArray<nsIContent*>& aElements,
                                                 uint32_t aFilter)
{
  if (mDisplayContent) {
    aElements.AppendElement(mDisplayContent);
  }

  if (mButtonContent) {
    aElements.AppendElement(mButtonContent);
  }
}

// XXXbz this is a for-now hack.  Now that display:inline-block works,
// need to revisit this.
class nsComboboxDisplayFrame : public nsBlockFrame {
public:
  NS_DECL_FRAMEARENA_HELPERS

  nsComboboxDisplayFrame (nsStyleContext* aContext,
                          nsComboboxControlFrame* aComboBox)
    : nsBlockFrame(aContext),
      mComboBox(aComboBox)
  {}

  // Need this so that line layout knows that this block's inline size
  // depends on the available inline size.
  virtual nsIAtom* GetType() const override;

  virtual bool IsFrameOfType(uint32_t aFlags) const override
  {
    return nsBlockFrame::IsFrameOfType(aFlags &
      ~(nsIFrame::eReplacedContainsBlock));
  }

  virtual void Reflow(nsPresContext*           aPresContext,
                          nsHTMLReflowMetrics&     aDesiredSize,
                          const nsHTMLReflowState& aReflowState,
                          nsReflowStatus&          aStatus) override;

  virtual void BuildDisplayList(nsDisplayListBuilder*   aBuilder,
                                const nsRect&           aDirtyRect,
                                const nsDisplayListSet& aLists) override;

protected:
  nsComboboxControlFrame* mComboBox;
};

NS_IMPL_FRAMEARENA_HELPERS(nsComboboxDisplayFrame)

nsIAtom*
nsComboboxDisplayFrame::GetType() const
{
  return nsGkAtoms::comboboxDisplayFrame;
}

void
nsComboboxDisplayFrame::Reflow(nsPresContext*           aPresContext,
                               nsHTMLReflowMetrics&     aDesiredSize,
                               const nsHTMLReflowState& aReflowState,
                               nsReflowStatus&          aStatus)
{
  nsHTMLReflowState state(aReflowState);
  if (state.ComputedBSize() == NS_INTRINSICSIZE) {
    // Note that the only way we can have a computed block size here is
    // if the combobox had a specified block size.  If it didn't, size
    // based on what our rows look like, for lack of anything better.
    state.SetComputedBSize(mComboBox->mListControlFrame->GetBSizeOfARow());
  }
  WritingMode wm = aReflowState.GetWritingMode();
  nscoord computedISize = mComboBox->mDisplayISize -
    state.ComputedLogicalBorderPadding().IStartEnd(wm);
  if (computedISize < 0) {
    computedISize = 0;
  }
  state.SetComputedISize(computedISize);
  nsBlockFrame::Reflow(aPresContext, aDesiredSize, state, aStatus);
  aStatus = NS_FRAME_COMPLETE; // this type of frame can't be split
}

void
nsComboboxDisplayFrame::BuildDisplayList(nsDisplayListBuilder*   aBuilder,
                                         const nsRect&           aDirtyRect,
                                         const nsDisplayListSet& aLists)
{
  nsDisplayListCollection set;
  nsBlockFrame::BuildDisplayList(aBuilder, aDirtyRect, set);

  // remove background items if parent frame is themed
  if (mComboBox->IsThemed()) {
    set.BorderBackground()->DeleteAll();
  }

  set.MoveTo(aLists);
}

nsIFrame*
nsComboboxControlFrame::CreateFrameFor(nsIContent*      aContent)
{
  NS_PRECONDITION(nullptr != aContent, "null ptr");

  NS_ASSERTION(mDisplayContent, "mDisplayContent can't be null!");

  if (mDisplayContent != aContent) {
    // We only handle the frames for mDisplayContent here
    return nullptr;
  }

  // Get PresShell
  nsIPresShell *shell = PresContext()->PresShell();
  nsStyleSet *styleSet = shell->StyleSet();

  // create the style contexts for the anonymous block frame and text frame
  RefPtr<nsStyleContext> styleContext;
  styleContext = styleSet->
    ResolveAnonymousBoxStyle(nsCSSAnonBoxes::mozDisplayComboboxControlFrame,
                             mStyleContext,
                             nsStyleSet::eSkipParentDisplayBasedStyleFixup);

  RefPtr<nsStyleContext> textStyleContext;
  textStyleContext = styleSet->ResolveStyleForNonElement(mStyleContext);

  // Start by creating our anonymous block frame
  mDisplayFrame = new (shell) nsComboboxDisplayFrame(styleContext, this);
  mDisplayFrame->Init(mContent, this, nullptr);

  // Create a text frame and put it inside the block frame
  nsIFrame* textFrame = NS_NewTextFrame(shell, textStyleContext);

  // initialize the text frame
  textFrame->Init(aContent, mDisplayFrame, nullptr);
  mDisplayContent->SetPrimaryFrame(textFrame);

  nsFrameList textList(textFrame, textFrame);
  mDisplayFrame->SetInitialChildList(kPrincipalList, textList);
  return mDisplayFrame;
}

void
nsComboboxControlFrame::DestroyFrom(nsIFrame* aDestructRoot)
{
  // Revoke any pending RedisplayTextEvent
  mRedisplayTextEvent.Revoke();

  nsFormControlFrame::RegUnRegAccessKey(static_cast<nsIFrame*>(this), false);

  if (mDroppedDown) {
    MOZ_ASSERT(mDropdownFrame, "mDroppedDown without frame");
    nsView* view = mDropdownFrame->GetView();
    MOZ_ASSERT(view);
    nsIWidget* widget = view->GetWidget();
    if (widget) {
      widget->CaptureRollupEvents(this, false);
    }
  }

  // Cleanup frames in popup child list
  mPopupFrames.DestroyFramesFrom(aDestructRoot);
  nsContentUtils::DestroyAnonymousContent(&mDisplayContent);
  nsContentUtils::DestroyAnonymousContent(&mButtonContent);
  nsBlockFrame::DestroyFrom(aDestructRoot);
}

const nsFrameList&
nsComboboxControlFrame::GetChildList(ChildListID aListID) const
{
  if (kSelectPopupList == aListID) {
    return mPopupFrames;
  }
  return nsBlockFrame::GetChildList(aListID);
}

void
nsComboboxControlFrame::GetChildLists(nsTArray<ChildList>* aLists) const
{
  nsBlockFrame::GetChildLists(aLists);
  mPopupFrames.AppendIfNonempty(aLists, kSelectPopupList);
}

void
nsComboboxControlFrame::SetInitialChildList(ChildListID     aListID,
                                            nsFrameList&    aChildList)
{
  if (kSelectPopupList == aListID) {
    mPopupFrames.SetFrames(aChildList);
  } else {
    for (nsFrameList::Enumerator e(aChildList); !e.AtEnd(); e.Next()) {
      nsCOMPtr<nsIFormControl> formControl =
        do_QueryInterface(e.get()->GetContent());
      if (formControl && formControl->GetType() == NS_FORM_BUTTON_BUTTON) {
        mButtonFrame = e.get();
        break;
      }
    }
    NS_ASSERTION(mButtonFrame, "missing button frame in initial child list");
    nsBlockFrame::SetInitialChildList(aListID, aChildList);
  }
}

//----------------------------------------------------------------------
  //nsIRollupListener
//----------------------------------------------------------------------
bool
nsComboboxControlFrame::Rollup(uint32_t aCount, bool aFlush,
                               const nsIntPoint* pos, nsIContent** aLastRolledUp)
{
  if (!mDroppedDown) {
    return false;
  }

  bool consume = !!COMBOBOX_ROLLUP_CONSUME_EVENT;
  nsWeakFrame weakFrame(this);
  mListControlFrame->AboutToRollup(); // might destroy us
  if (!weakFrame.IsAlive()) {
    return consume;
  }
  ShowDropDown(false); // might destroy us
  if (weakFrame.IsAlive()) {
    mListControlFrame->CaptureMouseEvents(false);
  }

  if (aFlush && weakFrame.IsAlive()) {
    // The popup's visibility doesn't update until the minimize animation has
    // finished, so call UpdateWidgetGeometry to update it right away.
    nsViewManager* viewManager = mDropdownFrame->GetView()->GetViewManager();
    viewManager->UpdateWidgetGeometry();
  }

  if (aLastRolledUp) {
    *aLastRolledUp = GetContent();
  }
  return consume;
}

nsIWidget*
nsComboboxControlFrame::GetRollupWidget()
{
  nsView* view = mDropdownFrame->GetView();
  MOZ_ASSERT(view);
  return view->GetWidget();
}

void
nsComboboxControlFrame::RollupFromList()
{
  if (ShowList(false))
    mListControlFrame->CaptureMouseEvents(false);
}

int32_t
nsComboboxControlFrame::UpdateRecentIndex(int32_t aIndex)
{
  int32_t index = mRecentSelectedIndex;
  if (mRecentSelectedIndex == NS_SKIP_NOTIFY_INDEX || aIndex == NS_SKIP_NOTIFY_INDEX)
    mRecentSelectedIndex = aIndex;
  return index;
}

class nsDisplayComboboxFocus : public nsDisplayItem {
public:
  nsDisplayComboboxFocus(nsDisplayListBuilder* aBuilder,
                         nsComboboxControlFrame* aFrame)
    : nsDisplayItem(aBuilder, aFrame) {
    MOZ_COUNT_CTOR(nsDisplayComboboxFocus);
  }
#ifdef NS_BUILD_REFCNT_LOGGING
  virtual ~nsDisplayComboboxFocus() {
    MOZ_COUNT_DTOR(nsDisplayComboboxFocus);
  }
#endif

  virtual void Paint(nsDisplayListBuilder* aBuilder,
                     nsRenderingContext* aCtx) override;
  NS_DISPLAY_DECL_NAME("ComboboxFocus", TYPE_COMBOBOX_FOCUS)
};

void nsDisplayComboboxFocus::Paint(nsDisplayListBuilder* aBuilder,
                                   nsRenderingContext* aCtx)
{
  static_cast<nsComboboxControlFrame*>(mFrame)
    ->PaintFocus(*aCtx->GetDrawTarget(), ToReferenceFrame());
}

void
nsComboboxControlFrame::BuildDisplayList(nsDisplayListBuilder*   aBuilder,
                                         const nsRect&           aDirtyRect,
                                         const nsDisplayListSet& aLists)
{
#ifdef NOISY
  printf("%p paint at (%d, %d, %d, %d)\n", this,
    aDirtyRect.x, aDirtyRect.y, aDirtyRect.width, aDirtyRect.height);
#endif

  if (aBuilder->IsForEventDelivery()) {
    // Don't allow children to receive events.
    // REVIEW: following old GetFrameForPoint
    DisplayBorderBackgroundOutline(aBuilder, aLists);
  } else {
    // REVIEW: Our in-flow child frames are inline-level so they will paint in our
    // content list, so we don't need to mess with layers.
    nsBlockFrame::BuildDisplayList(aBuilder, aDirtyRect, aLists);
  }

  // draw a focus indicator only when focus rings should be drawn
  nsIDocument* doc = mContent->GetComposedDoc();
  if (doc) {
    nsPIDOMWindow* window = doc->GetWindow();
    if (window && window->ShouldShowFocusRing()) {
      nsPresContext *presContext = PresContext();
      const nsStyleDisplay *disp = StyleDisplay();
      if ((!IsThemed(disp) ||
           !presContext->GetTheme()->ThemeDrawsFocusForWidget(disp->mAppearance)) &&
          mDisplayFrame && IsVisibleForPainting(aBuilder)) {
        aLists.Content()->AppendNewToTop(
          new (aBuilder) nsDisplayComboboxFocus(aBuilder, this));
      }
    }
  }

  DisplaySelectionOverlay(aBuilder, aLists.Content());
}

void nsComboboxControlFrame::PaintFocus(DrawTarget& aDrawTarget,
                                        nsPoint aPt)
{
  /* Do we need to do anything? */
  EventStates eventStates = mContent->AsElement()->State();
  if (eventStates.HasState(NS_EVENT_STATE_DISABLED) || sFocused != this)
    return;

  int32_t appUnitsPerDevPixel = PresContext()->AppUnitsPerDevPixel();

  nsRect clipRect = mDisplayFrame->GetRect() + aPt;
  aDrawTarget.PushClipRect(NSRectToSnappedRect(clipRect,
                                               appUnitsPerDevPixel,
                                               aDrawTarget));

  // REVIEW: Why does the old code paint mDisplayFrame again? We've
  // already painted it in the children above. So clipping it here won't do
  // us much good.

  /////////////////////
  // draw focus

  StrokeOptions strokeOptions;
  nsLayoutUtils::InitDashPattern(strokeOptions, NS_STYLE_BORDER_STYLE_DOTTED);
  ColorPattern color(ToDeviceColor(StyleColor()->mColor));
  nscoord onePixel = nsPresContext::CSSPixelsToAppUnits(1);
  clipRect.width -= onePixel;
  clipRect.height -= onePixel;
  Rect r = ToRect(nsLayoutUtils::RectToGfxRect(clipRect, appUnitsPerDevPixel));
  StrokeSnappedEdgesOfRect(r, aDrawTarget, color, strokeOptions);

  aDrawTarget.PopClip();
}

//---------------------------------------------------------
// gets the content (an option) by index and then set it as
// being selected or not selected
//---------------------------------------------------------
NS_IMETHODIMP
nsComboboxControlFrame::OnOptionSelected(int32_t aIndex, bool aSelected)
{
  if (mDroppedDown) {
    nsISelectControlFrame *selectFrame = do_QueryFrame(mListControlFrame);
    if (selectFrame) {
      selectFrame->OnOptionSelected(aIndex, aSelected);
    }
  } else {
    if (aSelected) {
      nsAutoScriptBlocker blocker;
      RedisplayText(aIndex);
    } else {
      nsWeakFrame weakFrame(this);
      RedisplaySelectedText();
      if (weakFrame.IsAlive()) {
        FireValueChangeEvent(); // Fire after old option is unselected
      }
    }
  }

  return NS_OK;
}

void nsComboboxControlFrame::FireValueChangeEvent()
{
  // Fire ValueChange event to indicate data value of combo box has changed
  nsContentUtils::AddScriptRunner(
    new AsyncEventDispatcher(mContent, NS_LITERAL_STRING("ValueChange"), true,
                             false));
}

void
nsComboboxControlFrame::OnContentReset()
{
  if (mListControlFrame) {
    mListControlFrame->OnContentReset();
  }
}


//--------------------------------------------------------
// nsIStatefulFrame
//--------------------------------------------------------
NS_IMETHODIMP
nsComboboxControlFrame::SaveState(nsPresState** aState)
{
  if (!mListControlFrame)
    return NS_ERROR_FAILURE;

  nsIStatefulFrame* stateful = do_QueryFrame(mListControlFrame);
  return stateful->SaveState(aState);
}

NS_IMETHODIMP
nsComboboxControlFrame::RestoreState(nsPresState* aState)
{
  if (!mListControlFrame)
    return NS_ERROR_FAILURE;

  nsIStatefulFrame* stateful = do_QueryFrame(mListControlFrame);
  NS_ASSERTION(stateful, "Must implement nsIStatefulFrame");
  return stateful->RestoreState(aState);
}


// Fennec uses a custom combobox built-in widget.
//

/* static */
bool
nsComboboxControlFrame::ToolkitHasNativePopup()
{
#ifdef MOZ_USE_NATIVE_POPUP_WINDOWS
  return true;
#else
  return false;
#endif /* MOZ_USE_NATIVE_POPUP_WINDOWS */
}

