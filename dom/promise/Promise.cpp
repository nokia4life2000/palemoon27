/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/Promise.h"

#include "js/Debug.h"

#include "mozilla/Atomics.h"
#include "mozilla/CycleCollectedJSRuntime.h"
#include "mozilla/OwningNonNull.h"
#include "mozilla/Preferences.h"

#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/DOMError.h"
#include "mozilla/dom/DOMException.h"
#include "mozilla/dom/MediaStreamError.h"
#include "mozilla/dom/PromiseBinding.h"
#include "mozilla/dom/ScriptSettings.h"

#include "jsfriendapi.h"
#include "js/StructuredClone.h"
#include "nsContentUtils.h"
#include "nsGlobalWindow.h"
#include "nsIScriptObjectPrincipal.h"
#include "nsJSEnvironment.h"
#include "nsJSPrincipals.h"
#include "nsJSUtils.h"
#include "nsPIDOMWindow.h"
#include "PromiseCallback.h"
#include "PromiseDebugging.h"
#include "PromiseNativeHandler.h"
#include "PromiseWorkerProxy.h"
#include "WorkerPrivate.h"
#include "WorkerRunnable.h"
#include "xpcpublic.h"
#ifdef MOZ_CRASHREPORTER
#include "nsExceptionHandler.h"
#endif

namespace mozilla {
namespace dom {

namespace {
// Generator used by Promise::GetID.
Atomic<uintptr_t> gIDGenerator(0);
} // namespace

using namespace workers;

// This class processes the promise's callbacks with promise's result.
class PromiseReactionJob final : public nsRunnable
{
public:
  PromiseReactionJob(Promise* aPromise,
                     PromiseCallback* aCallback,
                     const JS::Value& aValue)
    : mPromise(aPromise)
    , mCallback(aCallback)
    , mValue(CycleCollectedJSRuntime::Get()->Runtime(), aValue)
  {
    MOZ_ASSERT(aPromise);
    MOZ_ASSERT(aCallback);
    MOZ_COUNT_CTOR(PromiseReactionJob);
  }

  virtual
  ~PromiseReactionJob()
  {
    NS_ASSERT_OWNINGTHREAD(PromiseReactionJob);
    MOZ_COUNT_DTOR(PromiseReactionJob);
  }

protected:
  NS_IMETHOD
  Run() override
  {
    NS_ASSERT_OWNINGTHREAD(PromiseReactionJob);
    ThreadsafeAutoJSContext cx;
    JS::Rooted<JSObject*> wrapper(cx, mPromise->GetWrapper());
    MOZ_ASSERT(wrapper); // It was preserved!
    JSAutoCompartment ac(cx, wrapper);

    JS::Rooted<JS::Value> value(cx, mValue);
    if (!MaybeWrapValue(cx, &value)) {
      NS_WARNING("Failed to wrap value into the right compartment.");
      JS_ClearPendingException(cx);
      return NS_OK;
    }

    JS::Rooted<JSObject*> asyncStack(cx, mPromise->mAllocationStack);
    JS::Rooted<JSString*> asyncCause(cx, JS_NewStringCopyZ(cx, "Promise"));
    if (!asyncCause) {
      JS_ClearPendingException(cx);
      return NS_ERROR_OUT_OF_MEMORY;
    }

    {
      Maybe<JS::AutoSetAsyncStackForNewCalls> sas;
      if (asyncStack) {
        sas.emplace(cx, asyncStack, asyncCause);
      }
      mCallback->Call(cx, value);
    }

    return NS_OK;
  }

private:
  RefPtr<Promise> mPromise;
  RefPtr<PromiseCallback> mCallback;
  JS::PersistentRooted<JS::Value> mValue;
  NS_DECL_OWNINGTHREAD;
};

enum {
  SLOT_PROMISE = 0,
  SLOT_DATA
};

/*
 * Utilities for thenable callbacks.
 *
 * A thenable is a { then: function(resolve, reject) { } }.
 * `then` is called with a resolve and reject callback pair.
 * Since only one of these should be called at most once (first call wins), the
 * two keep a reference to each other in SLOT_DATA. When either of them is
 * called, the references are cleared. Further calls are ignored.
 */
namespace {
void
LinkThenableCallables(JSContext* aCx, JS::Handle<JSObject*> aResolveFunc,
                      JS::Handle<JSObject*> aRejectFunc)
{
  js::SetFunctionNativeReserved(aResolveFunc, SLOT_DATA,
                                JS::ObjectValue(*aRejectFunc));
  js::SetFunctionNativeReserved(aRejectFunc, SLOT_DATA,
                                JS::ObjectValue(*aResolveFunc));
}

/*
 * Returns false if callback was already called before, otherwise breaks the
 * links and returns true.
 */
bool
MarkAsCalledIfNotCalledBefore(JSContext* aCx, JS::Handle<JSObject*> aFunc)
{
  JS::Value otherFuncVal = js::GetFunctionNativeReserved(aFunc, SLOT_DATA);

  if (!otherFuncVal.isObject()) {
    return false;
  }

  JSObject* otherFuncObj = &otherFuncVal.toObject();
  MOZ_ASSERT(js::GetFunctionNativeReserved(otherFuncObj, SLOT_DATA).isObject());

  // Break both references.
  js::SetFunctionNativeReserved(aFunc, SLOT_DATA, JS::UndefinedValue());
  js::SetFunctionNativeReserved(otherFuncObj, SLOT_DATA, JS::UndefinedValue());

  return true;
}

Promise*
GetPromise(JSContext* aCx, JS::Handle<JSObject*> aFunc)
{
  JS::Value promiseVal = js::GetFunctionNativeReserved(aFunc, SLOT_PROMISE);

  MOZ_ASSERT(promiseVal.isObject());

  Promise* promise;
  UNWRAP_OBJECT(Promise, &promiseVal.toObject(), promise);
  return promise;
}
} // namespace

// Runnable to resolve thenables.
// Equivalent to the specification's ResolvePromiseViaThenableTask.
class PromiseResolveThenableJob final : public nsRunnable
{
public:
  PromiseResolveThenableJob(Promise* aPromise,
                            JS::Handle<JSObject*> aThenable,
                            PromiseInit* aThen)
    : mPromise(aPromise)
    , mThenable(CycleCollectedJSRuntime::Get()->Runtime(), aThenable)
    , mThen(aThen)
  {
    MOZ_ASSERT(aPromise);
    MOZ_COUNT_CTOR(PromiseResolveThenableJob);
  }

