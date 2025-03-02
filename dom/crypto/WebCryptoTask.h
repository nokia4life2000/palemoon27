/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_WebCryptoTask_h
#define mozilla_dom_WebCryptoTask_h

#include "CryptoTask.h"

#include "nsIGlobalObject.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/DOMException.h"
#include "mozilla/dom/SubtleCryptoBinding.h"
#include "mozilla/dom/CryptoKey.h"

namespace mozilla {
namespace dom {

typedef ArrayBufferViewOrArrayBuffer CryptoOperationData;
typedef ArrayBufferViewOrArrayBuffer KeyData;

/*

The execution of a WebCryptoTask happens in several phases

1. Constructor
2. BeforeCrypto
3. CalculateResult -> DoCrypto
4. AfterCrypto
5. Resolve or FailWithError
6. Cleanup

If any of these steps produces an error (setting mEarlyRv), then
subsequent steps will not proceed.  If the constructor or BeforeCrypto
sets mEarlyComplete to true, then we will skip step 3, saving the
thread overhead.

In general, the constructor should handle any parsing steps that
require JS context, and otherwise just cache information for later
steps to use.

All steps besides step 3 occur on the main thread, so they should
avoid blocking operations.

Only step 3 is guarded to ensure that NSS has not been shutdown,
so all NSS interactions should occur in DoCrypto

Cleanup should execute regardless of what else happens.

*/

#define MAYBE_EARLY_FAIL(rv) \
if (NS_FAILED(rv)) { \
  FailWithError(rv); \
  Skip(); \
  return; \
}

class WebCryptoTask : public CryptoTask
{
public:
  virtual void DispatchWithPromise(Promise* aResultPromise)
  {
    MOZ_ASSERT(NS_IsMainThread());
    mResultPromise = aResultPromise;

    // Fail if an error was set during the constructor
    MAYBE_EARLY_FAIL(mEarlyRv)

    // Perform pre-NSS operations, and fail if they fail
    mEarlyRv = BeforeCrypto();
    MAYBE_EARLY_FAIL(mEarlyRv)

    // Skip NSS if we're already done, or launch a CryptoTask
    if (mEarlyComplete) {
      CallCallback(mEarlyRv);
      Skip();
      return;
    }

     mEarlyRv = Dispatch("SubtleCrypto");
     MAYBE_EARLY_FAIL(mEarlyRv)
  }

protected:
  static WebCryptoTask* CreateEncryptDecryptTask(JSContext* aCx,
                           const ObjectOrString& aAlgorithm,
                           CryptoKey& aKey,
                           const CryptoOperationData& aData,
                           bool aEncrypt);

  static WebCryptoTask* CreateSignVerifyTask(JSContext* aCx,
                          const ObjectOrString& aAlgorithm,
                          CryptoKey& aKey,
                          const CryptoOperationData& aSignature,
                          const CryptoOperationData& aData,
                          bool aSign);

public:
  static WebCryptoTask* CreateEncryptTask(JSContext* aCx,
                          const ObjectOrString& aAlgorithm,
                          CryptoKey& aKey,
                          const CryptoOperationData& aData)
  {
    return CreateEncryptDecryptTask(aCx, aAlgorithm, aKey, aData, true);
  }

  static WebCryptoTask* CreateDecryptTask(JSContext* aCx,
                          const ObjectOrString& aAlgorithm,
                          CryptoKey& aKey,
                          const CryptoOperationData& aData)
  {
    return CreateEncryptDecryptTask(aCx, aAlgorithm, aKey, aData, false);
  }

  static WebCryptoTask* CreateSignTask(JSContext* aCx,
                          const ObjectOrString& aAlgorithm,
                          CryptoKey& aKey,
                          const CryptoOperationData& aData)
  {
    CryptoOperationData dummy;
    dummy.SetAsArrayBuffer(aCx);
    return CreateSignVerifyTask(aCx, aAlgorithm, aKey, dummy, aData, true);
  }

