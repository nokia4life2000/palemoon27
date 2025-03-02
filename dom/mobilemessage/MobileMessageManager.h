/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_mobilemessage_MobileMessageManager_h
#define mozilla_dom_mobilemessage_MobileMessageManager_h

#include "mozilla/Attributes.h"
#include "mozilla/DOMEventTargetHelper.h"
#include "nsIObserver.h"

class nsISmsService;
class nsIDOMMozSmsMessage;
class nsIDOMMozMmsMessage;

namespace mozilla {
namespace dom {

class Promise;
class DOMRequest;
class DOMCursor;
struct MmsParameters;
struct MmsSendParameters;
struct MobileMessageFilter;
class OwningLongOrMozSmsMessageOrMozMmsMessage;
struct SmsSendParameters;
struct SmscAddress;

class MobileMessageManager final : public DOMEventTargetHelper
                                 , public nsIObserver
{
public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIOBSERVER

  NS_REALLY_FORWARD_NSIDOMEVENTTARGET(DOMEventTargetHelper)

  explicit MobileMessageManager(nsPIDOMWindow* aWindow);

  void Init();
  void Shutdown();

  nsPIDOMWindow*
  GetParentObject() const { return GetOwner(); }

  // WrapperCache
  virtual JSObject*
  WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override;

  // WebIDL Interface
  already_AddRefed<DOMRequest>
  GetSegmentInfoForText(const nsAString& aText,
                        ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  Send(const nsAString& aNumber,
       const nsAString& aText,
       const SmsSendParameters& aSendParams,
       ErrorResult& aRv);

  void
  Send(const Sequence<nsString>& aNumbers,
       const nsAString& aText,
       const SmsSendParameters& aSendParams,
       nsTArray<RefPtr<DOMRequest>>& aReturn,
       ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  SendMMS(const MmsParameters& aParameters,
          const MmsSendParameters& aSendParams,
          ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  GetMessage(int32_t aId,
             ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  Delete(int32_t aId,
         ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  Delete(nsIDOMMozSmsMessage* aMessage,
         ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  Delete(nsIDOMMozMmsMessage* aMessage,
         ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  Delete(const Sequence<OwningLongOrMozSmsMessageOrMozMmsMessage>& aParams,
         ErrorResult& aRv);

  already_AddRefed<DOMCursor>
  GetMessages(const MobileMessageFilter& aFilter,
              bool aReverse,
              ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  MarkMessageRead(int32_t aId,
                  bool aRead,
                  bool aSendReadReport,
                  ErrorResult& aRv);

  already_AddRefed<DOMCursor>
  GetThreads(ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  RetrieveMMS(int32_t aId,
              ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  RetrieveMMS(nsIDOMMozMmsMessage* aMessage,
              ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  GetSmscAddress(const Optional<uint32_t>& aServiceId,
                 ErrorResult& aRv);

  already_AddRefed<Promise>
  SetSmscAddress(const SmscAddress& aSmscAddress,
                 const Optional<uint32_t>& aServiceId,
                 ErrorResult& aRv);

  IMPL_EVENT_HANDLER(received)
  IMPL_EVENT_HANDLER(retrieving)
  IMPL_EVENT_HANDLER(sending)
  IMPL_EVENT_HANDLER(sent)
  IMPL_EVENT_HANDLER(failed)
  IMPL_EVENT_HANDLER(deliverysuccess)
  IMPL_EVENT_HANDLER(deliveryerror)
  IMPL_EVENT_HANDLER(readsuccess)
  IMPL_EVENT_HANDLER(readerror)
  IMPL_EVENT_HANDLER(deleted)

private:
  ~MobileMessageManager() {}

  /**
   * Internal Send() method used to send one message.
   */
  already_AddRefed<DOMRequest>
  Send(nsISmsService* aSmsService,
       uint32_t aServiceId,
       const nsAString& aNumber,
       const nsAString& aText,
       ErrorResult& aRv);

  already_AddRefed<DOMRequest>
  Delete(int32_t* aIdArray,
         uint32_t aSize,
         ErrorResult& aRv);

  nsresult
  DispatchTrustedSmsEventToSelf(const char* aTopic,
                                const nsAString& aEventName,
                                nsISupports* aMsg);

  nsresult
  DispatchTrustedDeletedEventToSelf(nsISupports* aDeletedInfo);

  /**
   * Helper to get message ID from SMS/MMS Message object
   */
  nsresult
  GetMessageId(JSContext* aCx,
               const JS::Value& aMessage,
               int32_t* aId);
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_mobilemessage_MobileMessageManager_h