  virtual
  ~PromiseResolveThenableJob()
  {
    NS_ASSERT_OWNINGTHREAD(PromiseResolveThenableJob);
    MOZ_COUNT_DTOR(PromiseResolveThenableJob);
  }

protected:
  NS_IMETHOD
  Run() override
  {
    NS_ASSERT_OWNINGTHREAD(PromiseResolveThenableJob);
    ThreadsafeAutoJSContext cx;
    JS::Rooted<JSObject*> wrapper(cx, mPromise->GetWrapper());
    MOZ_ASSERT(wrapper); // It was preserved!
    // If we ever change which compartment we're working in here, make sure to
    // fix the fast-path for resolved-with-a-Promise in ResolveInternal.
    JSAutoCompartment ac(cx, wrapper);

    JS::Rooted<JSObject*> resolveFunc(cx,
      mPromise->CreateThenableFunction(cx, mPromise, PromiseCallback::Resolve));

    if (!resolveFunc) {
      mPromise->HandleException(cx);
      return NS_OK;
    }

    JS::Rooted<JSObject*> rejectFunc(cx,
      mPromise->CreateThenableFunction(cx, mPromise, PromiseCallback::Reject));
    if (!rejectFunc) {
      mPromise->HandleException(cx);
      return NS_OK;
    }

    LinkThenableCallables(cx, resolveFunc, rejectFunc);

    ErrorResult rv;

    JS::Rooted<JSObject*> rootedThenable(cx, mThenable);

    mThen->Call(rootedThenable, resolveFunc, rejectFunc, rv,
                "promise thenable", CallbackObject::eRethrowExceptions,
                mPromise->Compartment());

    rv.WouldReportJSException();
    if (rv.Failed()) {
      JS::Rooted<JS::Value> exn(cx);
      if (rv.IsJSException()) {
        rv.StealJSException(cx, &exn);
      } else {
        // Convert the ErrorResult to a JS exception object that we can reject
        // ourselves with.  This will be exactly the exception that would get
        // thrown from a binding method whose ErrorResult ended up with
        // whatever is on "rv" right now.
        JSAutoCompartment ac(cx, mPromise->GlobalJSObject());
        DebugOnly<bool> conversionResult = ToJSValue(cx, rv, &exn);
        MOZ_ASSERT(conversionResult);
      }

      bool couldMarkAsCalled = MarkAsCalledIfNotCalledBefore(cx, resolveFunc);

      // If we could mark as called, neither of the callbacks had been called
      // when the exception was thrown. So we can reject the Promise.
      if (couldMarkAsCalled) {
        bool ok = JS_WrapValue(cx, &exn);
        MOZ_ASSERT(ok);
        if (!ok) {
          NS_WARNING("Failed to wrap value into the right compartment.");
        }

        mPromise->RejectInternal(cx, exn);
      }
      // At least one of resolveFunc or rejectFunc have been called, so ignore
      // the exception. FIXME(nsm): This should be reported to the error
      // console though, for debugging.
    }

    return rv.StealNSResult();
  }

private:
  RefPtr<Promise> mPromise;
  JS::PersistentRooted<JSObject*> mThenable;
  RefPtr<PromiseInit> mThen;
  NS_DECL_OWNINGTHREAD;
};

// Fast version of PromiseResolveThenableJob for use in the cases when we know we're
// calling the canonical Promise.prototype.then on an actual DOM Promise.  In
// that case we can just bypass the jumping into and out of JS and call
// AppendCallbacks on that promise directly.
class FastPromiseResolveThenableJob final : public nsRunnable
{
public:
  FastPromiseResolveThenableJob(PromiseCallback* aResolveCallback,
                                PromiseCallback* aRejectCallback,
                                Promise* aNextPromise)
    : mResolveCallback(aResolveCallback)
    , mRejectCallback(aRejectCallback)
    , mNextPromise(aNextPromise)
  {
    MOZ_ASSERT(aResolveCallback);
    MOZ_ASSERT(aRejectCallback);
    MOZ_ASSERT(aNextPromise);
    MOZ_COUNT_CTOR(FastPromiseResolveThenableJob);
  }

  virtual
  ~FastPromiseResolveThenableJob()
  {
    NS_ASSERT_OWNINGTHREAD(FastPromiseResolveThenableJob);
    MOZ_COUNT_DTOR(FastPromiseResolveThenableJob);
  }

protected:
  NS_IMETHOD
  Run() override
  {
    NS_ASSERT_OWNINGTHREAD(FastPromiseResolveThenableJob);
    mNextPromise->AppendCallbacks(mResolveCallback, mRejectCallback);
    return NS_OK;
  }

private:
  RefPtr<PromiseCallback> mResolveCallback;
  RefPtr<PromiseCallback> mRejectCallback;
  RefPtr<Promise> mNextPromise;
};

// Promise

NS_IMPL_CYCLE_COLLECTION_CLASS(Promise)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(Promise)
#if defined(DOM_PROMISE_DEPRECATED_REPORTING)
  tmp->MaybeReportRejectedOnce();
#else
  tmp->mResult = JS::UndefinedValue();
#endif // defined(DOM_PROMISE_DEPRECATED_REPORTING)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mGlobal)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mResolveCallbacks)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mRejectCallbacks)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(Promise)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mGlobal)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mResolveCallbacks)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mRejectCallbacks)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(Promise)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JSVAL_MEMBER_CALLBACK(mResult)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mAllocationStack)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mRejectionStack)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mFullfillmentStack)
  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_BEGIN(Promise)
  if (tmp->IsBlack()) {
    if (tmp->mResult.isObject()) {
      JS::ExposeObjectToActiveJS(&(tmp->mResult.toObject()));
    }
    if (tmp->mAllocationStack) {
      JS::ExposeObjectToActiveJS(tmp->mAllocationStack);
    }
    if (tmp->mRejectionStack) {
      JS::ExposeObjectToActiveJS(tmp->mRejectionStack);
    }
    if (tmp->mFullfillmentStack) {
      JS::ExposeObjectToActiveJS(tmp->mFullfillmentStack);
    }
    return true;
  }
NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_END

NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_IN_CC_BEGIN(Promise)
  return tmp->IsBlackAndDoesNotNeedTracing(tmp);
NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_IN_CC_END

NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_THIS_BEGIN(Promise)
  return tmp->IsBlack();
NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_THIS_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(Promise)
NS_IMPL_CYCLE_COLLECTING_RELEASE(Promise)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(Promise)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
  NS_INTERFACE_MAP_ENTRY(Promise)
NS_INTERFACE_MAP_END

Promise::Promise(nsIGlobalObject* aGlobal)
  : mGlobal(aGlobal)
  , mResult(JS::UndefinedValue())
  , mAllocationStack(nullptr)
  , mRejectionStack(nullptr)
  , mFullfillmentStack(nullptr)
  , mState(Pending)
#if defined(DOM_PROMISE_DEPRECATED_REPORTING)
  , mHadRejectCallback(false)
#endif // defined(DOM_PROMISE_DEPRECATED_REPORTING)
  , mTaskPending(false)
  , mResolvePending(false)
  , mIsLastInChain(true)
  , mWasNotifiedAsUncaught(false)
  , mID(0)
{
  MOZ_ASSERT(mGlobal);

  mozilla::HoldJSObjects(this);

  mCreationTimestamp = TimeStamp::Now();
}

Promise::~Promise()
{
#if defined(DOM_PROMISE_DEPRECATED_REPORTING)
  MaybeReportRejectedOnce();
#endif // defined(DOM_PROMISE_DEPRECATED_REPORTING)
  mozilla::DropJSObjects(this);
}

JSObject*
Promise::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return PromiseBinding::Wrap(aCx, this, aGivenProto);
}

already_AddRefed<Promise>
Promise::Create(nsIGlobalObject* aGlobal, ErrorResult& aRv,
                JS::Handle<JSObject*> aDesiredProto)
{
  RefPtr<Promise> p = new Promise(aGlobal);
  p->CreateWrapper(aDesiredProto, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  return p.forget();
}

void
Promise::CreateWrapper(JS::Handle<JSObject*> aDesiredProto, ErrorResult& aRv)
{
  AutoJSAPI jsapi;
  if (!jsapi.Init(mGlobal)) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return;
  }
  JSContext* cx = jsapi.cx();

  JS::Rooted<JS::Value> wrapper(cx);
  if (!GetOrCreateDOMReflector(cx, this, &wrapper, aDesiredProto)) {
    JS_ClearPendingException(cx);
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }

  dom::PreserveWrapper(this);

  // Now grab our allocation stack
  if (!CaptureStack(cx, mAllocationStack)) {
    JS_ClearPendingException(cx);
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }

  JS::RootedObject obj(cx, &wrapper.toObject());
  JS::dbg::onNewPromise(cx, obj);
}

void
Promise::MaybeResolve(JSContext* aCx,
                      JS::Handle<JS::Value> aValue)
{
  MaybeResolveInternal(aCx, aValue);
}

void
Promise::MaybeReject(JSContext* aCx,
                     JS::Handle<JS::Value> aValue)
{
  MaybeRejectInternal(aCx, aValue);
}

void
Promise::MaybeReject(const RefPtr<MediaStreamError>& aArg) {
  MaybeSomething(aArg, &Promise::MaybeReject);
}

bool
Promise::PerformMicroTaskCheckpoint()
{
  CycleCollectedJSRuntime* runtime = CycleCollectedJSRuntime::Get();
  std::queue<nsCOMPtr<nsIRunnable>>& microtaskQueue =
    runtime->GetPromiseMicroTaskQueue();

  if (microtaskQueue.empty()) {
    return false;
  }

  Maybe<AutoSafeJSContext> cx;
  if (NS_IsMainThread()) {
    cx.emplace();
  }

  do {
    nsCOMPtr<nsIRunnable> runnable = microtaskQueue.front();
    MOZ_ASSERT(runnable);

    // This function can re-enter, so we remove the element before calling.
    microtaskQueue.pop();
    nsresult rv = runnable->Run();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return false;
    }
    if (cx.isSome()) {
      JS_CheckForInterrupt(cx.ref());
    }
    runtime->AfterProcessMicrotask();
  } while (!microtaskQueue.empty());