  static WebCryptoTask* CreateVerifyTask(JSContext* aCx,
                          const ObjectOrString& aAlgorithm,
                          CryptoKey& aKey,
                          const CryptoOperationData& aSignature,
                          const CryptoOperationData& aData)
  {
    return CreateSignVerifyTask(aCx, aAlgorithm, aKey, aSignature, aData, false);
  }

  static WebCryptoTask* CreateDigestTask(JSContext* aCx,
                          const ObjectOrString& aAlgorithm,
                          const CryptoOperationData& aData);

  static WebCryptoTask* CreateImportKeyTask(JSContext* aCx,
                          const nsAString& aFormat,
                          JS::Handle<JSObject*> aKeyData,
                          const ObjectOrString& aAlgorithm,
                          bool aExtractable,
                          const Sequence<nsString>& aKeyUsages);
  static WebCryptoTask* CreateExportKeyTask(const nsAString& aFormat,
                          CryptoKey& aKey);
  static WebCryptoTask* CreateGenerateKeyTask(JSContext* aCx,
                          const ObjectOrString& aAlgorithm,
                          bool aExtractable,
                          const Sequence<nsString>& aKeyUsages);

  static WebCryptoTask* CreateDeriveKeyTask(JSContext* aCx,
                          const ObjectOrString& aAlgorithm,
                          CryptoKey& aBaseKey,
                          const ObjectOrString& aDerivedKeyType,
                          bool extractable,
                          const Sequence<nsString>& aKeyUsages);
  static WebCryptoTask* CreateDeriveBitsTask(JSContext* aCx,
                          const ObjectOrString& aAlgorithm,
                          CryptoKey& aKey,
                          uint32_t aLength);

  static WebCryptoTask* CreateWrapKeyTask(JSContext* aCx,
                          const nsAString& aFormat,
                          CryptoKey& aKey,
                          CryptoKey& aWrappingKey,
                          const ObjectOrString& aWrapAlgorithm);
  static WebCryptoTask* CreateUnwrapKeyTask(JSContext* aCx,
                          const nsAString& aFormat,
                          const ArrayBufferViewOrArrayBuffer& aWrappedKey,
                          CryptoKey& aUnwrappingKey,
                          const ObjectOrString& aUnwrapAlgorithm,
                          const ObjectOrString& aUnwrappedKeyAlgorithm,
                          bool aExtractable,
                          const Sequence<nsString>& aKeyUsages);

protected:
  RefPtr<Promise> mResultPromise;
  nsresult mEarlyRv;
  bool mEarlyComplete;

  WebCryptoTask()
    : mEarlyRv(NS_OK)
    , mEarlyComplete(false)
  {}

  // For things that need to happen on the main thread
  // either before or after CalculateResult
  virtual nsresult BeforeCrypto() { return NS_OK; }
  virtual nsresult DoCrypto() { return NS_OK; }
  virtual nsresult AfterCrypto() { return NS_OK; }
  virtual void Resolve() {}
  virtual void Cleanup() {}

  void FailWithError(nsresult aRv);

  // Subclasses should override this method if they keep references to
  // any NSS objects, e.g., SECKEYPrivateKey or PK11SymKey.
  virtual void ReleaseNSSResources() override {}

  virtual nsresult CalculateResult() override final;

  virtual void CallCallback(nsresult rv) override final;
};

// XXX This class is declared here (unlike others) to enable reuse by WebRTC.
class GenerateAsymmetricKeyTask : public WebCryptoTask
{
public:
  GenerateAsymmetricKeyTask(JSContext* aCx,
                            const ObjectOrString& aAlgorithm, bool aExtractable,
                            const Sequence<nsString>& aKeyUsages);
protected:
  ScopedPLArenaPool mArena;
  CryptoKeyPair mKeyPair;
  nsString mAlgName;
  CK_MECHANISM_TYPE mMechanism;
  PK11RSAGenParams mRsaParams;
  SECKEYDHParams mDhParams;
  nsString mNamedCurve;

  virtual void ReleaseNSSResources() override;
  virtual nsresult DoCrypto() override;
  virtual void Resolve() override;

private:
  ScopedSECKEYPublicKey mPublicKey;
  ScopedSECKEYPrivateKey mPrivateKey;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_WebCryptoTask_h
