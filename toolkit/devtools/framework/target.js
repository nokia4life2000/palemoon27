/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {Cc, Ci, Cu} = require("chrome");
const {Promise: promise} = require("resource://gre/modules/Promise.jsm");
const EventEmitter = require("devtools/toolkit/event-emitter");

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "DebuggerServer",
  "resource://gre/modules/devtools/dbg-server.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "DebuggerClient",
  "resource://gre/modules/devtools/dbg-client.jsm");

const targets = new WeakMap();
const promiseTargets = new WeakMap();

/**
 * Functions for creating Targets
 */
exports.TargetFactory = {
  /**
   * Construct a Target
   * @param {XULTab} tab
   *        The tab to use in creating a new target.
   *
   * @return A target object
   */
  forTab: function TF_forTab(tab) {
    let target = targets.get(tab);
    if (target == null) {
      target = new TabTarget(tab);
      targets.set(tab, target);
    }
    return target;
  },

  /**
   * Return a promise of a Target for a remote tab.
   * @param {Object} options
   *        The options object has the following properties:
   *        {
   *          form: the remote protocol form of a tab,
   *          client: a DebuggerClient instance
   *                  (caller owns this and is responsible for closing),
   *          chrome: true if the remote target is the whole process
   *        }
   *
   * @return A promise of a target object
   */
  forRemoteTab: function TF_forRemoteTab(options) {
    let targetPromise = promiseTargets.get(options);
    if (targetPromise == null) {
      let target = new TabTarget(options);
      targetPromise = target.makeRemote().then(() => target);
      promiseTargets.set(options, targetPromise);
    }
    return targetPromise;
  },

  forWorker: function TF_forWorker(workerClient) {
    let target = targets.get(workerClient);
    if (target == null) {
      target = new WorkerTarget(workerClient);
      targets.set(workerClient, target);
    }
    return target;
  },

  /**
   * Creating a target for a tab that is being closed is a problem because it
   * allows a leak as a result of coming after the close event which normally
   * clears things up. This function allows us to ask if there is a known
   * target for a tab without creating a target
   * @return true/false
   */
  isKnownTab: function TF_isKnownTab(tab) {
    return targets.has(tab);
  },

  /**
   * Construct a Target
   * @param {nsIDOMWindow} window
   *        The chromeWindow to use in creating a new target
   * @return A target object
   */
  forWindow: function TF_forWindow(window) {
    let target = targets.get(window);
    if (target == null) {
      target = new WindowTarget(window);
      targets.set(window, target);
    }
    return target;
  },

  /**
   * Get all of the targets known to the local browser instance
   * @return An array of target objects
   */
  allTargets: function TF_allTargets() {
    let windows = [];
    let wm = Cc["@mozilla.org/appshell/window-mediator;1"]
                       .getService(Ci.nsIWindowMediator);
    let en = wm.getXULWindowEnumerator(null);
    while (en.hasMoreElements()) {
      windows.push(en.getNext());
    }

    return windows.map(function(window) {
      return TargetFactory.forWindow(window);
    });
  },
};

/**
 * The 'version' property allows the developer tools equivalent of browser
 * detection. Browser detection is evil, however while we don't know what we
 * will need to detect in the future, it is an easy way to postpone work.
 * We should be looking to use the support features added in bug 1069673
 * in place of version where possible.
 */
function getVersion() {
  // FIXME: return something better
  return 20;
}

/**
 * A Target represents something that we can debug. Targets are generally
 * read-only. Any changes that you wish to make to a target should be done via
 * a Tool that attaches to the target. i.e. a Target is just a pointer saying
 * "the thing to debug is over there".
 *
 * Providing a generalized abstraction of a web-page or web-browser (available
 * either locally or remotely) is beyond the scope of this class (and maybe
 * also beyond the scope of this universe) However Target does attempt to
 * abstract some common events and read-only properties common to many Tools.
 *
 * Supported read-only properties:
 * - name, isRemote, url
 *
 * Target extends EventEmitter and provides support for the following events:
 * - close: The target window has been closed. All tools attached to this
 *     target should close. This event is not currently cancelable.
 * - navigate: The target window has navigated to a different URL
 *
 * Optional events:
 * - will-navigate: The target window will navigate to a different URL
 * - hidden: The target is not visible anymore (for TargetTab, another tab is selected)
 * - visible: The target is visible (for TargetTab, tab is selected)
 *
 * Comparing Targets: 2 instances of a Target object can point at the same
 * thing, so t1 !== t2 and t1 != t2 even when they represent the same object.
 * To compare to targets use 't1.equals(t2)'.
 */