  return true;
}

/* static */ bool
Promise::JSCallback(JSContext* aCx, unsigned aArgc, JS::Value* aVp)
{
  JS::CallArgs args = CallArgsFromVp(aArgc, aVp);

  JS::Rooted<JS::Value> v(aCx,
                          js::GetFunctionNativeReserved(&args.callee(),
                                                        SLOT_PROMISE));
  MOZ_ASSERT(v.isObject());

  Promise* promise;
  if (NS_FAILED(UNWRAP_OBJECT(Promise, &v.toObject(), promise))) {
    return Throw(aCx, NS_ERROR_UNEXPECTED);
  }

  v = js::GetFunctionNativeReserved(&args.callee(), SLOT_DATA);
  PromiseCallback::Task task = static_cast<PromiseCallback::Task>(v.toInt32());

  if (task == PromiseCallback::Resolve) {
    if (!promise->CaptureStack(aCx, promise->mFullfillmentStack)) {
      return false;
    }
    promise->MaybeResolveInternal(aCx, args.get(0));
  } else {
    promise->MaybeRejectInternal(aCx, args.get(0));
    if (!promise->CaptureStack(aCx, promise->mRejectionStack)) {
      return false;
    }
  }

  args.rval().setUndefined();
  return true;
}

/*
 * Common bits of (JSCallbackThenableResolver/JSCallbackThenableRejecter).
 * Resolves/rejects the Promise if it is ok to do so, based on whether either of
 * the callbacks have been called before or not.
 */
/* static */ bool
Promise::ThenableResolverCommon(JSContext* aCx, uint32_t aTask,
                                unsigned aArgc, JS::Value* aVp)
{
  JS::CallArgs args = CallArgsFromVp(aArgc, aVp);
  JS::Rooted<JSObject*> thisFunc(aCx, &args.callee());
  if (!MarkAsCalledIfNotCalledBefore(aCx, thisFunc)) {
    // A function from this pair has been called before.
    args.rval().setUndefined();
    return true;
  }

  Promise* promise = GetPromise(aCx, thisFunc);
  MOZ_ASSERT(promise);

  if (aTask == PromiseCallback::Resolve) {
    promise->ResolveInternal(aCx, args.get(0));
  } else {
    promise->RejectInternal(aCx, args.get(0));
  }

  args.rval().setUndefined();
  return true;
}

/* static */ bool
Promise::JSCallbackThenableResolver(JSContext* aCx,
                                    unsigned aArgc, JS::Value* aVp)
{
  return ThenableResolverCommon(aCx, PromiseCallback::Resolve, aArgc, aVp);
}

/* static */ bool
Promise::JSCallbackThenableRejecter(JSContext* aCx,
                                    unsigned aArgc, JS::Value* aVp)
{
  return ThenableResolverCommon(aCx, PromiseCallback::Reject, aArgc, aVp);
}

/* static */ JSObject*
Promise::CreateFunction(JSContext* aCx, Promise* aPromise, int32_t aTask)
{
  JSFunction* func = js::NewFunctionWithReserved(aCx, JSCallback,
                                                 1 /* nargs */, 0 /* flags */,
                                                 nullptr);
  if (!func) {
    return nullptr;
  }

  JS::Rooted<JSObject*> obj(aCx, JS_GetFunctionObject(func));

  JS::Rooted<JS::Value> promiseObj(aCx);
  if (!dom::GetOrCreateDOMReflector(aCx, aPromise, &promiseObj)) {
    return nullptr;
  }

  js::SetFunctionNativeReserved(obj, SLOT_PROMISE, promiseObj);
  js::SetFunctionNativeReserved(obj, SLOT_DATA, JS::Int32Value(aTask));

  return obj;
}

/* static */ JSObject*
Promise::CreateThenableFunction(JSContext* aCx, Promise* aPromise, uint32_t aTask)
{
  JSNative whichFunc =
    aTask == PromiseCallback::Resolve ? JSCallbackThenableResolver :
                                        JSCallbackThenableRejecter ;

  JSFunction* func = js::NewFunctionWithReserved(aCx, whichFunc,
                                                 1 /* nargs */, 0 /* flags */,
                                                 nullptr);
  if (!func) {
    return nullptr;
  }

  JS::Rooted<JSObject*> obj(aCx, JS_GetFunctionObject(func));

  JS::Rooted<JS::Value> promiseObj(aCx);
  if (!dom::GetOrCreateDOMReflector(aCx, aPromise, &promiseObj)) {
    return nullptr;
  }

  js::SetFunctionNativeReserved(obj, SLOT_PROMISE, promiseObj);

  return obj;
}

