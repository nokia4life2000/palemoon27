/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_KeyframeEffect_h
#define mozilla_dom_KeyframeEffect_h

#include "nsAutoPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsCSSPseudoElements.h"
#include "nsIDocument.h"
#include "nsWrapperCache.h"
#include "mozilla/Attributes.h"
#include "mozilla/ComputedTimingFunction.h" // ComputedTimingFunction
#include "mozilla/LayerAnimationInfo.h"     // LayerAnimations::kRecords
#include "mozilla/StickyTimeDuration.h"
#include "mozilla/StyleAnimationValue.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/AnimationEffectReadOnly.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/KeyframeBinding.h"
#include "mozilla/dom/Nullable.h"

struct JSContext;
class nsCSSPropertySet;

namespace mozilla {

class AnimValuesStyleRule;

namespace dom {
struct ComputedTimingProperties;
}

/**
 * Input timing parameters.
 *
 * Eventually this will represent all the input timing parameters specified
 * by content but for now it encapsulates just the subset of those
 * parameters passed to GetPositionInIteration.
 */
struct AnimationTiming
{
  TimeDuration mIterationDuration;
  TimeDuration mDelay;
  float mIterationCount; // mozilla::PositiveInfinity<float>() means infinite
  uint8_t mDirection;
  uint8_t mFillMode;

  bool FillsForwards() const {
    return mFillMode == NS_STYLE_ANIMATION_FILL_MODE_BOTH ||
           mFillMode == NS_STYLE_ANIMATION_FILL_MODE_FORWARDS;
  }
  bool FillsBackwards() const {
    return mFillMode == NS_STYLE_ANIMATION_FILL_MODE_BOTH ||
           mFillMode == NS_STYLE_ANIMATION_FILL_MODE_BACKWARDS;
  }
  bool operator==(const AnimationTiming& aOther) const {
    return mIterationDuration == aOther.mIterationDuration &&
           mDelay == aOther.mDelay &&
           mIterationCount == aOther.mIterationCount &&
           mDirection == aOther.mDirection &&
           mFillMode == aOther.mFillMode;
  }
  bool operator!=(const AnimationTiming& aOther) const {
    return !(*this == aOther);
  }
};

/**
 * Stores the results of calculating the timing properties of an animation
 * at a given sample time.
 */
struct ComputedTiming
{
  // The total duration of the animation including all iterations.
  // Will equal StickyTimeDuration::Forever() if the animation repeats
  // indefinitely.
  StickyTimeDuration  mActiveDuration;
  // Progress towards the end of the current iteration. If the effect is
  // being sampled backwards, this will go from 1.0 to 0.0.
  // Will be null if the animation is neither animating nor
  // filling at the sampled time.
  Nullable<double>    mProgress;
  // Zero-based iteration index (meaningless if mProgress is null).
  uint64_t            mCurrentIteration = 0;

  enum class AnimationPhase {
    Null,   // Not sampled (null sample time)
    Before, // Sampled prior to the start of the active interval
    Active, // Sampled within the active interval
    After   // Sampled after (or at) the end of the active interval
  };
  AnimationPhase      mPhase = AnimationPhase::Null;
};

struct AnimationPropertySegment
{
  float mFromKey, mToKey;
  StyleAnimationValue mFromValue, mToValue;
  ComputedTimingFunction mTimingFunction;

  bool operator==(const AnimationPropertySegment& aOther) const {
    return mFromKey == aOther.mFromKey &&
           mToKey == aOther.mToKey &&
           mFromValue == aOther.mFromValue &&
           mToValue == aOther.mToValue &&
           mTimingFunction == aOther.mTimingFunction;
  }
  bool operator!=(const AnimationPropertySegment& aOther) const {
    return !(*this == aOther);
  }
};

struct AnimationProperty
{
  nsCSSProperty mProperty;

  // Does this property win in the CSS Cascade?
  //
  // For CSS transitions, this is true as long as a CSS animation on the
  // same property and element is not running, in which case we set this
  // to false so that the animation (lower in the cascade) can win.  We
  // then use this to decide whether to apply the style both in the CSS
  // cascade and for OMTA.
  //
  // For CSS Animations, which are overridden by !important rules in the
  // cascade, we actually determine this from the CSS cascade
  // computations, and then use it for OMTA.
  // **NOTE**: For CSS animations, we only bother setting mWinsInCascade
  // accurately for properties that we can animate on the compositor.
  // For other properties, we make it always be true.
  bool mWinsInCascade;

  InfallibleTArray<AnimationPropertySegment> mSegments;

  bool operator==(const AnimationProperty& aOther) const {
    return mProperty == aOther.mProperty &&
           mWinsInCascade == aOther.mWinsInCascade &&
           mSegments == aOther.mSegments;
  }
  bool operator!=(const AnimationProperty& aOther) const {
    return !(*this == aOther);
  }
};

struct ElementPropertyTransition;

namespace dom {

class Animation;

class KeyframeEffectReadOnly : public AnimationEffectReadOnly
{
public:
  KeyframeEffectReadOnly(nsIDocument* aDocument,
                         Element* aTarget,
                         nsCSSPseudoElements::Type aPseudoType,
                         const AnimationTiming& aTiming);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS_INHERITED(KeyframeEffectReadOnly,
                                                        AnimationEffectReadOnly)