function Target() {
  throw new Error("Use TargetFactory.newXXX or Target.getXXX to create a Target in place of 'new Target()'");
}

Object.defineProperty(Target.prototype, "version", {
  get: getVersion,
  enumerable: true
});


/**
 * A TabTarget represents a page living in a browser tab. Generally these will
 * be web pages served over http(s), but they don't have to be.
 */
function TabTarget(tab) {
  EventEmitter.decorate(this);
  this.destroy = this.destroy.bind(this);
  this._handleThreadState = this._handleThreadState.bind(this);
  this.on("thread-resumed", this._handleThreadState);
  this.on("thread-paused", this._handleThreadState);
  this.activeTab = this.activeConsole = null;
  // Only real tabs need initialization here. Placeholder objects for remote
  // targets will be initialized after a makeRemote method call.
  if (tab && !["client", "form", "chrome"].every(tab.hasOwnProperty, tab)) {
    this._tab = tab;
    this._setupListeners();
  } else {
    this._form = tab.form;
    this._client = tab.client;
    this._chrome = tab.chrome;
  }
  // Default isTabActor to true if not explicitely specified
  this._isTabActor = typeof(tab.isTabActor) == "boolean" ? tab.isTabActor : true;
}

TabTarget.prototype = {
  _webProgressListener: null,

  /**
   * Returns a promise for the protocol description from the root actor.
   * Used internally with `target.actorHasMethod`. Takes advantage of
   * caching if definition was fetched previously with the corresponding
   * actor information. Must be a remote target.
   *
   * @return {Promise}
   * {
   *   "category": "actor",
   *   "typeName": "longstractor",
   *   "methods": [{
   *     "name": "substring",
   *     "request": {
   *       "type": "substring",
   *       "start": {
   *         "_arg": 0,
   *         "type": "primitive"
   *       },
   *       "end": {
   *         "_arg": 1,
   *         "type": "primitive"
   *       }
   *     },
   *     "response": {
   *       "substring": {
   *         "_retval": "primitive"
   *       }
   *     }
   *   }],
   *  "events": {}
   * }
   */
  getActorDescription: function (actorName) {
    if (!this.client) {
      throw new Error("TabTarget#getActorDescription() can only be called on remote tabs.");
    }

    let deferred = promise.defer();

    if (this._protocolDescription && this._protocolDescription.types[actorName]) {
      deferred.resolve(this._protocolDescription.types[actorName]);
    } else {
      this.client.mainRoot.protocolDescription(description => {
        this._protocolDescription = description;
        deferred.resolve(description.types[actorName]);
      });
    }

    return deferred.promise;
  },

  /**
   * Returns a boolean indicating whether or not the specific actor
   * type exists. Must be a remote target.
   *
   * @param {String} actorName
   * @return {Boolean}
   */
  hasActor: function (actorName) {
    if (!this.client) {
      throw new Error("TabTarget#hasActor() can only be called on remote tabs.");
    }
    if (this.form) {
      return !!this.form[actorName + "Actor"];
    }
    return false;
  },

  /**
   * Queries the protocol description to see if an actor has
   * an available method. The actor must already be lazily-loaded,
   * so this is for use inside of tool. Returns a promise that
   * resolves to a boolean. Must be a remote target.
   *
   * @param {String} actorName
   * @param {String} methodName
   * @return {Promise}
   */
  actorHasMethod: function (actorName, methodName) {
    if (!this.client) {
      throw new Error("TabTarget#actorHasMethod() can only be called on remote tabs.");
    }
    return this.getActorDescription(actorName).then(desc => {
      if (desc && desc.methods) {
        return !!desc.methods.find(method => method.name === methodName);
      }
      return false;
    });
  },

  /**
   * Returns a trait from the root actor.
   *
   * @param {String} traitName
   * @return {Mixed}
   */
  getTrait: function (traitName) {
    if (!this.client) {
      throw new Error("TabTarget#getTrait() can only be called on remote tabs.");
    }

    // If the targeted actor exposes traits and has a defined value for this traits,
    // override the root actor traits
    if (this.form.traits && traitName in this.form.traits) {
      return this.form.traits[traitName];
    }

    return this.client.traits[traitName];
  },

  get version() { return getVersion(); },

  get tab() {
    return this._tab;
  },

  get form() {
    return this._form;
  },

  get root() {
    return this._root;
  },

  get client() {
    return this._client;
  },

  // Tells us if we are debugging content document
  // or if we are debugging chrome stuff.
  // Allows to controls which features are available against
  // a chrome or a content document.
  get chrome() {
    return this._chrome;
  },

  // Tells us if the related actor implements TabActor interface
  // and requires to call `attach` request before being used
  // and `detach` during cleanup
  get isTabActor() {
    return this._isTabActor;
  },

  get window() {
    // XXX - this is a footgun for e10s - there .contentWindow will be null,
    // and even though .contentWindowAsCPOW *might* work, it will not work
    // in all contexts.  Consumers of .window need to be refactored to not
    // rely on this.
    if (Services.appinfo.processType != Ci.nsIXULRuntime.PROCESS_TYPE_DEFAULT) {
      Cu.reportError("The .window getter on devtools' |target| object isn't e10s friendly!\n"
                     + Error().stack);
    }
    // Be extra careful here, since this may be called by HS_getHudByWindow
    // during shutdown.
    if (this._tab && this._tab.linkedBrowser) {
      return this._tab.linkedBrowser.contentWindow;
    }
    return null;
  },

  get name() {
    if (this._tab && this._tab.linkedBrowser.contentDocument) {
      return this._tab.linkedBrowser.contentDocument.title
    } else if (this.isAddon) {
      return this._form.name;
    } else {
      return this._form.title;
    }
  },

  get url() {
    return this._tab ? this._tab.linkedBrowser.currentURI.spec :
                       this._form.url;
  },

  get isRemote() {
    return !this.isLocalTab;
  },

  get isAddon() {
    return !!(this._form && this._form.actor &&
              this._form.actor.match(/conn\d+\.addon\d+/));
  },

  get isLocalTab() {
    return !!this._tab;
  },

  get isMultiProcess() {
    return !this.window;
  },

  get isThreadPaused() {
    return !!this._isThreadPaused;
  },

  /**
   * Adds remote protocol capabilities to the target, so that it can be used
   * for tools that support the Remote Debugging Protocol even for local
   * connections.
   */
  makeRemote: function TabTarget_makeRemote() {
    if (this._remote) {
      return this._remote.promise;
    }

    this._remote = promise.defer();

    if (this.isLocalTab) {
      // Since a remote protocol connection will be made, let's start the
      // DebuggerServer here, once and for all tools.
      if (!DebuggerServer.initialized) {
        DebuggerServer.init();
        DebuggerServer.addBrowserActors();
      }

      this._client = new DebuggerClient(DebuggerServer.connectPipe());
      // A local TabTarget will never perform chrome debugging.
      this._chrome = false;
    }

    this._setupRemoteListeners();

    let attachTab = () => {
      this._client.attachTab(this._form.actor, (aResponse, aTabClient) => {
        if (!aTabClient) {
          this._remote.reject("Unable to attach to the tab");
          return;
        }
        this.activeTab = aTabClient;
        this.threadActor = aResponse.threadActor;
        attachConsole();
      });
    };

    let attachConsole = () => {
      this._client.attachConsole(this._form.consoleActor,
                                 [ "NetworkActivity" ],
                                 (aResponse, aWebConsoleClient) => {
        if (!aWebConsoleClient) {
          this._remote.reject("Unable to attach to the console");
          return;
        }
        this.activeConsole = aWebConsoleClient;
        this._remote.resolve(null);
      });
    };

    if (this.isLocalTab) {
      this._client.connect((aType, aTraits) => {
        this._client.listTabs(aResponse => {
          this._root = aResponse;

          if (this.window) {
            let windowUtils = this.window
              .QueryInterface(Ci.nsIInterfaceRequestor)
              .getInterface(Ci.nsIDOMWindowUtils);
            let outerWindow = windowUtils.outerWindowID;
            aResponse.tabs.some((tab) => {
              if (tab.outerWindowID === outerWindow) {
                this._form = tab;
                return true;
              }
              return false;
            });
          }

          if (!this._form) {
            this._form = aResponse.tabs[aResponse.selected];
          }
          attachTab();
        });
      });
    } else if (this.isTabActor) {
      // In the remote debugging case, the protocol connection will have been
      // already initialized in the connection screen code.
      attachTab();
    } else {
      // AddonActor and chrome debugging on RootActor doesn't inherits from TabActor and
      // doesn't need to be attached.
      attachConsole();
    }

    return this._remote.promise;
  },

  /**
   * Listen to the different events.
   */
  _setupListeners: function TabTarget__setupListeners() {
    this._webProgressListener = new TabWebProgressListener(this);
    this.tab.linkedBrowser.addProgressListener(this._webProgressListener);
    this.tab.addEventListener("TabClose", this);
    this.tab.parentNode.addEventListener("TabSelect", this);
    this.tab.ownerDocument.defaultView.addEventListener("unload", this);
  },

  /**
   * Teardown event listeners.
   */
  _teardownListeners: function TabTarget__teardownListeners() {
    if (this._webProgressListener) {
      this._webProgressListener.destroy();
    }

    this._tab.ownerDocument.defaultView.removeEventListener("unload", this);
    this._tab.removeEventListener("TabClose", this);
    this._tab.parentNode.removeEventListener("TabSelect", this);
  },

  /**
   * Setup listeners for remote debugging, updating existing ones as necessary.
   */
  _setupRemoteListeners: function TabTarget__setupRemoteListeners() {
    this.client.addListener("closed", this.destroy);

    this._onTabDetached = (aType, aPacket) => {
      // We have to filter message to ensure that this detach is for this tab
      if (aPacket.from == this._form.actor) {
        this.destroy();
      }
    };
    this.client.addListener("tabDetached", this._onTabDetached);

    this._onTabNavigated = (aType, aPacket) => {
      let event = Object.create(null);
      event.url = aPacket.url;
      event.title = aPacket.title;
      event.nativeConsoleAPI = aPacket.nativeConsoleAPI;
      event.isFrameSwitching = aPacket.isFrameSwitching;
      // Send any stored event payload (DOMWindow or nsIRequest) for backwards
      // compatibility with non-remotable tools.
      if (aPacket.state == "start") {
        event._navPayload = this._navRequest;
        this.emit("will-navigate", event);
        this._navRequest = null;
      } else {
        event._navPayload = this._navWindow;
        this.emit("navigate", event);
        this._navWindow = null;
      }
    };
    this.client.addListener("tabNavigated", this._onTabNavigated);

    this._onFrameUpdate = (aType, aPacket) => {
      this.emit("frame-update", aPacket);
    };
    this.client.addListener("frameUpdate", this._onFrameUpdate);
  },

  /**
   * Teardown listeners for remote debugging.
   */
  _teardownRemoteListeners: function TabTarget__teardownRemoteListeners() {
    this.client.removeListener("closed", this.destroy);
    this.client.removeListener("tabNavigated", this._onTabNavigated);
    this.client.removeListener("tabDetached", this._onTabDetached);
    this.client.removeListener("frameUpdate", this._onFrameUpdate);
  },

  /**
   * Handle tabs events.
   */
  handleEvent: function (event) {
    switch (event.type) {
      case "TabClose":
      case "unload":
        this.destroy();
        break;
      case "TabSelect":
        if (this.tab.selected) {
          this.emit("visible", event);
        } else {
          this.emit("hidden", event);
        }
        break;
    }
  },

  /**
   * Handle script status.
   */
  _handleThreadState: function(event) {
    switch (event) {
      case "thread-resumed":
        this._isThreadPaused = false;
        break;
      case "thread-paused":
        this._isThreadPaused = true;
        break;
    }
  },

  /**
   * Target is not alive anymore.
   */
  destroy: function() {
    // If several things call destroy then we give them all the same
    // destruction promise so we're sure to destroy only once
    if (this._destroyer) {
      return this._destroyer.promise;
    }

    this._destroyer = promise.defer();

    // Before taking any action, notify listeners that destruction is imminent.
    this.emit("close");

    // First of all, do cleanup tasks that pertain to both remoted and
    // non-remoted targets.
    this.off("thread-resumed", this._handleThreadState);
    this.off("thread-paused", this._handleThreadState);

    if (this._tab) {
      this._teardownListeners();
    }

    let cleanupAndResolve = () => {
      this._cleanup();
      this._destroyer.resolve(null);
    };
    // If this target was not remoted, the promise will be resolved before the
    // function returns.
    if (this._tab && !this._client) {
      cleanupAndResolve();
    } else if (this._client) {
      // If, on the other hand, this target was remoted, the promise will be
      // resolved after the remote connection is closed.
      this._teardownRemoteListeners();

      if (this.isLocalTab) {
        // We started with a local tab and created the client ourselves, so we
        // should close it.
        this._client.close(cleanupAndResolve);
      } else if (this.activeTab) {
        // The client was handed to us, so we are not responsible for closing
        // it. We just need to detach from the tab, if already attached.
        // |detach| may fail if the connection is already dead, so proceed with
        // cleanup directly after this.
        this.activeTab.detach();
        cleanupAndResolve();
      } else {
        cleanupAndResolve();
      }
    }

    return this._destroyer.promise;
  },

  /**
   * Clean up references to what this target points to.
   */
  _cleanup: function TabTarget__cleanup() {
    if (this._tab) {
      targets.delete(this._tab);
    } else {
      promiseTargets.delete(this._form);
    }
    this.activeTab = null;
    this.activeConsole = null;
    this._client = null;
    this._tab = null;
    this._form = null;
    this._remote = null;
  },

  toString: function() {
    return 'TabTarget:' + (this._tab ? this._tab : (this._form && this._form.actor));
  },
};