/* static */ already_AddRefed<Promise>
Promise::Constructor(const GlobalObject& aGlobal, PromiseInit& aInit,
                     ErrorResult& aRv, JS::Handle<JSObject*> aDesiredProto)
{
  nsCOMPtr<nsIGlobalObject> global;
  global = do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  RefPtr<Promise> promise = Create(global, aRv, aDesiredProto);
  if (aRv.Failed()) {
    return nullptr;
  }

  promise->CallInitFunction(aGlobal, aInit, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  return promise.forget();
}

void
Promise::CallInitFunction(const GlobalObject& aGlobal,
                          PromiseInit& aInit, ErrorResult& aRv)
{
  JSContext* cx = aGlobal.Context();

  JS::Rooted<JSObject*> resolveFunc(cx,
                                    CreateFunction(cx, this,
                                                   PromiseCallback::Resolve));
  if (!resolveFunc) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return;
  }

  JS::Rooted<JSObject*> rejectFunc(cx,
                                   CreateFunction(cx, this,
                                                  PromiseCallback::Reject));
  if (!rejectFunc) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return;
  }

  aInit.Call(resolveFunc, rejectFunc, aRv, "promise initializer",
             CallbackObject::eRethrowExceptions, Compartment());
  aRv.WouldReportJSException();

  if (aRv.IsJSException()) {
    JS::Rooted<JS::Value> value(cx);
    aRv.StealJSException(cx, &value);

    // we want the same behavior as this JS implementation:
    // function Promise(arg) { try { arg(a, b); } catch (e) { this.reject(e); }}
    if (!JS_WrapValue(cx, &value)) {
      aRv.Throw(NS_ERROR_UNEXPECTED);
      return;
    }

    MaybeRejectInternal(cx, value);
  }

  // Else aRv is an error.  We _could_ reject ourselves with that error, but
  // we're just going to propagate aRv out to the binding code, which will then
  // throw us away and create a new promise rejected with the error on aRv.  So
  // there's no need to worry about rejecting ourselves here; the bindings
  // will do the right thing.
}

/* static */ already_AddRefed<Promise>
Promise::Resolve(const GlobalObject& aGlobal,
                 JS::Handle<JS::Value> aValue, ErrorResult& aRv)
{
  // If a Promise was passed, just return it.
  if (aValue.isObject()) {
    JS::Rooted<JSObject*> valueObj(aGlobal.Context(), &aValue.toObject());
    Promise* nextPromise;
    nsresult rv = UNWRAP_OBJECT(Promise, valueObj, nextPromise);

    if (NS_SUCCEEDED(rv)) {
      RefPtr<Promise> addRefed = nextPromise;
      return addRefed.forget();
    }
  }

  nsCOMPtr<nsIGlobalObject> global =
    do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  RefPtr<Promise> p = Resolve(global, aGlobal.Context(), aValue, aRv);
  if (p) {
    p->mFullfillmentStack = p->mAllocationStack;
  }
  return p.forget();
}

/* static */ already_AddRefed<Promise>
Promise::Resolve(nsIGlobalObject* aGlobal, JSContext* aCx,
                 JS::Handle<JS::Value> aValue, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Create(aGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  promise->MaybeResolveInternal(aCx, aValue);
  return promise.forget();
}

/* static */ already_AddRefed<Promise>
Promise::Reject(const GlobalObject& aGlobal,
                JS::Handle<JS::Value> aValue, ErrorResult& aRv)
{
  nsCOMPtr<nsIGlobalObject> global =
    do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  RefPtr<Promise> p = Reject(global, aGlobal.Context(), aValue, aRv);
  if (p) {
    p->mRejectionStack = p->mAllocationStack;
  }
  return p.forget();
}

/* static */ already_AddRefed<Promise>
Promise::Reject(nsIGlobalObject* aGlobal, JSContext* aCx,
                JS::Handle<JS::Value> aValue, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Create(aGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  promise->MaybeRejectInternal(aCx, aValue);
  return promise.forget();
}

already_AddRefed<Promise>
Promise::Then(JSContext* aCx, AnyCallback* aResolveCallback,
              AnyCallback* aRejectCallback, ErrorResult& aRv)
{
  RefPtr<Promise> promise = Create(GetParentObject(), aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  JS::Rooted<JSObject*> global(aCx, JS::CurrentGlobalOrNull(aCx));

  RefPtr<PromiseCallback> resolveCb =
    PromiseCallback::Factory(promise, global, aResolveCallback,
                             PromiseCallback::Resolve);

  RefPtr<PromiseCallback> rejectCb =
    PromiseCallback::Factory(promise, global, aRejectCallback,
                             PromiseCallback::Reject);

  AppendCallbacks(resolveCb, rejectCb);

  return promise.forget();
}

already_AddRefed<Promise>
Promise::Catch(JSContext* aCx, AnyCallback* aRejectCallback, ErrorResult& aRv)
{
  RefPtr<AnyCallback> resolveCb;
  return Then(aCx, resolveCb, aRejectCallback, aRv);
}

/**
 * The CountdownHolder class encapsulates Promise.all countdown functions and
 * the countdown holder parts of the Promises spec. It maintains the result
 * array and AllResolveElementFunctions use SetValue() to set the array indices.
 */
class CountdownHolder final : public nsISupports
{
public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(CountdownHolder)

  CountdownHolder(const GlobalObject& aGlobal, Promise* aPromise,
                  uint32_t aCountdown)
    : mPromise(aPromise), mCountdown(aCountdown)
  {
    MOZ_ASSERT(aCountdown != 0);
    JSContext* cx = aGlobal.Context();

    // The only time aGlobal.Context() and aGlobal.Get() are not
    // same-compartment is when we're called via Xrays, and in that situation we
    // in fact want to create the array in the callee compartment

    JSAutoCompartment ac(cx, aGlobal.Get());
    mValues = JS_NewArrayObject(cx, aCountdown);
    mozilla::HoldJSObjects(this);
  }

private:
  ~CountdownHolder()
  {
    mozilla::DropJSObjects(this);
  }

public:
  void SetValue(uint32_t index, const JS::Handle<JS::Value> aValue)
  {
    MOZ_ASSERT(mCountdown > 0);

    ThreadsafeAutoSafeJSContext cx;
    JSAutoCompartment ac(cx, mValues);
    {

      AutoDontReportUncaught silenceReporting(cx);
      JS::Rooted<JS::Value> value(cx, aValue);
      JS::Rooted<JSObject*> values(cx, mValues);
      if (!JS_WrapValue(cx, &value) ||
          !JS_DefineElement(cx, values, index, value, JSPROP_ENUMERATE)) {
        MOZ_ASSERT(JS_IsExceptionPending(cx));
        JS::Rooted<JS::Value> exn(cx);
        JS_GetPendingException(cx, &exn);

        mPromise->MaybeReject(cx, exn);
      }
    }

    --mCountdown;
    if (mCountdown == 0) {
      JS::Rooted<JS::Value> result(cx, JS::ObjectValue(*mValues));
      mPromise->MaybeResolve(cx, result);
    }
  }

private:
  RefPtr<Promise> mPromise;
  uint32_t mCountdown;
  JS::Heap<JSObject*> mValues;
};

NS_IMPL_CYCLE_COLLECTING_ADDREF(CountdownHolder)
NS_IMPL_CYCLE_COLLECTING_RELEASE(CountdownHolder)
NS_IMPL_CYCLE_COLLECTION_CLASS(CountdownHolder)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(CountdownHolder)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(CountdownHolder)
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mValues)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(CountdownHolder)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mPromise)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(CountdownHolder)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mPromise)
  tmp->mValues = nullptr;
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

/**
 * An AllResolveElementFunction is the per-promise
 * part of the Promise.all() algorithm.
 * Every Promise in the handler is handed an instance of this as a resolution
 * handler and it sets the relevant index in the CountdownHolder.
 */
class AllResolveElementFunction final : public PromiseNativeHandler
{
public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_CLASS(AllResolveElementFunction)

  AllResolveElementFunction(CountdownHolder* aHolder, uint32_t aIndex)
    : mCountdownHolder(aHolder), mIndex(aIndex)
  {
    MOZ_ASSERT(aHolder);
  }

