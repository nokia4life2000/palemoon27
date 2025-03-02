/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_bluetooth_bluedroid_BluetoothHfpManager_h
#define mozilla_dom_bluetooth_bluedroid_BluetoothHfpManager_h

#include "BluetoothHfpManagerBase.h"

/**
 * Fallback BluetoothHfpManager is built for non-phone devices (e.g., tablets).
 * These devices has no radio interface and the build flag MOZ_B2G_RIL is
 * disabled. To prevent build breaks of accessing radio interface, we implement
 * fallback BluetoothHfpManager with empty functions to keep original
 * BluetoothHfpManager away from numerous #ifdef/#endif statements.
 */

BEGIN_BLUETOOTH_NAMESPACE

class BluetoothHfpManager : public BluetoothHfpManagerBase
{
public:
  BT_DECL_HFP_MGR_BASE
  virtual void GetName(nsACString& aName)
  {
    aName.AssignLiteral("Fallback HFP/HSP");
  }

  static BluetoothHfpManager* Get();
  static void InitHfpInterface(BluetoothProfileResultHandler* aRes);
  static void DeinitHfpInterface(BluetoothProfileResultHandler* aRes);

  bool ConnectSco();
  bool DisconnectSco();

protected:
  virtual ~BluetoothHfpManager() { }

private:
  BluetoothHfpManager() { }
  bool Init();
  void HandleShutdown();
};

END_BLUETOOTH_NAMESPACE

#endif // mozilla_dom_bluetooth_bluedroid_BluetoothHfpManager_h
