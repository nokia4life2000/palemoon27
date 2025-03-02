/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- /
/* vim: set shiftwidth=2 tabstop=2 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

dump("###################################### BrowserElementCopyPaste.js loaded\n");

let CopyPasteAssistent = {
  COMMAND_MAP: {
    'cut': 'cmd_cut',
    'copy': 'cmd_copyAndCollapseToEnd',
    'paste': 'cmd_paste',
    'selectall': 'cmd_selectAll'
  },

  init: function() {
    addEventListener('mozcaretstatechanged',
                     this._caretStateChangedHandler.bind(this),
                     /* useCapture = */ true,
                     /* wantsUntrusted = */ false);
    addMessageListener('browser-element-api:call', this._browserAPIHandler.bind(this));
  },

  _browserAPIHandler: function(e) {
    switch (e.data.msg_name) {
      case 'copypaste-do-command':
        if (this._isCommandEnabled(e.data.command)) {
          docShell.doCommand(COMMAND_MAP[e.data.command]);
        }
        break;
    }
  },

  _isCommandEnabled: function(cmd) {
    let command = this.COMMAND_MAP[cmd];
    if (!command) {
      return false;
    }

    return docShell.isCommandEnabled(command);
  },

  _caretStateChangedHandler: function(e) {
    e.stopPropagation();

    let boundingClientRect = e.boundingClientRect;
    let canPaste = this._isCommandEnabled("paste");
    let zoomFactor = content.innerWidth == 0 ? 1 : content.screen.width / content.innerWidth;

    let detail = {
      rect: {
        width: boundingClientRect ? boundingClientRect.width : 0,
        height: boundingClientRect ? boundingClientRect.height : 0,
        top: boundingClientRect ? boundingClientRect.top : 0,
        bottom: boundingClientRect ? boundingClientRect.bottom : 0,
        left: boundingClientRect ? boundingClientRect.left : 0,
        right: boundingClientRect ? boundingClientRect.right : 0,
      },
      commands: {
        canSelectAll: this._isCommandEnabled("selectall"),
        canCut: this._isCommandEnabled("cut"),
        canCopy: this._isCommandEnabled("copy"),
        canPaste: this._isCommandEnabled("paste"),
      },
      zoomFactor: zoomFactor,
      reason: e.reason,
      collapsed: e.collapsed,
      caretVisible: e.caretVisible,
      selectionVisible: e.selectionVisible,
      selectionEditable: e.selectionEditable
    };

    // Get correct geometry information if we have nested iframe.
    let currentWindow = e.target.defaultView;
    while (currentWindow.realFrameElement) {
      let currentRect = currentWindow.realFrameElement.getBoundingClientRect();
      detail.rect.top += currentRect.top;
      detail.rect.bottom += currentRect.top;
      detail.rect.left += currentRect.left;
      detail.rect.right += currentRect.left;
      currentWindow = currentWindow.realFrameElement.ownerDocument.defaultView;
    }

    sendAsyncMsg('caretstatechanged', detail);
  },
};

CopyPasteAssistent.init();
