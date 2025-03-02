/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsContentSecurityManager.h"
#include "nsSecCheckWrapChannel.h"
#include "nsIForcePendingChannel.h"
#include "nsCOMPtr.h"

static mozilla::LazyLogModule gChannelWrapperLog("ChannelWrapper");
#define CHANNELWRAPPERLOG(args) MOZ_LOG(gChannelWrapperLog, mozilla::LogLevel::Debug, args)

NS_IMPL_ADDREF(nsSecCheckWrapChannelBase)
NS_IMPL_RELEASE(nsSecCheckWrapChannelBase)

NS_INTERFACE_MAP_BEGIN(nsSecCheckWrapChannelBase)
  NS_INTERFACE_MAP_ENTRY_CONDITIONAL(nsIHttpChannel, mHttpChannel)
  NS_INTERFACE_MAP_ENTRY_CONDITIONAL(nsIHttpChannelInternal, mHttpChannelInternal)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIHttpChannel)
  NS_INTERFACE_MAP_ENTRY(nsIRequest)
  NS_INTERFACE_MAP_ENTRY(nsIChannel)
  NS_INTERFACE_MAP_ENTRY_CONDITIONAL(nsIUploadChannel, mUploadChannel)
  NS_INTERFACE_MAP_ENTRY_CONDITIONAL(nsIUploadChannel2, mUploadChannel2)
  NS_INTERFACE_MAP_ENTRY(nsISecCheckWrapChannel)
NS_INTERFACE_MAP_END

//---------------------------------------------------------
// nsSecCheckWrapChannelBase implementation
//---------------------------------------------------------

nsSecCheckWrapChannelBase::nsSecCheckWrapChannelBase(nsIChannel* aChannel)
 : mChannel(aChannel)
 , mHttpChannel(do_QueryInterface(aChannel))
 , mHttpChannelInternal(do_QueryInterface(aChannel))
 , mRequest(do_QueryInterface(aChannel))
 , mUploadChannel(do_QueryInterface(aChannel))
 , mUploadChannel2(do_QueryInterface(aChannel))
{
  MOZ_ASSERT(mChannel, "can not create a channel wrapper without a channel");
}

nsSecCheckWrapChannelBase::~nsSecCheckWrapChannelBase()
{
}

//---------------------------------------------------------
// nsISecCheckWrapChannel implementation
//---------------------------------------------------------

NS_IMETHODIMP
nsSecCheckWrapChannelBase::GetInnerChannel(nsIChannel **aInnerChannel)
{
  NS_IF_ADDREF(*aInnerChannel = mChannel);
  return NS_OK;
}

//---------------------------------------------------------
// nsSecCheckWrapChannel implementation
//---------------------------------------------------------

nsSecCheckWrapChannel::nsSecCheckWrapChannel(nsIChannel* aChannel,
                                             nsILoadInfo* aLoadInfo)
 : nsSecCheckWrapChannelBase(aChannel)
 , mLoadInfo(aLoadInfo)
{
  {
    nsCOMPtr<nsIURI> uri;
    mChannel->GetURI(getter_AddRefs(uri));
    nsAutoCString spec;
    if (uri) {
      uri->GetSpec(spec);
    }
    CHANNELWRAPPERLOG(("nsSecCheckWrapChannel::nsSecCheckWrapChannel [%p] (%s)",this, spec.get()));
  }
}

// static
already_AddRefed<nsIChannel>
nsSecCheckWrapChannel::MaybeWrap(nsIChannel* aChannel, nsILoadInfo* aLoadInfo)
{
  // Maybe a custom protocol handler actually returns a gecko
  // http/ftpChannel - To check this we will check whether the channel
  // implements a gecko non-scriptable interface e.g. nsIForcePendingChannel.
  nsCOMPtr<nsIForcePendingChannel> isGeckoChannel = do_QueryInterface(aChannel);

  nsCOMPtr<nsIChannel> channel;
  if (isGeckoChannel) {
    // If it is a gecko channel (ftp or http) we do not need to wrap it.
    channel = aChannel;
    channel->SetLoadInfo(aLoadInfo);
  } else {
    channel = new nsSecCheckWrapChannel(aChannel, aLoadInfo);
  }
  return channel.forget();
}

nsSecCheckWrapChannel::~nsSecCheckWrapChannel()
{
}

//---------------------------------------------------------
// nsIChannel implementation
//---------------------------------------------------------

NS_IMETHODIMP
nsSecCheckWrapChannel::GetLoadInfo(nsILoadInfo** aLoadInfo)
{
  CHANNELWRAPPERLOG(("nsSecCheckWrapChannel::GetLoadInfo() [%p]",this));
  NS_IF_ADDREF(*aLoadInfo = mLoadInfo);
  return NS_OK;
}

NS_IMETHODIMP
nsSecCheckWrapChannel::SetLoadInfo(nsILoadInfo* aLoadInfo)
{
  CHANNELWRAPPERLOG(("nsSecCheckWrapChannel::SetLoadInfo() [%p]", this));
  mLoadInfo = aLoadInfo;
  return NS_OK;
}

NS_IMETHODIMP
nsSecCheckWrapChannel::AsyncOpen2(nsIStreamListener *aListener)
{
  nsCOMPtr<nsIStreamListener> listener = aListener;
  nsresult rv = nsContentSecurityManager::doContentSecurityCheck(this, listener);
  NS_ENSURE_SUCCESS(rv, rv);
  return AsyncOpen(listener, nullptr);
}

NS_IMETHODIMP
nsSecCheckWrapChannel::Open2(nsIInputStream** aStream)
{
  nsCOMPtr<nsIStreamListener> listener;
  nsresult rv = nsContentSecurityManager::doContentSecurityCheck(this, listener);
  NS_ENSURE_SUCCESS(rv, rv);
  return Open(aStream);
}