/**
 * WebProgressListener for TabTarget.
 *
 * @param object aTarget
 *        The TabTarget instance to work with.
 */
function TabWebProgressListener(aTarget) {
  this.target = aTarget;
}

TabWebProgressListener.prototype = {
  target: null,

  QueryInterface: XPCOMUtils.generateQI([Ci.nsIWebProgressListener, Ci.nsISupportsWeakReference]),

  onStateChange: function TWPL_onStateChange(progress, request, flag, status) {
    let isStart = flag & Ci.nsIWebProgressListener.STATE_START;
    let isDocument = flag & Ci.nsIWebProgressListener.STATE_IS_DOCUMENT;
    let isNetwork = flag & Ci.nsIWebProgressListener.STATE_IS_NETWORK;
    let isRequest = flag & Ci.nsIWebProgressListener.STATE_IS_REQUEST;

    // Skip non-interesting states.
    if (!isStart || !isDocument || !isRequest || !isNetwork) {
      return;
    }

    // emit event if the top frame is navigating
    if (progress.isTopLevel) {
      // Emit the event if the target is not remoted or store the payload for
      // later emission otherwise.
      if (this.target._client) {
        this.target._navRequest = request;
      } else {
        this.target.emit("will-navigate", request);
      }
    }
  },

  onProgressChange: function() {},
  onSecurityChange: function() {},
  onStatusChange: function() {},

  onLocationChange: function TWPL_onLocationChange(webProgress, request, URI, flags) {
    if (this.target &&
        !(flags & Ci.nsIWebProgressListener.LOCATION_CHANGE_SAME_DOCUMENT)) {
      let window = webProgress.DOMWindow;
      // Emit the event if the target is not remoted or store the payload for
      // later emission otherwise.
      if (this.target._client) {
        this.target._navWindow = window;
      } else {
        this.target.emit("navigate", window);
      }
    }
  },

  /**
   * Destroy the progress listener instance.
   */
  destroy: function TWPL_destroy() {
    if (this.target.tab) {
      try {
        this.target.tab.linkedBrowser.removeProgressListener(this);
      } catch (ex) {
        // This can throw when a tab crashes in e10s.
      }
    }
    this.target._webProgressListener = null;
    this.target._navRequest = null;
    this.target._navWindow = null;
    this.target = null;
  }
};


