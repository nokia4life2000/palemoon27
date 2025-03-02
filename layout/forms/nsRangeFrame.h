/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsRangeFrame_h___
#define nsRangeFrame_h___

#include "mozilla/Attributes.h"
#include "mozilla/Decimal.h"
#include "mozilla/EventForwards.h"
#include "nsContainerFrame.h"
#include "nsIAnonymousContentCreator.h"
#include "nsIDOMEventListener.h"
#include "nsCOMPtr.h"

class nsDisplayRangeFocusRing;

class nsRangeFrame : public nsContainerFrame,
                     public nsIAnonymousContentCreator
{
  friend nsIFrame*
  NS_NewRangeFrame(nsIPresShell* aPresShell, nsStyleContext* aContext);

  friend class nsDisplayRangeFocusRing;

  explicit nsRangeFrame(nsStyleContext* aContext);
  virtual ~nsRangeFrame();

  typedef mozilla::dom::Element Element;

public:
  NS_DECL_QUERYFRAME_TARGET(nsRangeFrame)
  NS_DECL_QUERYFRAME
  NS_DECL_FRAMEARENA_HELPERS

  // nsIFrame overrides
  virtual void Init(nsIContent*       aContent,
                    nsContainerFrame* aParent,
                    nsIFrame*         aPrevInFlow) override;

  virtual void DestroyFrom(nsIFrame* aDestructRoot) override;

  void BuildDisplayList(nsDisplayListBuilder*   aBuilder,
                        const nsRect&           aDirtyRect,
                        const nsDisplayListSet& aLists) override;

  virtual void Reflow(nsPresContext*           aPresContext,
                      nsHTMLReflowMetrics&     aDesiredSize,
                      const nsHTMLReflowState& aReflowState,
                      nsReflowStatus&          aStatus) override;

#ifdef DEBUG_FRAME_DUMP
  virtual nsresult GetFrameName(nsAString& aResult) const override {
    return MakeFrameName(NS_LITERAL_STRING("Range"), aResult);
  }
#endif

  virtual bool IsLeaf() const override { return true; }

#ifdef ACCESSIBILITY
  virtual mozilla::a11y::AccType AccessibleType() override;
#endif

  // nsIAnonymousContentCreator
  virtual nsresult CreateAnonymousContent(nsTArray<ContentInfo>& aElements) override;
  virtual void AppendAnonymousContentTo(nsTArray<nsIContent*>& aElements,
                                        uint32_t aFilter) override;

  virtual nsresult AttributeChanged(int32_t  aNameSpaceID,
                                    nsIAtom* aAttribute,
                                    int32_t  aModType) override;

  virtual mozilla::LogicalSize
  ComputeAutoSize(nsRenderingContext *aRenderingContext,
                  mozilla::WritingMode aWritingMode,
                  const mozilla::LogicalSize& aCBSize,
                  nscoord aAvailableISize,
                  const mozilla::LogicalSize& aMargin,
                  const mozilla::LogicalSize& aBorder,
                  const mozilla::LogicalSize& aPadding,
                  bool aShrinkWrap) override;

  virtual nscoord GetMinISize(nsRenderingContext *aRenderingContext) override;
  virtual nscoord GetPrefISize(nsRenderingContext *aRenderingContext) override;

  virtual nsIAtom* GetType() const override;

  virtual bool IsFrameOfType(uint32_t aFlags) const override
  {
    return nsContainerFrame::IsFrameOfType(aFlags &
      ~(nsIFrame::eReplaced | nsIFrame::eReplacedContainsBlock));
  }

  nsStyleContext* GetAdditionalStyleContext(int32_t aIndex) const override;
  void SetAdditionalStyleContext(int32_t aIndex,
                                 nsStyleContext* aStyleContext) override;

  /**
   * Returns true if the slider's thumb moves horizontally, or else false if it
   * moves vertically.
   *
   * aOverrideFrameSize If specified, this will be used instead of the size of
   *   the frame's rect (i.e. the frame's border-box size) if the frame's
   *   rect would have otherwise been examined. This should only be specified
   *   during reflow when the frame's [new] border-box size has not yet been
   *   stored in its mRect.
   */
  bool IsHorizontal(const nsSize *aFrameSizeOverride = nullptr) const;

  double GetMin() const;
  double GetMax() const;
  double GetValue() const;

  /** 
   * Returns the input element's value as a fraction of the difference between
   * the input's minimum and its maximum (i.e. returns 0.0 when the value is
   * the same as the minimum, and returns 1.0 when the value is the same as the 
   * maximum).
   */  
  double GetValueAsFractionOfRange();

  /**
   * Returns whether the frame and its child should use the native style.
   */
  bool ShouldUseNativeStyle() const;

  mozilla::Decimal GetValueAtEventPoint(mozilla::WidgetGUIEvent* aEvent);

  /**
   * Helper that's used when the value of the range changes to reposition the
   * thumb, resize the range-progress element, and schedule a repaint. (This
   * does not reflow, since the position and size of the thumb and
   * range-progress element do not affect the position or size of any other
   * frames.)
   */
  void UpdateForValueChange();

  virtual Element* GetPseudoElement(nsCSSPseudoElements::Type aType) override;

private:

  nsresult MakeAnonymousDiv(Element** aResult,
                            nsCSSPseudoElements::Type aPseudoType,
                            nsTArray<ContentInfo>& aElements);

  // Helper function which reflows the anonymous div frames.
  void ReflowAnonymousContent(nsPresContext*           aPresContext,
                              nsHTMLReflowMetrics&     aDesiredSize,
                              const nsHTMLReflowState& aReflowState);

  void DoUpdateThumbPosition(nsIFrame* aThumbFrame,
                             const nsSize& aRangeSize);

  void DoUpdateRangeProgressFrame(nsIFrame* aProgressFrame,
                                  const nsSize& aRangeSize);

  /**
   * The div used to show the ::-moz-range-track pseudo-element.
   * @see nsRangeFrame::CreateAnonymousContent
   */
  nsCOMPtr<Element> mTrackDiv;

  /**
   * The div used to show the ::-moz-range-progress pseudo-element, which is
   * used to (optionally) style the specific chunk of track leading up to the
   * thumb's current position.
   * @see nsRangeFrame::CreateAnonymousContent
   */
  nsCOMPtr<Element> mProgressDiv;

  /**
   * The div used to show the ::-moz-range-thumb pseudo-element.
   * @see nsRangeFrame::CreateAnonymousContent
   */
  nsCOMPtr<Element> mThumbDiv;

  /**
   * Cached style context for -moz-focus-outer CSS pseudo-element style.
   */
  RefPtr<nsStyleContext> mOuterFocusStyle;

  class DummyTouchListener final : public nsIDOMEventListener
  {
  private:
    ~DummyTouchListener() {}

  public:
    NS_DECL_ISUPPORTS

    NS_IMETHOD HandleEvent(nsIDOMEvent* aEvent) override
    {
      return NS_OK;
    }
  };

  /**
   * A no-op touch-listener used for APZ purposes (see nsRangeFrame::Init).
   */
  RefPtr<DummyTouchListener> mDummyTouchListener;
};

#endif