  void
  ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) override
  {
    mCountdownHolder->SetValue(mIndex, aValue);
  }

  void
  RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) override
  {
    // Should never be attached to Promise as a reject handler.
    MOZ_CRASH("AllResolveElementFunction should never be attached to a Promise's reject handler!");
  }

private:
  ~AllResolveElementFunction()
  {
  }

  RefPtr<CountdownHolder> mCountdownHolder;
  uint32_t mIndex;
};

NS_IMPL_CYCLE_COLLECTING_ADDREF(AllResolveElementFunction)
NS_IMPL_CYCLE_COLLECTING_RELEASE(AllResolveElementFunction)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(AllResolveElementFunction)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTION(AllResolveElementFunction, mCountdownHolder)

/* static */ already_AddRefed<Promise>
Promise::All(const GlobalObject& aGlobal,
             const Sequence<JS::Value>& aIterable, ErrorResult& aRv)
{
  JSContext* cx = aGlobal.Context();

  nsTArray<RefPtr<Promise>> promiseList;

  for (uint32_t i = 0; i < aIterable.Length(); ++i) {
    JS::Rooted<JS::Value> value(cx, aIterable.ElementAt(i));
    RefPtr<Promise> nextPromise = Promise::Resolve(aGlobal, value, aRv);

    MOZ_ASSERT(!aRv.Failed());

    promiseList.AppendElement(Move(nextPromise));
  }

  return Promise::All(aGlobal, promiseList, aRv);
}