/**
 * A WindowTarget represents a page living in a xul window or panel. Generally
 * these will have a chrome: URL
 */
function WindowTarget(window) {
  EventEmitter.decorate(this);
  this._window = window;
  this._setupListeners();
}

WindowTarget.prototype = {
  get version() { return getVersion(); },

  get window() {
    return this._window;
  },

  get name() {
    return this._window.document.title;
  },

  get url() {
    return this._window.document.location.href;
  },

  get isRemote() {
    return false;
  },

  get isLocalTab() {
    return false;
  },

  get isThreadPaused() {
    return !!this._isThreadPaused;
  },

  /**
   * Listen to the different events.
   */
  _setupListeners: function() {
    this._handleThreadState = this._handleThreadState.bind(this);
    this.on("thread-paused", this._handleThreadState);
    this.on("thread-resumed", this._handleThreadState);
  },

  _handleThreadState: function(event) {
    switch (event) {
      case "thread-resumed":
        this._isThreadPaused = false;
        break;
      case "thread-paused":
        this._isThreadPaused = true;
        break;
    }
  },

  /**
   * Target is not alive anymore.
   */
  destroy: function() {
    if (!this._destroyed) {
      this._destroyed = true;

      this.off("thread-paused", this._handleThreadState);
      this.off("thread-resumed", this._handleThreadState);
      this.emit("close");

      targets.delete(this._window);
      this._window = null;
    }

    return promise.resolve(null);
  },

  toString: function() {
    return 'WindowTarget:' + this.window;
  },
};