  virtual JSObject* WrapObject(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto) override;

  virtual ElementPropertyTransition* AsTransition() { return nullptr; }
  virtual const ElementPropertyTransition* AsTransition() const
  {
    return nullptr;
  }

  // KeyframeEffectReadOnly interface
  static already_AddRefed<KeyframeEffectReadOnly>
  Constructor(const GlobalObject& aGlobal,
              Element* aTarget,
              const Optional<JS::Handle<JSObject*>>& aFrames,
              const Optional<double>& aOptions,
              ErrorResult& aRv);
  Element* GetTarget() const {
    // Currently we never return animations from the API whose effect
    // targets a pseudo-element so this should never be called when
    // mPseudoType is not 'none' (see bug 1174575).
    MOZ_ASSERT(mPseudoType == nsCSSPseudoElements::ePseudo_NotPseudoElement,
               "Requesting the target of a KeyframeEffect that targets a"
               " pseudo-element is not yet supported.");
    return mTarget;
  }
  void GetFrames(JSContext*& aCx,
                 nsTArray<JSObject*>& aResult,
                 ErrorResult& aRv);

  // Temporary workaround to return both the target element and pseudo-type
  // until we implement PseudoElement (bug 1174575).
  void GetTarget(Element*& aTarget,
                 nsCSSPseudoElements::Type& aPseudoType) const {
    aTarget = mTarget;
    aPseudoType = mPseudoType;
  }

  const AnimationTiming& Timing() const {
    return mTiming;
  }
  AnimationTiming& Timing() {
    return mTiming;
  }
  void SetTiming(const AnimationTiming& aTiming);

  Nullable<TimeDuration> GetLocalTime() const;

  // This function takes as input the timing parameters of an animation and
  // returns the computed timing at the specified local time.
  //
  // The local time may be null in which case only static parameters such as the
  // active duration are calculated. All other members of the returned object
  // are given a null/initial value.
  //
  // This function returns a null mProgress member of the return value
  // if the animation should not be run
  // (because it is not currently active and is not filling at this time).
  static ComputedTiming
  GetComputedTimingAt(const Nullable<TimeDuration>& aLocalTime,
                      const AnimationTiming& aTiming);

  // Shortcut for that gets the computed timing using the current local time as
  // calculated from the timeline time.
  ComputedTiming
  GetComputedTiming(const AnimationTiming* aTiming = nullptr) const
  {
    return GetComputedTimingAt(GetLocalTime(), aTiming ? *aTiming : mTiming);
  }

  void
  GetComputedTimingAsDict(ComputedTimingProperties& aRetVal) const override;

  // Return the duration of the active interval for the given timing parameters.
  static StickyTimeDuration
  ActiveDuration(const AnimationTiming& aTiming);

  bool IsInPlay() const;
  bool IsCurrent() const;
  bool IsInEffect() const;

  void SetAnimation(Animation* aAnimation);

  const AnimationProperty*
  GetAnimationOfProperty(nsCSSProperty aProperty) const;
  bool HasAnimationOfProperty(nsCSSProperty aProperty) const {
    return GetAnimationOfProperty(aProperty) != nullptr;
  }
  bool HasAnimationOfProperties(const nsCSSProperty* aProperties,
                                size_t aPropertyCount) const;
  const InfallibleTArray<AnimationProperty>& Properties() const {
    return mProperties;
  }
  InfallibleTArray<AnimationProperty>& Properties() {
    return mProperties;
  }

  // Updates |aStyleRule| with the animation values produced by this
  // AnimationEffect for the current time except any properties already
  // contained in |aSetProperties|.
  // Any updated properties are added to |aSetProperties|.
  void ComposeStyle(RefPtr<AnimValuesStyleRule>& aStyleRule,
                    nsCSSPropertySet& aSetProperties);
  bool IsRunningOnCompositor() const;
  void SetIsRunningOnCompositor(nsCSSProperty aProperty, bool aIsRunning);

protected:
  virtual ~KeyframeEffectReadOnly();
  void ResetIsRunningOnCompositor();

  static AnimationTiming ConvertKeyframeEffectOptions(
      const Optional<double>& aOptions);
  static void BuildAnimationPropertyList(
      JSContext* aCx,
      Element* aTarget,
      const Optional<JS::Handle<JSObject*>>& aFrames,
      InfallibleTArray<AnimationProperty>& aResult,
      ErrorResult& aRv);

  nsCOMPtr<Element> mTarget;
  RefPtr<Animation> mAnimation;

  AnimationTiming mTiming;
  nsCSSPseudoElements::Type mPseudoType;

  InfallibleTArray<AnimationProperty> mProperties;

  // Parallel array corresponding to CommonAnimationManager::sLayerAnimationInfo
  // such that mIsPropertyRunningOnCompositor[x] is true only if this effect has
  // an animation of CommonAnimationManager::sLayerAnimationInfo[x].mProperty
  // that is currently running on the compositor.
  //
  // Note that when the owning Animation requests a non-throttled restyle, in
  // between calling RequestRestyle on its AnimationCollection and when the
  // restyle is performed, this member may temporarily become false even if
  // the animation remains on the layer after the restyle.
  bool mIsPropertyRunningOnCompositor[LayerAnimationInfo::kRecords];
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_KeyframeEffect_h
