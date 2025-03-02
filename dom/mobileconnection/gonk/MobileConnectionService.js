/* -*- Mode: js; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {classes: Cc, interfaces: Ci, utils: Cu, results: Cr} = Components;

Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/systemlibs.js");
Cu.import("resource://gre/modules/XPCOMUtils.jsm");

var RIL = {};
Cu.import("resource://gre/modules/ril_consts.js", RIL);

const GONK_MOBILECONNECTIONSERVICE_CONTRACTID =
  "@mozilla.org/mobileconnection/gonkmobileconnectionservice;1";

const GONK_MOBILECONNECTIONSERVICE_CID =
  Components.ID("{0c9c1a96-2c72-4c55-9e27-0ca73eb16f63}");
const MOBILECONNECTIONINFO_CID =
  Components.ID("{8162b3c0-664b-45f6-96cd-f07b4e193b0e}");
const MOBILENETWORKINFO_CID =
  Components.ID("{a6c8416c-09b4-46d1-bf29-6520d677d085}");
const MOBILECELLINFO_CID =
  Components.ID("{0635d9ab-997e-4cdf-84e7-c1883752dff3}");
const MOBILECALLFORWARDINGOPTIONS_CID =
  Components.ID("{e0cf4463-ee63-4b05-ab2e-d94bf764836c}");
const NEIGHBORINGCELLINFO_CID =
  Components.ID("{6078cbf1-f34c-44fa-96f8-11a88d4bfdd3}");
const GSMCELLINFO_CID =
  Components.ID("{e3cf3aa0-f992-48fe-967b-ec98a28c8535}");
const WCDMACELLINFO_CID =
  Components.ID("{62e2c83c-b535-4068-9762-8039fac48106}");
const CDMACELLINFO_CID =
  Components.ID("{40f491f0-dd8b-42fd-af32-aef5b002749a}");
const LTECELLINFO_CID =
  Components.ID("{715e2c76-3b08-41e4-8ea5-e60c5ce6393e}");


const NS_XPCOM_SHUTDOWN_OBSERVER_ID      = "xpcom-shutdown";
const NS_PREFBRANCH_PREFCHANGE_TOPIC_ID  = "nsPref:changed";
const NS_NETWORK_ACTIVE_CHANGED_TOPIC_ID = "network-active-changed";
const NS_DATA_CALL_ERROR_TOPIC_ID        = "data-call-error";

const kPrefRilDebuggingEnabled = "ril.debugging.enabled";

const INT32_MAX = 2147483647;
const UNKNOWN_RSSI = 99;

XPCOMUtils.defineLazyServiceGetter(this, "gMobileConnectionMessenger",
                                   "@mozilla.org/ril/system-messenger-helper;1",
                                   "nsIMobileConnectionMessenger");

XPCOMUtils.defineLazyServiceGetter(this, "gNetworkManager",
                                   "@mozilla.org/network/manager;1",
                                   "nsINetworkManager");

XPCOMUtils.defineLazyServiceGetter(this, "gIccService",
                                   "@mozilla.org/icc/iccservice;1",
                                   "nsIIccService");

XPCOMUtils.defineLazyServiceGetter(this, "gDataCallManager",
                                   "@mozilla.org/datacall/manager;1",
                                   "nsIDataCallManager");

XPCOMUtils.defineLazyModuleGetter(this, "gTelephonyUtils",
                                  "resource://gre/modules/TelephonyUtils.jsm",
                                  "TelephonyUtils");

XPCOMUtils.defineLazyGetter(this, "gRadioInterfaceLayer", function() {
  let ril = { numRadioInterfaces: 0 };
  try {
    ril = Cc["@mozilla.org/ril;1"].getService(Ci.nsIRadioInterfaceLayer);
  } catch(e) {}
  return ril;
});

let DEBUG = RIL.DEBUG_RIL;
function debug(s) {
  dump("MobileConnectionService: " + s + "\n");
}

function MobileNetworkInfo() {
  this.shortName = null;
  this.longName = null;
  this.mcc = null;
  this.mnc = null;
  this.stat = null;
}
MobileNetworkInfo.prototype = {
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIMobileNetworkInfo]),
  classID:        MOBILENETWORKINFO_CID,
  classInfo:      XPCOMUtils.generateCI({
    classID:          MOBILENETWORKINFO_CID,
    classDescription: "MobileNetworkInfo",
    interfaces:       [Ci.nsIMobileNetworkInfo]
  })
};

function MobileCellInfo() {
  this.gsmLocationAreaCode = -1;
  this.gsmCellId = -1;
  this.cdmaBaseStationId = -1;
  this.cdmaBaseStationLatitude = -2147483648;
  this.cdmaBaseStationLongitude = -2147483648;
  this.cdmaSystemId = -1;
  this.cdmaNetworkId = -1;
}
MobileCellInfo.prototype = {
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIMobileCellInfo]),
  classID:        MOBILECELLINFO_CID,
  classInfo:      XPCOMUtils.generateCI({
    classID:          MOBILECELLINFO_CID,
    classDescription: "MobileCellInfo",
    interfaces:       [Ci.nsIMobileCellInfo]
  })
};

function MobileConnectionInfo() {}
MobileConnectionInfo.prototype = {
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIMobileConnectionInfo]),
  classID: MOBILECONNECTIONINFO_CID,
  classInfo: XPCOMUtils.generateCI({
    classID: MOBILECONNECTIONINFO_CID,
    classDescription: "MobileConnectionInfo",
    interfaces: [Ci.nsIMobileConnectionInfo]
  }),

  state: null,
  connected: false,
  emergencyCallsOnly: false,
  roaming: false,
  network: null,
  cell: null,
  type: null,
  signalStrength: null,
  relSignalStrength: null
};

function MobileCallForwardingOptions(aOptions) {
  for (let key in aOptions) {
    this[key] = aOptions[key];
  }
}
MobileCallForwardingOptions.prototype = {
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIMobileCallForwardingOptions]),
  classID: MOBILECALLFORWARDINGOPTIONS_CID,
  classInfo: XPCOMUtils.generateCI({
    classID:          MOBILECALLFORWARDINGOPTIONS_CID,
    classDescription: "MobileCallForwardingOptions",
    interfaces:       [Ci.nsIMobileCallForwardingOptions]
  }),

  // nsIMobileForwardingOptions

  active: false,
  action: Ci.nsIMobileConnection.CALL_FORWARD_ACTION_UNKNOWN,
  reason: Ci.nsIMobileConnection.CALL_FORWARD_REASON_UNKNOWN,
  number: null,
  timeSeconds: -1,
  serviceClass: Ci.nsIMobileConnection.ICC_SERVICE_CLASS_NONE
}

function NeighboringCellInfo() {}
NeighboringCellInfo.prototype = {
  QueryInterface: XPCOMUtils.generateQI([Ci.nsINeighboringCellInfo]),
  classID:        NEIGHBORINGCELLINFO_CID,
  classInfo:      XPCOMUtils.generateCI({
    classID:          NEIGHBORINGCELLINFO_CID,
    classDescription: "NeighboringCellInfo",
    interfaces:       [Ci.nsINeighboringCellInfo]
  }),

  // nsINeighboringCellInfo

  networkType: null,
  gsmLocationAreaCode: -1,
  gsmCellId: -1,
  wcdmaPsc: -1,
  signalStrength: UNKNOWN_RSSI
};

function CellInfo() {}
CellInfo.prototype = {

  // nsICellInfo

  type: null,
  registered: false,
  timestampType: Ci.nsICellInfo.TIMESTAMP_TYPE_UNKNOWN,
  timestamp: 0
};

function GsmCellInfo() {}
GsmCellInfo.prototype = {
  __proto__: CellInfo.prototype,
  QueryInterface: XPCOMUtils.generateQI([Ci.nsICellInfo,
                                         Ci.nsIGsmCellInfo]),
  classID: GSMCELLINFO_CID,
  classInfo: XPCOMUtils.generateCI({
    classID:          GSMCELLINFO_CID,
    classDescription: "GsmCellInfo",
    interfaces:       [Ci.nsIGsmCellInfo]
  }),

  // nsIGsmCellInfo

  mcc: INT32_MAX,
  mnc: INT32_MAX,
  lac: INT32_MAX,
  cid: INT32_MAX,
  signalStrength: UNKNOWN_RSSI,
  bitErrorRate: UNKNOWN_RSSI
};

function WcdmaCellInfo() {}
WcdmaCellInfo.prototype = {
  __proto__: CellInfo.prototype,
  QueryInterface: XPCOMUtils.generateQI([Ci.nsICellInfo,
                                         Ci.nsIWcdmaCellInfo]),
  classID: WCDMACELLINFO_CID,
  classInfo: XPCOMUtils.generateCI({
    classID:          WCDMACELLINFO_CID,
    classDescription: "WcdmaCellInfo",
    interfaces:       [Ci.nsIWcdmaCellInfo]
  }),

  // nsIWcdmaCellInfo

  mcc: INT32_MAX,
  mnc: INT32_MAX,
  lac: INT32_MAX,
  cid: INT32_MAX,
  psc: INT32_MAX,
  signalStrength: UNKNOWN_RSSI,
  bitErrorRate: UNKNOWN_RSSI
};

function LteCellInfo() {}
LteCellInfo.prototype = {
  __proto__: CellInfo.prototype,
  QueryInterface: XPCOMUtils.generateQI([Ci.nsICellInfo,
                                         Ci.nsILteCellInfo]),
  classID: LTECELLINFO_CID,
  classInfo: XPCOMUtils.generateCI({
    classID:          LTECELLINFO_CID,
    classDescription: "LteCellInfo",
    interfaces:       [Ci.nsILteCellInfo]
  }),

  // nsILteCellInfo

  mcc: INT32_MAX,
  mnc: INT32_MAX,
  cid: INT32_MAX,
  pcid: INT32_MAX,
  tac: INT32_MAX,
  signalStrength: UNKNOWN_RSSI,
  rsrp: INT32_MAX,
  rsrq: INT32_MAX,
  rssnr: INT32_MAX,
  cqi: INT32_MAX,
  timingAdvance: INT32_MAX
};

function CdmaCellInfo() {}
CdmaCellInfo.prototype = {
  __proto__: CellInfo.prototype,
  QueryInterface: XPCOMUtils.generateQI([Ci.nsICellInfo,
                                         Ci.nsICdmaCellInfo]),
  classID: CDMACELLINFO_CID,
  classInfo: XPCOMUtils.generateCI({
    classID:          CDMACELLINFO_CID,
    classDescription: "CdmaCellInfo",
    interfaces:       [Ci.nsICdmaCellInfo]
  }),

  // nsICdmaCellInfo

  networkId: INT32_MAX,
  systemId: INT32_MAX,
  baseStationId: INT32_MAX,
  longitude: INT32_MAX,
  latitude: INT32_MAX,
  cdmaDbm: INT32_MAX,
  cdmaEcio: INT32_MAX,
  evdoDbm: INT32_MAX,
  evdoEcio: INT32_MAX,
  evdoSnr: INT32_MAX
};

function MobileConnectionProvider(aClientId, aRadioInterface) {
  this._clientId = aClientId;
  this._radioInterface = aRadioInterface;
  this._operatorInfo = new MobileNetworkInfo();
  // An array of nsIMobileConnectionListener instances.
  this._listeners = [];

  this.supportedNetworkTypes = this._getSupportedNetworkTypes();
  this.voice = new MobileConnectionInfo();
  this.data = new MobileConnectionInfo();
}
MobileConnectionProvider.prototype = {
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIMobileConnection]),

  _clientId: null,
  _radioInterface: null,
  _operatorInfo: null,
  _listeners: null,

  /**
   * The networks that are currently trying to be selected (or "automatic").
   * This helps ensure that only one network per client is selected at a time.
   */
  _selectingNetwork: null,

  /**
   * The two radio states below stand for the user expectation and the hardware
   * status, respectively. |radioState| will be updated based on their values.
   */
  _expectedRadioState: RIL.GECKO_RADIOSTATE_UNKNOWN,
  _hardwareRadioState: RIL.GECKO_RADIOSTATE_UNKNOWN,

  voice: null,
  data: null,
  networkSelectionMode: Ci.nsIMobileConnection.NETWORK_SELECTION_MODE_UNKNOWN,
  radioState: Ci.nsIMobileConnection.MOBILE_RADIO_STATE_UNKNOWN,
  lastKnownNetwork: null,
  lastKnownHomeNetwork: null,
  supportedNetworkTypes: null,

  /**
   * A utility function to dump debug message.
   */
  _debug: function(aMessage) {
    dump("MobileConnectionProvider[" + this._clientId + "]: " + aMessage + "\n");
  },

  /**
   * A utility function to get supportedNetworkTypes from system property.
   */
  _getSupportedNetworkTypes: function() {
    let key = "ro.moz.ril." + this._clientId + ".network_types";
    let supportedNetworkTypes = libcutils.property_get(key, "").split(",");

    // If mozRIL system property is not available, fallback to AOSP system
    // property for support network types.
    if (supportedNetworkTypes.length === 1 && supportedNetworkTypes[0] === "") {
      key = "ro.telephony.default_network";
      let indexString = libcutils.property_get(key, "");
      let index = parseInt(indexString, 10);
      if (DEBUG) this._debug("Fallback to " + key + ": " + index);

      let networkTypes = RIL.RIL_PREFERRED_NETWORK_TYPE_TO_GECKO[index];
      supportedNetworkTypes = networkTypes ?
        networkTypes.replace(/-auto/g, "").split("/") :
        RIL.GECKO_SUPPORTED_NETWORK_TYPES_DEFAULT.split(",");
    }

    let enumNetworkTypes = [];
    for (let type of supportedNetworkTypes) {
      // If the value in system property is not valid, use the default one which
      // is defined in ril_consts.js.
      if (RIL.GECKO_SUPPORTED_NETWORK_TYPES.indexOf(type) < 0) {
        if (DEBUG) {
          this._debug("Unknown network type: " + type);
        }
        RIL.GECKO_SUPPORTED_NETWORK_TYPES_DEFAULT.split(",").forEach(aType => {
          enumNetworkTypes.push(RIL.GECKO_SUPPORTED_NETWORK_TYPES.indexOf(aType));
        });
        break;
      }
      enumNetworkTypes.push(RIL.GECKO_SUPPORTED_NETWORK_TYPES.indexOf(type));
    }
    if (DEBUG) {
      this._debug("Supported Network Types: " + enumNetworkTypes);
    }

    return enumNetworkTypes;
  },

  /**
   * Helper for guarding us against invalid mode for clir.
   */
  _isValidClirMode: function(aMode) {
    switch (aMode) {
      case Ci.nsIMobileConnection.CLIR_DEFAULT:
      case Ci.nsIMobileConnection.CLIR_INVOCATION:
      case Ci.nsIMobileConnection.CLIR_SUPPRESSION:
        return true;
      default:
        return false;
    }
  },

  /**
   * Fix the roaming. RIL can report roaming in some case it is not
   * really the case. See bug 787967
   */
  _checkRoamingBetweenOperators: function(aNetworkInfo) {
    let icc = gIccService.getIccByServiceId(this._clientId);
    let iccInfo = icc ? icc.iccInfo : null;
    let operator = aNetworkInfo.network;
    let state = aNetworkInfo.state;

    if (!iccInfo || !operator ||
        state !== RIL.GECKO_MOBILE_CONNECTION_STATE_REGISTERED) {
      return false;
    }

    let spn = iccInfo.spn && iccInfo.spn.toLowerCase();
    let longName = operator.longName && operator.longName.toLowerCase();
    let shortName = operator.shortName && operator.shortName.toLowerCase();

    let equalsLongName = longName && (spn == longName);
    let equalsShortName = shortName && (spn == shortName);
    let equalsMcc = iccInfo.mcc == operator.mcc;

    let newRoaming = aNetworkInfo.roaming &&
                     !(equalsMcc && (equalsLongName || equalsShortName));
    if (newRoaming === aNetworkInfo.roaming) {
      return false;
    }

    aNetworkInfo.roaming = newRoaming;
    return true;
  },

  /**
   * The design of this updating function is to update the attribute in
   * |aDestInfo| *only if* new data (e.g. aSrcInfo) contains the same attribute.
   * Thus, for the attribute in |aDestInfo| that isn't showed in |aSrcInfo|, it
   * should just keep the original value unchanged.
   */
  _updateConnectionInfo: function(aDestInfo, aSrcInfo) {
    let isUpdated = false;
    for (let key in aSrcInfo) {
      if (key === "network" || key === "cell") {
        // nsIMobileNetworkInfo and nsIMobileCellInfo are handled explicitly below.
        continue;
      }

      if (aDestInfo[key] !== aSrcInfo[key]) {
        isUpdated = true;
        aDestInfo[key] = aSrcInfo[key];
      }
    }

    // Make sure we also reset the operator and signal strength information
    // if we drop off the network.
    if (aDestInfo.state !== RIL.GECKO_MOBILE_CONNECTION_STATE_REGISTERED) {
      aDestInfo.cell = null;
      aDestInfo.network = null;
      aDestInfo.signalStrength = null;
      aDestInfo.relSignalStrength = null;
    } else {
      aDestInfo.network = this._operatorInfo;

      // If no new cell data is passed, we should just keep the original cell
      // data unchanged.
      if (aSrcInfo.cell) {
        if (!aDestInfo.cell) {
          aDestInfo.cell = new MobileCellInfo();
        }

        isUpdated = this._updateInfo(aDestInfo.cell, aSrcInfo.cell) || isUpdated;
      }
    }

    // Check roaming state
    isUpdated = this._checkRoamingBetweenOperators(aDestInfo) || isUpdated;
    return isUpdated;
  },

  /**
   * The design of this updating function is to update the attribute in
   * |aDestInfo| *only if* new data (e.g. aSrcInfo) contains the same attribute.
   * Thus, for the attribute in |aDestInfo| that isn't showed in |aSrcInfo|, it
   * should just keep the original value unchanged.
   */
  _updateInfo: function(aDestInfo, aSrcInfo) {
    let isUpdated = false;
    for (let key in aSrcInfo) {
      if (aDestInfo[key] !== aSrcInfo[key]) {
        isUpdated = true;
        aDestInfo[key] = aSrcInfo[key];
      }
    }
    return isUpdated;
  },

  _rulesToCallForwardingOptions: function(aRules) {
    return aRules.map(rule => new MobileCallForwardingOptions(rule));
  },

  _dispatchNotifyError: function(aCallback, aErrorMsg) {
    Services.tm.currentThread.dispatch(() => aCallback.notifyError(aErrorMsg),
                                       Ci.nsIThread.DISPATCH_NORMAL);
  },

  registerListener: function(aListener) {
    if (this._listeners.indexOf(aListener) >= 0) {
      throw Cr.NS_ERROR_UNEXPECTED;
    }

    this._listeners.push(aListener);
  },

  unregisterListener: function(aListener) {
    let index = this._listeners.indexOf(aListener);
    if (index >= 0) {
      this._listeners.splice(index, 1);
    }
  },

  deliverListenerEvent: function(aName, aArgs) {
    let listeners = this._listeners.slice();
    for (let listener of listeners) {
      if (this._listeners.indexOf(listener) === -1) {
        continue;
      }
      let handler = listener[aName];
      if (typeof handler != "function") {
        throw new Error("No handler for " + aName);
      }
      try {
        handler.apply(listener, aArgs);
      } catch (e) {
        if (DEBUG) {
          this._debug("listener for " + aName + " threw an exception: " + e);
        }
      }
    }
  },

  updateVoiceInfo: function(aNewInfo, aBatch = false) {
    let isUpdated = this._updateConnectionInfo(this.voice, aNewInfo);
    if (isUpdated && !aBatch) {
      this.deliverListenerEvent("notifyVoiceChanged");
    }
  },

  updateDataInfo: function(aNewInfo, aBatch = false) {
    // For the data connection, the `connected` flag indicates whether
    // there's an active data call. We get correct `connected` state here.
    let active = gNetworkManager.activeNetworkInfo;
    aNewInfo.connected = false;
    if (active &&
        active.type === Ci.nsINetworkInfo.NETWORK_TYPE_MOBILE &&
        active.serviceId === this._clientId) {
      aNewInfo.connected = true;
    }

    let isUpdated = this._updateConnectionInfo(this.data, aNewInfo);
    if (isUpdated && !aBatch) {
      this.deliverListenerEvent("notifyDataChanged");
    }

    if (isUpdated) {
      this._ensureDataRegistration();
    }
  },

  _dataRegistrationFailed: false,
  _ensureDataRegistration: function() {
    let isDataRegistered =
      this.data &&
      this.data.state == RIL.GECKO_MOBILE_CONNECTION_STATE_REGISTERED;
    let isVoiceRegistered =
      this.voice &&
      this.voice.state == RIL.GECKO_MOBILE_CONNECTION_STATE_REGISTERED;

    if (isVoiceRegistered && isDataRegistered) {
      if (DEBUG) {
        this._debug("Voice and data registered.");
      }
      this._dataRegistrationFailed = false;
      return;
    }

    if (isVoiceRegistered && !isDataRegistered &&
        this._clientId == gDataCallManager.dataDefaultServiceId) {

      // We have been here before, no more recovery.
      if (this._dataRegistrationFailed) {
        if (DEBUG) {
          this._debug("Voice and data not consistent: " + this.voice.state +
                      " != " + this.data.state + ".");
        }
        return;
      }

      if (DEBUG) {
        this._debug("Voice and data not consistent: " + this.voice.state +
                    " != " + this.data.state + ", try to recover.");
      }

      this._dataRegistrationFailed = true;
      // If there is any ongoing call, wait for them to disconnect.
      if (gTelephonyUtils.hasAnyCalls(this._clientId)) {
        gTelephonyUtils.waitForNoCalls(this._clientId)
          .then(() => {
            if (this._dataRegistrationFailed) {
              this._recoverDataRegistration();
            }
          });
        return;
      }

      this._recoverDataRegistration();
    }
  },


  /**
   * To recover data registration, get the current preferred network type first,
   * then set it to a temporary preferred network type, and last set back to the
   * previous preferred network type. This is will cause deregistration and
   * registration on both voice and data networks.
   */
  _recoverDataRegistration: function() {
    if (DEBUG) {
      this._debug("Trying to recover data registration...");
    }

    let currentPreferredNetworkType;

    let resetPreferredNetworkType = () => {
      this.setPreferredNetworkType(currentPreferredNetworkType, {
        QueryInterface: XPCOMUtils.generateQI([Ci.nsIMobileConnectionCallback]),
        notifySuccess: () => {},
        notifyError: aErrorMsg => {}
      });
    };

    let setTemporaryPreferredNetworkType = () => {
      this.setPreferredNetworkType(
        Ci.nsIMobileConnection.PREFERRED_NETWORK_TYPE_WCDMA_GSM_CDMA_EVDO, {
          QueryInterface: XPCOMUtils.generateQI([Ci.nsIMobileConnectionCallback]),
          notifySuccess: () =>  resetPreferredNetworkType(),
          notifyError: aErrorMsg => resetPreferredNetworkType()
      });
    };

    this.getPreferredNetworkType({
      QueryInterface: XPCOMUtils.generateQI([Ci.nsIMobileConnectionCallback]),
      notifyGetPreferredNetworkTypeSuccess: networkType => {
        currentPreferredNetworkType = networkType;
        setTemporaryPreferredNetworkType();
      },
      notifyError: aErrorMsg => {
        currentPreferredNetworkType =
          Ci.nsIMobileConnection.PREFERRED_NETWORK_TYPE_LTE_WCDMA_GSM_CDMA_EVDO;
        setTemporaryPreferredNetworkType();
      }
    });
  },

  updateOperatorInfo: function(aNewInfo, aBatch = false) {
    let isUpdated = this._updateInfo(this._operatorInfo, aNewInfo);

    // Update lastKnownNetwork
    if (this._operatorInfo.mcc && this._operatorInfo.mnc) {
      let network = this._operatorInfo.mcc + "-" + this._operatorInfo.mnc;
      if (this.lastKnownNetwork !== network) {
        if (DEBUG) {
          this._debug("lastKnownNetwork now is " + network);
        }

        this.lastKnownNetwork = network;
        this.deliverListenerEvent("notifyLastKnownNetworkChanged");
      }
    }

    // If the voice is unregistered, no need to send notification.
    if (this.voice.state !== RIL.GECKO_MOBILE_CONNECTION_STATE_REGISTERED &&
        isUpdated && !aBatch) {
      this.deliverListenerEvent("notifyVoiceChanged");
    }

    // If the data is unregistered, no need to send notification.
    if (this.data.state !== RIL.GECKO_MOBILE_CONNECTION_STATE_REGISTERED &&
        isUpdated && !aBatch) {
      this.deliverListenerEvent("notifyDataChanged");
    }
  },

  updateSignalInfo: function(aNewInfo, aBatch = false) {
    // If the voice is not registered, no need to update signal information.
    if (this.voice.state === RIL.GECKO_MOBILE_CONNECTION_STATE_REGISTERED) {
      if (this._updateInfo(this.voice, aNewInfo.voice) && !aBatch) {
        this.deliverListenerEvent("notifyVoiceChanged");
      }
    }

    // If the data is not registered, no need to update signal information.
    if (this.data.state === RIL.GECKO_MOBILE_CONNECTION_STATE_REGISTERED) {
      if (this._updateInfo(this.data, aNewInfo.data) && !aBatch) {
        this.deliverListenerEvent("notifyDataChanged");
      }
    }
  },

  updateRadioState: function(aMessage, aCallback = null) {
    switch (aMessage.msgType) {
      case "ExpectedRadioState":
        this._expectedRadioState = aMessage.msgData;
        break;
      case "HardwareRadioState":
        this._hardwareRadioState = aMessage.msgData;
        break;
      default:
        if (DEBUG) this._debug("updateRadioState: Invalid message type");
        return;
    }

    if (aMessage.msgType === "ExpectedRadioState" && aCallback &&
        this._hardwareRadioState === this._expectedRadioState) {
      // Early resolved
      aCallback.notifySuccess();
      return;
    }

    let newState;
    switch (this._expectedRadioState) {
      case RIL.GECKO_RADIOSTATE_ENABLED:
        newState = this._hardwareRadioState === this._expectedRadioState ?
          Ci.nsIMobileConnection.MOBILE_RADIO_STATE_ENABLED :
          Ci.nsIMobileConnection.MOBILE_RADIO_STATE_ENABLING;
        break;

      case RIL.GECKO_RADIOSTATE_DISABLED:
        newState = this._hardwareRadioState === this._expectedRadioState ?
          Ci.nsIMobileConnection.MOBILE_RADIO_STATE_DISABLED :
          Ci.nsIMobileConnection.MOBILE_RADIO_STATE_DISABLING;
        break;

      default: /* RIL.GECKO_RADIOSTATE_UNKNOWN */
        switch (this._hardwareRadioState) {
          case RIL.GECKO_RADIOSTATE_ENABLED:
            newState = Ci.nsIMobileConnection.MOBILE_RADIO_STATE_ENABLED;
            break;
          case RIL.GECKO_RADIOSTATE_DISABLED:
            newState = Ci.nsIMobileConnection.MOBILE_RADIO_STATE_DISABLED;
            break;
          default: /* RIL.GECKO_RADIOSTATE_UNKNOWN */
            newState = Ci.nsIMobileConnection.MOBILE_RADIO_STATE_UNKNOWN;
        }
    }

    // This update is triggered by underlying layers and the state is UNKNOWN
    if (aMessage.msgType === "HardwareRadioState" &&
        aMessage.msgData === RIL.GECKO_RADIOSTATE_UNKNOWN) {
      // TODO: Find a better way than just setting the radio state to UNKNOWN
      newState = Ci.nsIMobileConnection.MOBILE_RADIO_STATE_UNKNOWN;
    }

    if (newState === Ci.nsIMobileConnection.MOBILE_RADIO_STATE_ENABLING ||
        newState === Ci.nsIMobileConnection.MOBILE_RADIO_STATE_DISABLING) {
      let action = this._expectedRadioState === RIL.GECKO_RADIOSTATE_ENABLED;
      this._radioInterface.sendWorkerMessage("setRadioEnabled",
                                             {enabled: action},
                                             function(aResponse) {
        if (!aCallback) {
          return false;
        }
        if (aResponse.errorMsg) {
          aCallback.notifyError(aResponse.errorMsg);
          return false;
        }
        aCallback.notifySuccess();
        return false;
      });
    }

    if (DEBUG) this._debug("Current Radio State is '" + newState + "'");
    if (this.radioState === newState) {
      return;
    }
    this.radioState = newState;
    this.deliverListenerEvent("notifyRadioStateChanged");
  },

  notifyCFStateChanged: function(aAction, aReason, aNumber, aTimeSeconds,
                                 aServiceClass) {
    this.deliverListenerEvent("notifyCFStateChanged",
                              [aAction, aReason, aNumber, aTimeSeconds,
                               aServiceClass]);
  },

  getSupportedNetworkTypes: function(aTypes) {
    aTypes.value = this.supportedNetworkTypes.slice();
    return aTypes.value.length;
  },

  getNetworks: function(aCallback) {
    this._radioInterface.sendWorkerMessage("getAvailableNetworks", null,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      let networks = aResponse.networks;
      for (let i = 0; i < networks.length; i++) {
        let info = new MobileNetworkInfo();
        this._updateInfo(info, networks[i]);
        networks[i] = info;
      }

      aCallback.notifyGetNetworksSuccess(networks.length, networks);
      return false;
    }).bind(this));
  },

  selectNetwork: function(aNetwork, aCallback) {
    if (!aNetwork ||
        isNaN(parseInt(aNetwork.mcc, 10)) ||
        isNaN(parseInt(aNetwork.mnc, 10))) {
      this._dispatchNotifyError(aCallback, RIL.GECKO_ERROR_INVALID_PARAMETER);
      return;
    }

    if (this._selectingNetwork) {
      this._dispatchNotifyError(aCallback, "AlreadySelectingANetwork");
      return;
    }

    let options = {mcc: aNetwork.mcc, mnc: aNetwork.mnc};
    this._selectingNetwork = options;
    this._radioInterface.sendWorkerMessage("selectNetwork", options,
                                           (function(aResponse) {
      this._selectingNetwork = null;
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  selectNetworkAutomatically: function(aCallback) {
    if (this._selectingNetwork) {
      this._dispatchNotifyError(aCallback, "AlreadySelectingANetwork");
      return;
    }

    this._selectingNetwork = "automatic";
    this._radioInterface.sendWorkerMessage("selectNetworkAuto", null,
                                           (function(aResponse) {
      this._selectingNetwork = null;
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  setPreferredNetworkType: function(aType, aCallback) {
    if (this.radioState !== Ci.nsIMobileConnection.MOBILE_RADIO_STATE_ENABLED) {
      this._dispatchNotifyError(aCallback, RIL.GECKO_ERROR_RADIO_NOT_AVAILABLE);
      return;
    }

    this._radioInterface.sendWorkerMessage("setPreferredNetworkType",
                                           {type: aType},
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  getPreferredNetworkType: function(aCallback) {
    if (this.radioState !== Ci.nsIMobileConnection.MOBILE_RADIO_STATE_ENABLED) {
      this._dispatchNotifyError(aCallback, RIL.GECKO_ERROR_RADIO_NOT_AVAILABLE);
      return;
    }

    this._radioInterface.sendWorkerMessage("getPreferredNetworkType", null,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifyGetPreferredNetworkTypeSuccess(aResponse.type);
      return false;
    }).bind(this));
  },

  setRoamingPreference: function(aMode, aCallback) {
    this._radioInterface.sendWorkerMessage("setRoamingPreference",
                                           {mode: aMode},
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  getRoamingPreference: function(aCallback) {
    this._radioInterface.sendWorkerMessage("queryRoamingPreference", null,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifyGetRoamingPreferenceSuccess(aResponse.mode);
      return false;
    }).bind(this));
  },

  setVoicePrivacyMode: function(aEnabled, aCallback) {
    this._radioInterface.sendWorkerMessage("setVoicePrivacyMode",
                                           {enabled: aEnabled},
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  getVoicePrivacyMode: function(aCallback) {
    this._radioInterface.sendWorkerMessage("queryVoicePrivacyMode", null,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccessWithBoolean(aResponse.enabled);
      return false;
    }).bind(this));
  },

  setCallForwarding: function(aAction, aReason, aNumber, aTimeSeconds,
                              aServiceClass, aCallback) {
    let options = {
      action: aAction,
      reason: aReason,
      number: aNumber,
      timeSeconds: aTimeSeconds,
      serviceClass: RIL.ICC_SERVICE_CLASS_VOICE
    };

    this._radioInterface.sendWorkerMessage("setCallForward", options,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      this.notifyCFStateChanged(aResponse.action, aResponse.reason,
                                aResponse.number, aResponse.timeSeconds,
                                aResponse.serviceClass);
      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  getCallForwarding: function(aReason, aCallback) {
    this._radioInterface.sendWorkerMessage("queryCallForwardStatus",
                                           {reason: aReason},
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      let infos = this._rulesToCallForwardingOptions(aResponse.rules);
      aCallback.notifyGetCallForwardingSuccess(infos.length, infos);
      return false;
    }).bind(this));
  },

  setCallBarring: function(aProgram, aEnabled, aPassword, aServiceClass,
                           aCallback) {
    let options = {
      program: aProgram,
      enabled: aEnabled,
      password: aPassword,
      serviceClass: aServiceClass
    };

    this._radioInterface.sendWorkerMessage("setCallBarring", options,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  getCallBarring: function(aProgram, aPassword, aServiceClass, aCallback) {
    let options = {
      program: aProgram,
      password: aPassword,
      serviceClass: aServiceClass
    };

    this._radioInterface.sendWorkerMessage("queryCallBarringStatus", options,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifyGetCallBarringSuccess(aResponse.program,
                                            aResponse.enabled,
                                            aResponse.serviceClass);
      return false;
    }).bind(this));
  },

  changeCallBarringPassword: function(aPin, aNewPin, aCallback) {
    let options = {
      pin: aPin,
      newPin: aNewPin
    };

    this._radioInterface.sendWorkerMessage("changeCallBarringPassword", options,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  setCallWaiting: function(aEnabled, aCallback) {
    this._radioInterface.sendWorkerMessage("setCallWaiting",
                                           {enabled: aEnabled},
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  getCallWaiting: function(aCallback) {
    this._radioInterface.sendWorkerMessage("queryCallWaiting", null,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccessWithBoolean(aResponse.enabled);
      return false;
    }).bind(this));
  },

  setCallingLineIdRestriction: function(aMode, aCallback) {
    if (!this._isValidClirMode(aMode)) {
      this._dispatchNotifyError(aCallback, RIL.GECKO_ERROR_INVALID_PARAMETER);
      return;
    }

    if (this.radioState !== Ci.nsIMobileConnection.MOBILE_RADIO_STATE_ENABLED) {
      this._dispatchNotifyError(aCallback, RIL.GECKO_ERROR_RADIO_NOT_AVAILABLE);
      return;
    }

    this._radioInterface.sendWorkerMessage("setCLIR", {clirMode: aMode},
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      this.deliverListenerEvent("notifyClirModeChanged", [aResponse.mode]);
      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  getCallingLineIdRestriction: function(aCallback) {
    if (this.radioState !== Ci.nsIMobileConnection.MOBILE_RADIO_STATE_ENABLED) {
      this._dispatchNotifyError(aCallback, RIL.GECKO_ERROR_RADIO_NOT_AVAILABLE);
      return;
    }

    this._radioInterface.sendWorkerMessage("getCLIR", null,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifyGetClirStatusSuccess(aResponse.n, aResponse.m);
      return false;
    }).bind(this));
  },

  exitEmergencyCbMode: function(aCallback) {
    this._radioInterface.sendWorkerMessage("exitEmergencyCbMode", null,
                                           (function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyError(aResponse.errorMsg);
        return false;
      }

      aCallback.notifySuccess();
      return false;
    }).bind(this));
  },

  setRadioEnabled: function(aEnabled, aCallback) {
    if (DEBUG) {
      this._debug("setRadioEnabled: " + aEnabled);
    }

    // Before sending a equest to |ril_worker.js|, we should check radioState.
    switch (this.radioState) {
      case Ci.nsIMobileConnection.MOBILE_RADIO_STATE_UNKNOWN:
      case Ci.nsIMobileConnection.MOBILE_RADIO_STATE_ENABLED:
      case Ci.nsIMobileConnection.MOBILE_RADIO_STATE_DISABLED:
        break;
      default:
        aCallback.notifyError("InvalidStateError");
        return;
    }

    let message = {
      msgType: "ExpectedRadioState",
      msgData: (aEnabled ? RIL.GECKO_RADIOSTATE_ENABLED :
                           RIL.GECKO_RADIOSTATE_DISABLED)
    };
    this.updateRadioState(message, aCallback);
  },

  getCellInfoList: function(aCallback) {
    this._radioInterface.sendWorkerMessage("getCellInfoList",
                                           null,
                                           function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyGetCellInfoListFailed(aResponse.errorMsg);
        return;
      }

      let cellInfoList = [];
      let count = aResponse.result.length;
      for (let i = 0; i < count; i++) {
        let srcCellInfo = aResponse.result[i];
        let cellInfo;
        switch (srcCellInfo.type) {
          case RIL.CELL_INFO_TYPE_GSM:
            cellInfo = new GsmCellInfo();
            break;
          case RIL.CELL_INFO_TYPE_WCDMA:
            cellInfo = new WcdmaCellInfo();
            break;
          case RIL.CELL_INFO_TYPE_LTE:
            cellInfo = new LteCellInfo();
            break;
          case RIL.CELL_INFO_TYPE_CDMA:
            cellInfo = new CdmaCellInfo();
            break;
        }

        if (!cellInfo) {
          continue;
        }
        this._updateInfo(cellInfo, srcCellInfo);
        cellInfoList.push(cellInfo);
      }
      aCallback.notifyGetCellInfoList(count, cellInfoList);
    }.bind(this));
  },

  getNeighboringCellIds: function(aCallback) {
    this._radioInterface.sendWorkerMessage("getNeighboringCellIds",
                                           null,
                                           function(aResponse) {
      if (aResponse.errorMsg) {
        aCallback.notifyGetNeighboringCellIdsFailed(aResponse.errorMsg);
        return;
      }

      let neighboringCellIds = [];
      let count = aResponse.result.length;
      for (let i = 0; i < count; i++) {
        let srcCellInfo = aResponse.result[i];
        let cellInfo = new NeighboringCellInfo();
        this._updateInfo(cellInfo, srcCellInfo);
        neighboringCellIds.push(cellInfo);
      }
      aCallback.notifyGetNeighboringCellIds(count, neighboringCellIds);

    }.bind(this));
  },
};

function MobileConnectionService() {
  this._providers = [];

  let numClients = gRadioInterfaceLayer.numRadioInterfaces;
  for (let i = 0; i < numClients; i++) {
    let radioInterface = gRadioInterfaceLayer.getRadioInterface(i);
    let provider = new MobileConnectionProvider(i, radioInterface);
    this._providers.push(provider);
  }

  Services.prefs.addObserver(kPrefRilDebuggingEnabled, this, false);
  Services.obs.addObserver(this, NS_NETWORK_ACTIVE_CHANGED_TOPIC_ID, false);
  Services.obs.addObserver(this, NS_DATA_CALL_ERROR_TOPIC_ID, false);
  Services.obs.addObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID, false);

  debug("init complete");
}
MobileConnectionService.prototype = {
  classID: GONK_MOBILECONNECTIONSERVICE_CID,
  classInfo: XPCOMUtils.generateCI({classID: GONK_MOBILECONNECTIONSERVICE_CID,
                                    contractID: GONK_MOBILECONNECTIONSERVICE_CONTRACTID,
                                    classDescription: "MobileConnectionService",
                                    interfaces: [Ci.nsIGonkMobileConnectionService,
                                                 Ci.nsIMobileConnectionService],
                                    flags: Ci.nsIClassInfo.SINGLETON}),
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIGonkMobileConnectionService,
                                         Ci.nsIMobileConnectionService,
                                         Ci.nsIObserver]),

  // An array of MobileConnectionProvider instances.
  _providers: null,

  _shutdown: function() {
    Services.prefs.removeObserver(kPrefRilDebuggingEnabled, this);
    Services.obs.removeObserver(this, NS_NETWORK_ACTIVE_CHANGED_TOPIC_ID);
    Services.obs.removeObserver(this, NS_DATA_CALL_ERROR_TOPIC_ID);
    Services.obs.removeObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID);
  },

  _updateDebugFlag: function() {
    try {
      DEBUG = RIL.DEBUG_RIL ||
              Services.prefs.getBoolPref(kPrefRilDebuggingEnabled);
    } catch (e) {}
  },

  /**
   * nsIMobileConnectionService interface.
   */
  get numItems() {
    return this._providers.length;
  },

  getItemByServiceId: function(aServiceId) {
    let provider = this._providers[aServiceId];
    if (!provider) {
      throw Cr.NS_ERROR_UNEXPECTED;
    }

    return provider;
  },

  /**
   * nsIGonkMobileConnectionService interface.
   */
  notifyVoiceInfoChanged: function(aClientId, aVoiceInfo) {
    if (DEBUG) {
      debug("notifyVoiceInfoChanged for " + aClientId + ": " +
            JSON.stringify(aVoiceInfo));
    }

    this.getItemByServiceId(aClientId).updateVoiceInfo(aVoiceInfo);
  },

  notifyDataInfoChanged: function(aClientId, aDataInfo) {
    if (DEBUG) {
      debug("notifyDataInfoChanged for " + aClientId + ": " +
            JSON.stringify(aDataInfo));
    }

    this.getItemByServiceId(aClientId).updateDataInfo(aDataInfo);
  },

  notifyDataError: function(aClientId, aMessage) {
    if (DEBUG) {
      debug("notifyDataError for " + aClientId + ": " + aMessage);
    }

    this.getItemByServiceId(aClientId)
        .deliverListenerEvent("notifyDataError", [aMessage]);
  },

  notifyEmergencyCallbackModeChanged: function(aClientId, aActive, aTimeoutMs) {
    if (DEBUG) {
      debug("notifyEmergencyCbModeChanged for " + aClientId + ": " +
            JSON.stringify({active: aActive, timeoutMs: aTimeoutMs}));
    }

    this.getItemByServiceId(aClientId)
        .deliverListenerEvent("notifyEmergencyCbModeChanged",
                              [aActive, aTimeoutMs]);
  },

  notifyOtaStatusChanged: function(aClientId, aStatus) {
    if (DEBUG) {
      debug("notifyOtaStatusChanged for " + aClientId + ": " + aStatus);
    }

    this.getItemByServiceId(aClientId)
        .deliverListenerEvent("notifyOtaStatusChanged", [aStatus]);
  },

  notifyRadioStateChanged: function(aClientId, aRadioState) {
    if (DEBUG) {
      debug("notifyRadioStateChanged for " + aClientId + ": " + aRadioState);
    }

    let message = {
      msgType: "HardwareRadioState",
      msgData: aRadioState
    };
    this.getItemByServiceId(aClientId).updateRadioState(message);
  },

  notifyNetworkInfoChanged: function(aClientId, aNetworkInfo) {
    if (DEBUG) {
      debug("notifyNetworkInfoChanged for " + aClientId + ": " +
            JSON.stringify(aNetworkInfo));
    }

    let provider = this.getItemByServiceId(aClientId);

    let isVoiceUpdated = false;
    let isDataUpdated = false;
    let operatorMessage = aNetworkInfo[RIL.NETWORK_INFO_OPERATOR];
    let voiceMessage = aNetworkInfo[RIL.NETWORK_INFO_VOICE_REGISTRATION_STATE];
    let dataMessage = aNetworkInfo[RIL.NETWORK_INFO_DATA_REGISTRATION_STATE];
    let signalMessage = aNetworkInfo[RIL.NETWORK_INFO_SIGNAL];
    let selectionMessage = aNetworkInfo[RIL.NETWORK_INFO_NETWORK_SELECTION_MODE];

    // Batch the *InfoChanged messages together
    if (operatorMessage) {
      provider.updateOperatorInfo(operatorMessage, true);
    }

    if (voiceMessage) {
      provider.updateVoiceInfo(voiceMessage, true);
    }

    if (dataMessage) {
      provider.updateDataInfo(dataMessage, true);
    }

    if (signalMessage) {
      provider.updateSignalInfo(signalMessage, true);
    }

    if (selectionMessage) {
      this.notifyNetworkSelectModeChanged(aClientId, selectionMessage.mode);
    }

    if (voiceMessage || operatorMessage || signalMessage) {
      provider.deliverListenerEvent("notifyVoiceChanged");
    }

    if (dataMessage || operatorMessage || signalMessage) {
      provider.deliverListenerEvent("notifyDataChanged");
    }
  },

  notifySignalStrengthChanged: function(aClientId, aSignal) {
    if (DEBUG) {
      debug("notifySignalStrengthChanged for " + aClientId + ": " +
            JSON.stringify(aSignal));
    }

    this.getItemByServiceId(aClientId).updateSignalInfo(aSignal);
  },

  notifyOperatorChanged: function(aClientId, aOperator) {
    if (DEBUG) {
      debug("notifyOperatorChanged for " + aClientId + ": " +
            JSON.stringify(aOperator));
    }

    this.getItemByServiceId(aClientId).updateOperatorInfo(aOperator);
  },

  notifyNetworkSelectModeChanged: function(aClientId, aMode) {
    if (DEBUG) {
      debug("notifyNetworkSelectModeChanged for " + aClientId + ": " + aMode);
    }

    let provider = this.getItemByServiceId(aClientId);
    if (provider.networkSelectionMode === aMode) {
      return;
    }

    provider.networkSelectionMode = aMode;
    provider.deliverListenerEvent("notifyNetworkSelectionModeChanged");
  },

  notifySpnAvailable: function(aClientId) {
    if (DEBUG) {
      debug("notifySpnAvailable for " + aClientId);
    }

    let provider = this.getItemByServiceId(aClientId);

    // Update voice roaming state
    provider.updateVoiceInfo({});

    // Update data roaming state
    provider.updateDataInfo({});
  },

  notifyLastHomeNetworkChanged: function(aClientId, aNetwork) {
    if (DEBUG) {
      debug("notifyLastHomeNetworkChanged for " + aClientId + ": " + aNetwork);
    }

    let provider = this.getItemByServiceId(aClientId);
    if (provider.lastKnownHomeNetwork === aNetwork) {
      return;
    }

    provider.lastKnownHomeNetwork = aNetwork;
    provider.deliverListenerEvent("notifyLastKnownHomeNetworkChanged");
  },

  notifyCFStateChanged: function(aClientId, aAction, aReason, aNumber,
                                 aTimeSeconds, aServiceClass) {
    if (DEBUG) {
      debug("notifyCFStateChanged for " + aClientId);
    }

    let provider = this.getItemByServiceId(aClientId);
    provider.notifyCFStateChanged(aAction, aReason, aNumber, aTimeSeconds,
                                  aServiceClass);
  },

  notifyCdmaInfoRecDisplay: function(aClientId, aDisplay) {
    gMobileConnectionMessenger.notifyCdmaInfoRecDisplay(aClientId, aDisplay);
  },

  notifyCdmaInfoRecCalledPartyNumber: function(aClientId, aType, aPlan, aNumber,
                                               aPi, aSi) {
    gMobileConnectionMessenger
      .notifyCdmaInfoRecCalledPartyNumber(aClientId, aType, aPlan, aNumber,
                                          aPi, aSi);
  },

  notifyCdmaInfoRecCallingPartyNumber: function(aClientId, aType, aPlan, aNumber,
                                                aPi, aSi) {
    gMobileConnectionMessenger
      .notifyCdmaInfoRecCallingPartyNumber(aClientId, aType, aPlan, aNumber,
                                           aPi, aSi);
  },

  notifyCdmaInfoRecConnectedPartyNumber: function(aClientId, aType, aPlan, aNumber,
                                                  aPi, aSi) {
    gMobileConnectionMessenger
      .notifyCdmaInfoRecConnectedPartyNumber(aClientId, aType, aPlan, aNumber,
                                             aPi, aSi);
  },

  notifyCdmaInfoRecSignal: function(aClientId, aType, aAlertPitch, aSignal){
    gMobileConnectionMessenger
      .notifyCdmaInfoRecSignal(aClientId, aType, aAlertPitch, aSignal);
  },

  notifyCdmaInfoRecRedirectingNumber: function(aClientId, aType, aPlan, aNumber,
                                               aPi, aSi, aReason) {
    gMobileConnectionMessenger
      .notifyCdmaInfoRecRedirectingNumber(aClientId, aType, aPlan, aNumber,
                                          aPi, aSi, aReason);
  },

  notifyCdmaInfoRecLineControl: function(aClientId, aPolarityIncluded, aToggle,
                                         aReverse, aPowerDenial) {
    gMobileConnectionMessenger
      .notifyCdmaInfoRecLineControl(aClientId, aPolarityIncluded, aToggle,
                                    aReverse, aPowerDenial);
  },

  notifyCdmaInfoRecClir: function(aClientId, aCause) {
    gMobileConnectionMessenger.notifyCdmaInfoRecClir(aClientId, aCause);
  },

  notifyCdmaInfoRecAudioControl: function(aClientId, aUpLink, aDownLink) {
    gMobileConnectionMessenger
      .notifyCdmaInfoRecAudioControl(aClientId, aUpLink, aDownLink);
  },

  /**
   * nsIObserver interface.
   */
  observe: function(aSubject, aTopic, aData) {
    switch (aTopic) {
      case NS_NETWORK_ACTIVE_CHANGED_TOPIC_ID:
        for (let i = 0; i < this.numItems; i++) {
          let provider = this._providers[i];
          // Update connected flag only.
          provider.updateDataInfo({});
        }
        break;
      case NS_DATA_CALL_ERROR_TOPIC_ID:
        try {
          if (aSubject instanceof Ci.nsIRilNetworkInfo) {
            let rilInfo = aSubject.QueryInterface(Ci.nsIRilNetworkInfo);
            this.notifyDataError(rilInfo.serviceId, aData);
          }
        } catch (e) {}
        break;
      case NS_PREFBRANCH_PREFCHANGE_TOPIC_ID:
        if (aData === kPrefRilDebuggingEnabled) {
          this._updateDebugFlag();
        }
        break;
      case NS_XPCOM_SHUTDOWN_OBSERVER_ID:
        this._shutdown();
        break;
    }
  }
};

this.NSGetFactory = XPCOMUtils.generateNSGetFactory([MobileConnectionService]);