function WorkerTarget(workerClient) {
  EventEmitter.decorate(this);
  this._workerClient = workerClient;
}

/**
 * A WorkerTarget represents a worker. Unlike TabTarget, which can represent
 * either a local or remote tab, WorkerTarget always represents a remote worker.
 * Moreover, unlike TabTarget, which is constructed with a placeholder object
 * for remote tabs (from which a TabClient can then be lazily obtained),
 * WorkerTarget is constructed with a WorkerClient directly.
 *
 * The reason for this is that in order to get notifications when a worker
 * closes/freezes/thaws, the UI needs to attach to each worker anyway, so by
 * the time a WorkerTarget for a given worker is created, a WorkerClient for
 * that worker will already be available. Consequently, there is no need to
 * obtain a WorkerClient lazily.
 *
 * WorkerClient is designed to mimic the interface of TabClient as closely as
 * possible. This allows us to debug workers as if they were ordinary tabs,
 * requiring only minimal changes to the rest of the frontend.
 */
WorkerTarget.prototype = {
  get isRemote() {
    return true;
  },

  get isTabActor() {
    return true;
  },

  get form() {
    return {
      from: this._workerClient.actor,
      type: "attached",
      isFrozen: this._workerClient.isFrozen,
      url: this._workerClient.url
    };
  },

  get activeTab() {
    return this._workerClient;
  },

  get client() {
    return this._workerClient.client;
  },

  destroy: function () {},

  hasActor: function (name) {
    return false;
  },

  getTrait: function (name) {
    return undefined;
  },

  makeRemote: function () {}
};
