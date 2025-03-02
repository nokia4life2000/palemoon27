/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

var Cc = Components.classes;
var Ci = Components.interfaces;
var Cu = Components.utils;

this.EXPORTED_SYMBOLS = [ "ContentClick" ];

Cu.import("resource:///modules/PlacesUIUtils.jsm");
Cu.import("resource://gre/modules/PrivateBrowsingUtils.jsm");
Cu.import("resource://gre/modules/Services.jsm");

var ContentClick = {
  init: function() {
    let mm = Cc["@mozilla.org/globalmessagemanager;1"].getService(Ci.nsIMessageListenerManager);
    mm.addMessageListener("Content:Click", this);
  },

  receiveMessage: function (message) {
    switch (message.name) {
      case "Content:Click":
        this.contentAreaClick(message.json, message.target)
        break;
    }
  },

  contentAreaClick: function (json, browser) {
    // This is heavily based on contentAreaClick from browser.js (Bug 903016)
    // The json is set up in a way to look like an Event.
    let window = browser.ownerDocument.defaultView;

    if (!json.href) {
      // Might be middle mouse navigation.
      if (Services.prefs.getBoolPref("middlemouse.contentLoadURL") &&
          !Services.prefs.getBoolPref("general.autoScroll")) {
        window.middleMousePaste(json);
      }
      return;
    }

    if (json.bookmark) {
      // This is the Opera convention for a special link that, when clicked,
      // allows to add a sidebar panel.  The link's title attribute contains
      // the title that should be used for the sidebar panel.
      PlacesUIUtils.showBookmarkDialog({ action: "add"
                                       , type: "bookmark"
                                       , uri: Services.io.newURI(json.href, null, null)
                                       , title: json.title
                                       , loadBookmarkInSidebar: true
                                       , hiddenRows: [ "description"
                                                     , "location"
                                                     , "keyword" ]
                                       }, window);
      return;
    }

    // Note: We don't need the sidebar code here.

    // This part is based on handleLinkClick.
    var where = window.whereToOpenLink(json);
    if (where == "current")
      return false;

    // Todo(903022): code for where == save

    window.openLinkIn(json.href, where, { referrerURI: browser.documentURI,
                                          charset: browser.characterSet });

    // Mark the page as a user followed link.  This is done so that history can
    // distinguish automatic embed visits from user activated ones.  For example
    // pages loaded in frames are embed visits and lost with the session, while
    // visits across frames should be preserved.
    try {
      if (!PrivateBrowsingUtils.isWindowPrivate(window))
        PlacesUIUtils.markPageAsFollowedLink(href);
    } catch (ex) { /* Skip invalid URIs. */ }
  }
};