/* static */ already_AddRefed<Promise>
Promise::All(const GlobalObject& aGlobal,
             const nsTArray<RefPtr<Promise>>& aPromiseList, ErrorResult& aRv)
{
  nsCOMPtr<nsIGlobalObject> global =
    do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  JSContext* cx = aGlobal.Context();

  if (aPromiseList.IsEmpty()) {
    JS::Rooted<JSObject*> empty(cx, JS_NewArrayObject(cx, 0));
    if (!empty) {
      aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
      return nullptr;
    }
    JS::Rooted<JS::Value> value(cx, JS::ObjectValue(*empty));
    // We know "value" is not a promise, so call the Resolve function
    // that doesn't have to check for that.
    return Promise::Resolve(global, cx, value, aRv);
  }

  RefPtr<Promise> promise = Create(global, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  RefPtr<CountdownHolder> holder =
    new CountdownHolder(aGlobal, promise, aPromiseList.Length());

  JS::Rooted<JSObject*> obj(cx, JS::CurrentGlobalOrNull(cx));
  if (!obj) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  RefPtr<PromiseCallback> rejectCb = new RejectPromiseCallback(promise, obj);

  for (uint32_t i = 0; i < aPromiseList.Length(); ++i) {
    RefPtr<PromiseNativeHandler> resolveHandler =
      new AllResolveElementFunction(holder, i);

    RefPtr<PromiseCallback> resolveCb =
      new NativePromiseCallback(resolveHandler, Resolved);

    // Every promise gets its own resolve callback, which will set the right
    // index in the array to the resolution value.
    aPromiseList[i]->AppendCallbacks(resolveCb, rejectCb);
  }

  return promise.forget();
}

/* static */ already_AddRefed<Promise>
Promise::Race(const GlobalObject& aGlobal,
              const Sequence<JS::Value>& aIterable, ErrorResult& aRv)
{
  nsCOMPtr<nsIGlobalObject> global =
    do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  JSContext* cx = aGlobal.Context();

  JS::Rooted<JSObject*> obj(cx, JS::CurrentGlobalOrNull(cx));
  if (!obj) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  RefPtr<Promise> promise = Create(global, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  RefPtr<PromiseCallback> resolveCb =
    new ResolvePromiseCallback(promise, obj);

  RefPtr<PromiseCallback> rejectCb = new RejectPromiseCallback(promise, obj);

  for (uint32_t i = 0; i < aIterable.Length(); ++i) {
    JS::Rooted<JS::Value> value(cx, aIterable.ElementAt(i));
    RefPtr<Promise> nextPromise = Promise::Resolve(aGlobal, value, aRv);
    // According to spec, Resolve can throw, but our implementation never does.
    // Well it does when window isn't passed on the main thread, but that is an
    // implementation detail which should never be reached since we are checking
    // for window above. Remove this when subclassing is supported.
    MOZ_ASSERT(!aRv.Failed());
    nextPromise->AppendCallbacks(resolveCb, rejectCb);
  }

  return promise.forget();
}

void
Promise::AppendNativeHandler(PromiseNativeHandler* aRunnable)
{
  RefPtr<PromiseCallback> resolveCb =
    new NativePromiseCallback(aRunnable, Resolved);

  RefPtr<PromiseCallback> rejectCb =
    new NativePromiseCallback(aRunnable, Rejected);

  AppendCallbacks(resolveCb, rejectCb);
}

JSObject*
Promise::GlobalJSObject() const
{
  return mGlobal->GetGlobalJSObject();
}

JSCompartment*
Promise::Compartment() const
{
  return js::GetObjectCompartment(GlobalJSObject());
}

void
Promise::AppendCallbacks(PromiseCallback* aResolveCallback,
                         PromiseCallback* aRejectCallback)
{
  if (mGlobal->IsDying()) {
    return;
  }

  MOZ_ASSERT(aResolveCallback);
  MOZ_ASSERT(aRejectCallback);

  if (mIsLastInChain && mState == PromiseState::Rejected) {
    // This rejection is now consumed.
    PromiseDebugging::AddConsumedRejection(*this);
    // Note that we may not have had the opportunity to call
    // RunResolveTask() yet, so we may never have called
    // `PromiseDebugging:AddUncaughtRejection`.
  }
  mIsLastInChain = false;

#if defined(DOM_PROMISE_DEPRECATED_REPORTING)
  // Now that there is a callback, we don't need to report anymore.
  mHadRejectCallback = true;
  RemoveFeature();
#endif // defined(DOM_PROMISE_DEPRECATED_REPORTING)

  mResolveCallbacks.AppendElement(aResolveCallback);
  mRejectCallbacks.AppendElement(aRejectCallback);

  // If promise's state is fulfilled, queue a task to process our fulfill
  // callbacks with promise's result. If promise's state is rejected, queue a
  // task to process our reject callbacks with promise's result.
  if (mState != Pending) {
    TriggerPromiseReactions();
  }
}

class WrappedWorkerRunnable final : public WorkerSameThreadRunnable
{
public:
  WrappedWorkerRunnable(workers::WorkerPrivate* aWorkerPrivate, nsIRunnable* aRunnable)
    : WorkerSameThreadRunnable(aWorkerPrivate)
    , mRunnable(aRunnable)
  {
    MOZ_ASSERT(aRunnable);
    MOZ_COUNT_CTOR(WrappedWorkerRunnable);
  }

  bool
  WorkerRun(JSContext* aCx, workers::WorkerPrivate* aWorkerPrivate) override
  {
    NS_ASSERT_OWNINGTHREAD(WrappedWorkerRunnable);
    mRunnable->Run();
    return true;
  }

private:
  virtual
  ~WrappedWorkerRunnable()
  {
    MOZ_COUNT_DTOR(WrappedWorkerRunnable);
    NS_ASSERT_OWNINGTHREAD(WrappedWorkerRunnable);
  }

  nsCOMPtr<nsIRunnable> mRunnable;
  NS_DECL_OWNINGTHREAD
};

/* static */ void
Promise::DispatchToMicroTask(nsIRunnable* aRunnable)
{
  MOZ_ASSERT(aRunnable);

  CycleCollectedJSRuntime* runtime = CycleCollectedJSRuntime::Get();
  std::queue<nsCOMPtr<nsIRunnable>>& microtaskQueue =
    runtime->GetPromiseMicroTaskQueue();

  microtaskQueue.push(aRunnable);
}

#if defined(DOM_PROMISE_DEPRECATED_REPORTING)
void
Promise::MaybeReportRejected()
{
  if (mState != Rejected || mHadRejectCallback || mResult.isUndefined()) {
    return;
  }

  AutoJSAPI jsapi;
  // We may not have a usable global by now (if it got unlinked
  // already), so don't init with it.
  jsapi.Init();
  JSContext* cx = jsapi.cx();
  JS::Rooted<JSObject*> obj(cx, GetWrapper());
  MOZ_ASSERT(obj); // We preserve our wrapper, so should always have one here.
  JS::Rooted<JS::Value> val(cx, mResult);
  JS::ExposeValueToActiveJS(val);

  JSAutoCompartment ac(cx, obj);
  if (!JS_WrapValue(cx, &val)) {
    JS_ClearPendingException(cx);
    return;
  }

  js::ErrorReport report(cx);
  if (!report.init(cx, val)) {
    JS_ClearPendingException(cx);
    return;
  }

  RefPtr<xpc::ErrorReport> xpcReport = new xpc::ErrorReport();
  bool isMainThread = MOZ_LIKELY(NS_IsMainThread());
  bool isChrome = isMainThread ? nsContentUtils::IsSystemPrincipal(nsContentUtils::ObjectPrincipal(obj))
                               : GetCurrentThreadWorkerPrivate()->IsChromeWorker();
  nsPIDOMWindow* win = isMainThread ? xpc::WindowGlobalOrNull(obj) : nullptr;
  xpcReport->Init(report.report(), report.message(), isChrome, win ? win->WindowID() : 0);

  // Now post an event to do the real reporting async
  // Since Promises preserve their wrapper, it is essential to RefPtr<> the
  // AsyncErrorReporter, otherwise if the call to DispatchToMainThread fails, it
  // will leak. See Bug 958684.  So... don't use DispatchToMainThread()
  nsCOMPtr<nsIThread> mainThread = do_GetMainThread();
  if (NS_WARN_IF(!mainThread)) {
    // Would prefer NS_ASSERTION, but that causes failure in xpcshell tests
    NS_WARNING("!!! Trying to report rejected Promise after MainThread shutdown");
  }
  if (mainThread) {
    RefPtr<AsyncErrorReporter> r = new AsyncErrorReporter(xpcReport);
    mainThread->Dispatch(r.forget(), NS_DISPATCH_NORMAL);
  }
}
#endif // defined(DOM_PROMISE_DEPRECATED_REPORTING)

void
Promise::MaybeResolveInternal(JSContext* aCx,
                              JS::Handle<JS::Value> aValue)
{
  if (mResolvePending) {
    return;
  }

  ResolveInternal(aCx, aValue);
}

void
Promise::MaybeRejectInternal(JSContext* aCx,
                             JS::Handle<JS::Value> aValue)
{
  if (mResolvePending) {
    return;
  }

  RejectInternal(aCx, aValue);
}

void
Promise::HandleException(JSContext* aCx)
{
  JS::Rooted<JS::Value> exn(aCx);
  if (JS_GetPendingException(aCx, &exn)) {
    JS_ClearPendingException(aCx);
    RejectInternal(aCx, exn);
  }
}

void
Promise::ResolveInternal(JSContext* aCx,
                         JS::Handle<JS::Value> aValue)
{
  mResolvePending = true;

  if (aValue.isObject()) {
    AutoDontReportUncaught silenceReporting(aCx);
    JS::Rooted<JSObject*> valueObj(aCx, &aValue.toObject());

    // Thenables.
    JS::Rooted<JS::Value> then(aCx);
    if (!JS_GetProperty(aCx, valueObj, "then", &then)) {
      HandleException(aCx);
      return;
    }

    if (then.isObject() && JS::IsCallable(&then.toObject())) {
      // This is the then() function of the thenable aValueObj.
      JS::Rooted<JSObject*> thenObj(aCx, &then.toObject());

      // Add a fast path for the case when we're resolved with an actual
      // Promise.  This has two requirements:
      //
      // 1) valueObj is a Promise.
      // 2) thenObj is a JSFunction backed by our actual Promise::Then
      //    implementation.
      //
      // If those requirements are satisfied, then we know exactly what
      // thenObj.call(valueObj) will do, so we can optimize a bit and avoid ever
      // entering JS for this stuff.
      Promise* nextPromise;
      if (PromiseBinding::IsThenMethod(thenObj) &&
          NS_SUCCEEDED(UNWRAP_OBJECT(Promise, valueObj, nextPromise))) {
        // If we were taking the codepath that involves PromiseResolveThenableJob and
        // PromiseInit below, then eventually, in PromiseResolveThenableJob::Run, we
        // would create some JSFunctions in the compartment of
        // this->GetWrapper() and pass them to the PromiseInit. So by the time
        // we'd see the resolution value it would be wrapped into the
        // compartment of this->GetWrapper().  The global of that compartment is
        // this->GetGlobalJSObject(), so use that as the global for
        // ResolvePromiseCallback/RejectPromiseCallback.
        JS::Rooted<JSObject*> glob(aCx, GlobalJSObject());
        RefPtr<PromiseCallback> resolveCb = new ResolvePromiseCallback(this, glob);
        RefPtr<PromiseCallback> rejectCb = new RejectPromiseCallback(this, glob);
        RefPtr<FastPromiseResolveThenableJob> task =
          new FastPromiseResolveThenableJob(resolveCb, rejectCb, nextPromise);
        DispatchToMicroTask(task);
        return;
      }

      RefPtr<PromiseInit> thenCallback =
        new PromiseInit(nullptr, thenObj, mozilla::dom::GetIncumbentGlobal());
      RefPtr<PromiseResolveThenableJob> task =
        new PromiseResolveThenableJob(this, valueObj, thenCallback);
      DispatchToMicroTask(task);
      return;
    }
  }

  MaybeSettle(aValue, Resolved);
}

void
Promise::RejectInternal(JSContext* aCx,
                        JS::Handle<JS::Value> aValue)
{
  mResolvePending = true;

  MaybeSettle(aValue, Rejected);
}

void
Promise::Settle(JS::Handle<JS::Value> aValue, PromiseState aState)
{
#ifdef MOZ_CRASHREPORTER
  if (!mGlobal && mFullfillmentStack) {
    AutoJSAPI jsapi;
    jsapi.Init();
    JSContext* cx = jsapi.cx();
    JS::RootedObject stack(cx, mFullfillmentStack);
    JSAutoCompartment ac(cx, stack);
    JS::RootedString stackJSString(cx);
    if (JS::BuildStackString(cx, stack, &stackJSString)) {
      nsAutoJSString stackString;
      if (stackString.init(cx, stackJSString)) {
        // Put the string in the crash report here, since we're about to crash
        CrashReporter::AnnotateCrashReport(NS_LITERAL_CSTRING("cced_promise_stack"),
                                           NS_ConvertUTF16toUTF8(stackString));
      } else {
        JS_ClearPendingException(cx);
      }
    } else {
      JS_ClearPendingException(cx);
    }
  }
#endif

  if (mGlobal->IsDying()) {
    return;
  }

  mSettlementTimestamp = TimeStamp::Now();

  SetResult(aValue);
  SetState(aState);

  AutoJSAPI jsapi;
  jsapi.Init();
  JSContext* cx = jsapi.cx();
  JS::RootedObject wrapper(cx, GetWrapper());
  MOZ_ASSERT(wrapper); // We preserved it
  JSAutoCompartment ac(cx, wrapper);
  JS::dbg::onPromiseSettled(cx, wrapper);

  if (aState == PromiseState::Rejected &&
      mIsLastInChain) {
    // The Promise has just been rejected, and it is last in chain.
    // We need to inform PromiseDebugging.
    // If the Promise is eventually not the last in chain anymore,
    // we will need to inform PromiseDebugging again.
    PromiseDebugging::AddUncaughtRejection(*this);
  }

#if defined(DOM_PROMISE_DEPRECATED_REPORTING)
  // If the Promise was rejected, and there is no reject handler already setup,
  // watch for thread shutdown.
  if (aState == PromiseState::Rejected &&
      !mHadRejectCallback &&
      !NS_IsMainThread()) {
    workers::WorkerPrivate* worker = GetCurrentThreadWorkerPrivate();
    MOZ_ASSERT(worker);
    worker->AssertIsOnWorkerThread();

    mFeature = new PromiseReportRejectFeature(this);
    if (NS_WARN_IF(!worker->AddFeature(worker->GetJSContext(), mFeature))) {
      // To avoid a false RemoveFeature().
      mFeature = nullptr;
      // Worker is shutting down, report rejection immediately since it is
      // unlikely that reject callbacks will be added after this point.
      MaybeReportRejectedOnce();
    }
  }
#endif // defined(DOM_PROMISE_DEPRECATED_REPORTING)

  TriggerPromiseReactions();
}

void
Promise::MaybeSettle(JS::Handle<JS::Value> aValue,
                     PromiseState aState)
{
  // Promise.all() or Promise.race() implementations will repeatedly call
  // Resolve/RejectInternal rather than using the Maybe... forms. Stop SetState
  // from asserting.
  if (mState != Pending) {
    return;
  }

  Settle(aValue, aState);
}

void
Promise::TriggerPromiseReactions()
{
  nsTArray<RefPtr<PromiseCallback>> callbacks;
  callbacks.SwapElements(mState == Resolved ? mResolveCallbacks
                                            : mRejectCallbacks);
  mResolveCallbacks.Clear();
  mRejectCallbacks.Clear();

  for (uint32_t i = 0; i < callbacks.Length(); ++i) {
    RefPtr<PromiseReactionJob> task =
      new PromiseReactionJob(this, callbacks[i], mResult);
    DispatchToMicroTask(task);
  }
}

#if defined(DOM_PROMISE_DEPRECATED_REPORTING)
void
Promise::RemoveFeature()
{
  if (mFeature) {
    workers::WorkerPrivate* worker = GetCurrentThreadWorkerPrivate();
    MOZ_ASSERT(worker);
    worker->RemoveFeature(worker->GetJSContext(), mFeature);
    mFeature = nullptr;
  }
}

bool
PromiseReportRejectFeature::Notify(JSContext* aCx, workers::Status aStatus)
{
  MOZ_ASSERT(aStatus > workers::Running);
  mPromise->MaybeReportRejectedOnce();
  // After this point, `this` has been deleted by RemoveFeature!
  return true;
}
#endif // defined(DOM_PROMISE_DEPRECATED_REPORTING)

bool
Promise::CaptureStack(JSContext* aCx, JS::Heap<JSObject*>& aTarget)
{
  JS::Rooted<JSObject*> stack(aCx);
  if (!JS::CaptureCurrentStack(aCx, &stack)) {
    return false;
  }
  aTarget = stack;
  return true;
}

void
Promise::GetDependentPromises(nsTArray<RefPtr<Promise>>& aPromises)
{
  // We want to return promises that correspond to then() calls, Promise.all()
  // calls, and Promise.race() calls.
  //
  // For the then() case, we have both resolve and reject callbacks that know
  // what the next promise is.
  //
  // For the race() case, likewise.
  //
  // For the all() case, our reject callback knows what the next promise is, but
  // our resolve callback just knows it needs to notify some
  // PromiseNativeHandler, which itself only has an indirect relationship to the
  // next promise.
  //
  // So we walk over our _reject_ callbacks and ask each of them what promise
  // its dependent promise is.
  for (size_t i = 0; i < mRejectCallbacks.Length(); ++i) {
    Promise* p = mRejectCallbacks[i]->GetDependentPromise();
    if (p) {
      aPromises.AppendElement(p);
    }
  }
}

// A WorkerRunnable to resolve/reject the Promise on the worker thread.
// Calling thread MUST hold PromiseWorkerProxy's mutex before creating this.
class PromiseWorkerProxyRunnable : public workers::WorkerRunnable
{
public:
  PromiseWorkerProxyRunnable(PromiseWorkerProxy* aPromiseWorkerProxy,
                             PromiseWorkerProxy::RunCallbackFunc aFunc)
    : WorkerRunnable(aPromiseWorkerProxy->GetWorkerPrivate(),
                     WorkerThreadUnchangedBusyCount)
    , mPromiseWorkerProxy(aPromiseWorkerProxy)
    , mFunc(aFunc)
  {
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_ASSERT(mPromiseWorkerProxy);
  }

  virtual bool
  WorkerRun(JSContext* aCx, workers::WorkerPrivate* aWorkerPrivate)
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();
    MOZ_ASSERT(aWorkerPrivate == mWorkerPrivate);

    MOZ_ASSERT(mPromiseWorkerProxy);
    RefPtr<Promise> workerPromise = mPromiseWorkerProxy->WorkerPromise();

    // Here we convert the buffer to a JS::Value.
    JS::Rooted<JS::Value> value(aCx);
    if (!mPromiseWorkerProxy->Read(aCx, &value)) {
      JS_ClearPendingException(aCx);
      return false;
    }

    (workerPromise->*mFunc)(aCx, value);

    // Release the Promise because it has been resolved/rejected for sure.
    mPromiseWorkerProxy->CleanUp(aCx);
    return true;
  }

protected:
  ~PromiseWorkerProxyRunnable() {}

private:
  RefPtr<PromiseWorkerProxy> mPromiseWorkerProxy;

  // Function pointer for calling Promise::{ResolveInternal,RejectInternal}.
  PromiseWorkerProxy::RunCallbackFunc mFunc;
};

/* static */
already_AddRefed<PromiseWorkerProxy>
PromiseWorkerProxy::Create(workers::WorkerPrivate* aWorkerPrivate,
                           Promise* aWorkerPromise,
                           const PromiseWorkerProxyStructuredCloneCallbacks* aCb)
{
  MOZ_ASSERT(aWorkerPrivate);
  aWorkerPrivate->AssertIsOnWorkerThread();
  MOZ_ASSERT(aWorkerPromise);
  MOZ_ASSERT_IF(aCb, !!aCb->Write && !!aCb->Read);

  RefPtr<PromiseWorkerProxy> proxy =
    new PromiseWorkerProxy(aWorkerPrivate, aWorkerPromise, aCb);

  // We do this to make sure the worker thread won't shut down before the
  // promise is resolved/rejected on the worker thread.
  if (!proxy->AddRefObject()) {
    // Probably the worker is terminating. We cannot complete the operation
    // and we have to release all the resources.
    proxy->CleanProperties();
    return nullptr;
  }

  return proxy.forget();
}

NS_IMPL_ISUPPORTS0(PromiseWorkerProxy)

PromiseWorkerProxy::PromiseWorkerProxy(workers::WorkerPrivate* aWorkerPrivate,
                                       Promise* aWorkerPromise,
                                       const PromiseWorkerProxyStructuredCloneCallbacks* aCallbacks)
  : mWorkerPrivate(aWorkerPrivate)
  , mWorkerPromise(aWorkerPromise)
  , mCleanedUp(false)
  , mCallbacks(aCallbacks)
  , mCleanUpLock("cleanUpLock")
  , mFeatureAdded(false)
{
}

PromiseWorkerProxy::~PromiseWorkerProxy()
{
  MOZ_ASSERT(mCleanedUp);
  MOZ_ASSERT(!mFeatureAdded);
  MOZ_ASSERT(!mWorkerPromise);
  MOZ_ASSERT(!mWorkerPrivate);
}

void
PromiseWorkerProxy::CleanProperties()
{
#ifdef DEBUG
  workers::WorkerPrivate* worker = GetCurrentThreadWorkerPrivate();
  MOZ_ASSERT(worker);
  worker->AssertIsOnWorkerThread();
#endif
  // Ok to do this unprotected from Create().
  // CleanUp() holds the lock before calling this.
  mCleanedUp = true;
  mWorkerPromise = nullptr;
  mWorkerPrivate = nullptr;

  // Clear the StructuredCloneHolderBase class.
  Clear();
}

bool
PromiseWorkerProxy::AddRefObject()
{
  MOZ_ASSERT(mWorkerPrivate);
  mWorkerPrivate->AssertIsOnWorkerThread();
  MOZ_ASSERT(!mFeatureAdded);
  if (!mWorkerPrivate->AddFeature(mWorkerPrivate->GetJSContext(),
                                  this)) {
    return false;
  }

  mFeatureAdded = true;
  // Maintain a reference so that we have a valid object to clean up when
  // removing the feature.
  AddRef();
  return true;
}

workers::WorkerPrivate*
PromiseWorkerProxy::GetWorkerPrivate() const
{
#ifdef DEBUG
  if (NS_IsMainThread()) {
    mCleanUpLock.AssertCurrentThreadOwns();
  }
#endif
  // Safe to check this without a lock since we assert lock ownership on the
  // main thread above.
  MOZ_ASSERT(!mCleanedUp);
  MOZ_ASSERT(mFeatureAdded);

  return mWorkerPrivate;
}

Promise*
PromiseWorkerProxy::WorkerPromise() const
{
#ifdef DEBUG
  workers::WorkerPrivate* worker = GetCurrentThreadWorkerPrivate();
  MOZ_ASSERT(worker);
  worker->AssertIsOnWorkerThread();
#endif
  MOZ_ASSERT(mWorkerPromise);
  return mWorkerPromise;
}

void
PromiseWorkerProxy::StoreISupports(nsISupports* aSupports)
{
  MOZ_ASSERT(NS_IsMainThread());

  nsMainThreadPtrHandle<nsISupports> supports(
    new nsMainThreadPtrHolder<nsISupports>(aSupports));
  mSupportsArray.AppendElement(supports);
}

void
PromiseWorkerProxy::RunCallback(JSContext* aCx,
                                JS::Handle<JS::Value> aValue,
                                RunCallbackFunc aFunc)
{
  MOZ_ASSERT(NS_IsMainThread());

  MutexAutoLock lock(Lock());
  // If the worker thread's been cancelled we don't need to resolve the Promise.
  if (CleanedUp()) {
    return;
  }

  // The |aValue| is written into the StructuredCloneHolderBase.
  if (!Write(aCx, aValue)) {
    JS_ClearPendingException(aCx);
    MOZ_ASSERT(false, "cannot serialize the value with the StructuredCloneAlgorithm!");
  }

  RefPtr<PromiseWorkerProxyRunnable> runnable =
    new PromiseWorkerProxyRunnable(this, aFunc);

  runnable->Dispatch(aCx);
}

void
PromiseWorkerProxy::ResolvedCallback(JSContext* aCx,
                                     JS::Handle<JS::Value> aValue)
{
  RunCallback(aCx, aValue, &Promise::ResolveInternal);
}

void
PromiseWorkerProxy::RejectedCallback(JSContext* aCx,
                                     JS::Handle<JS::Value> aValue)
{
  RunCallback(aCx, aValue, &Promise::RejectInternal);
}

bool
PromiseWorkerProxy::Notify(JSContext* aCx, Status aStatus)
{
  if (aStatus >= Canceling) {
    CleanUp(aCx);
  }

  return true;
}

void
PromiseWorkerProxy::CleanUp(JSContext* aCx)
{
  // Can't release Mutex while it is still locked, so scope the lock.
  {
    MutexAutoLock lock(Lock());

    // |mWorkerPrivate| is not safe to use anymore if we have already
    // cleaned up and RemoveFeature(), so we need to check |mCleanedUp| first.
    if (CleanedUp()) {
      return;
    }

    MOZ_ASSERT(mWorkerPrivate);
    mWorkerPrivate->AssertIsOnWorkerThread();
    MOZ_ASSERT(mWorkerPrivate->GetJSContext() == aCx);

    // Release the Promise and remove the PromiseWorkerProxy from the features of
    // the worker thread since the Promise has been resolved/rejected or the
    // worker thread has been cancelled.
    MOZ_ASSERT(mFeatureAdded);
    mWorkerPrivate->RemoveFeature(mWorkerPrivate->GetJSContext(), this);
    mFeatureAdded = false;
    CleanProperties();
  }
  Release();
}

JSObject*
PromiseWorkerProxy::CustomReadHandler(JSContext* aCx,
                                      JSStructuredCloneReader* aReader,
                                      uint32_t aTag,
                                      uint32_t aIndex)
{
  if (NS_WARN_IF(!mCallbacks)) {
    return nullptr;
  }

  return mCallbacks->Read(aCx, aReader, this, aTag, aIndex);
}

bool
PromiseWorkerProxy::CustomWriteHandler(JSContext* aCx,
                                       JSStructuredCloneWriter* aWriter,
                                       JS::Handle<JSObject*> aObj)
{
  if (NS_WARN_IF(!mCallbacks)) {
    return false;
  }

  return mCallbacks->Write(aCx, aWriter, this, aObj);
}

// Specializations of MaybeRejectBrokenly we actually support.
template<>
void Promise::MaybeRejectBrokenly(const RefPtr<DOMError>& aArg) {
  MaybeSomething(aArg, &Promise::MaybeReject);
}
template<>
void Promise::MaybeRejectBrokenly(const RefPtr<DOMException>& aArg) {
  MaybeSomething(aArg, &Promise::MaybeReject);
}
template<>
void Promise::MaybeRejectBrokenly(const nsAString& aArg) {
  MaybeSomething(aArg, &Promise::MaybeReject);
}

uint64_t
Promise::GetID() {
  if (mID != 0) {
    return mID;
  }
  return mID = ++gIDGenerator;
}

} // namespace dom
} // namespace mozilla
