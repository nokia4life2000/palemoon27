/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

const { L10N } = require("devtools/performance/global");
const { Formatters, CollapseFunctions } = require("devtools/performance/marker-utils");

/**
 * A simple schema for mapping markers to the timeline UI. The keys correspond
 * to marker names, while the values are objects with the following format:
 *
 * - group: The row index in the timeline overview graph; multiple markers
 *          can be added on the same row. @see <overview.js/buildGraphImage>
 * - label: The label used in the waterfall to identify the marker. Can be a
 *          string or just a function that accepts the marker and returns a
 *          string, if you want to use a dynamic property for the main label.
 *          If you use a function for a label, it *must* handle the case where
 *          no marker is provided for a main label to describe all markers of
 *          this type.
 * - colorName: The label of the DevTools color used for this marker. If
 *              adding a new color, be sure to check that there's an entry
 *              for `.marker-details-bullet.{COLORNAME}` for the equivilent
 *              entry in ./browser/themes/shared/devtools/performance.inc.css
 *              https://developer.mozilla.org/en-US/docs/Tools/DevToolsColors
 * - collapseFunc: A function determining how markers are collapsed together.
 *                 Invoked with 3 arguments: the current parent marker, the
 *                 current marker and a method for peeking i markers ahead. If
 *                 nothing is returned, the marker is added as a standalone entry
 *                 in the waterfall. Otherwise, an object needs to be returned
 *                 with the following properties:
 *                 - toParent: The parent marker name (needs to be an entry in
 *                             the `TIMELINE_BLUEPRINT` itself).
 *                 - withData: An object containing some properties to staple
 *                             on the parent marker.
 *                 - forceNew: True if a new parent marker needs to be created
 *                             even though there is one currently available
 *                             with the same name.
 *                 - forceEnd: True if the current parent marker is full after
 *                             this collapse operation and should be finalized.
 * - fields: An optional array of marker properties you wish to display in the
 *           marker details view. For example, a field in the array such as
 *           { property: "aCauseName", label: "Cause" } would render a string
 *           like `Cause: ${marker.aCauseName}` in the marker details view.
 *           Each `field` item may take the following properties:
 *           - property: The property that must exist on the marker to render,
 *                       and the value of the property will be displayed.
 *           - label: The name of the property that should be displayed.
 *           - formatter: If a formatter is provided, instead of directly using
 *                        the `property` property on the marker, the marker is
 *                        passed into the formatter function to determine the
 *                        displayed value.
 *            Can also be a function that returns an object. Each key in the object
 *            will be rendered as a field, with its value rendering as the value.
 *
 * Whenever this is changed, browser_timeline_waterfall-styles.js *must* be
 * updated as well.
 */
const TIMELINE_BLUEPRINT = {
  /* Group 0 - Reflow and Rendering pipeline */
  "Styles": {
    group: 0,
    colorName: "graphs-purple",
    collapseFunc: CollapseFunctions.identical,
    label: L10N.getStr("timeline.label.styles2"),
    fields: Formatters.StylesFields,
  },
  "Reflow": {
    group: 0,
    colorName: "graphs-purple",
    collapseFunc: CollapseFunctions.identical,
    label: L10N.getStr("timeline.label.reflow2"),
  },
  "Paint": {
    group: 0,
    colorName: "graphs-green",
    collapseFunc: CollapseFunctions.identical,
    label: L10N.getStr("timeline.label.paint"),
  },
  "Composite": {
    group: 0,
    colorName: "graphs-green",
    label: L10N.getStr("marker.label.composite"),
  },
  "CompositeForwardTransaction": {
    group: 0,
    colorName: "graphs-bluegrey",
    label: L10N.getStr("marker.label.compositeForwardTransaction"),
  },

  /* Group 1 - JS */
  "DOMEvent": {
    group: 1,
    colorName: "graphs-yellow",
    collapseFunc: CollapseFunctions.DOMtoDOMJS,
    label: L10N.getStr("timeline.label.domevent"),
    fields: Formatters.DOMEventFields,
  },
  "document::DOMContentLoaded": {
    group: 1,
    colorName: "graphs-full-red",
    label: "DOMContentLoaded"
  },
  "document::Load": {
    group: 1,
    colorName: "graphs-full-blue",
    label: "Load"
  },
  "Javascript": {
    group: 1,
    colorName: "graphs-yellow",
    collapseFunc: either(CollapseFunctions.JStoDOMJS, CollapseFunctions.identical),
    label: Formatters.JSLabel,
    fields: Formatters.JSFields
  },
  "meta::DOMEvent+JS": {
    colorName: "graphs-yellow",
    label: Formatters.DOMJSLabel,
    fields: Formatters.DOMJSFields,
  },
  "Parse HTML": {
    group: 1,
    colorName: "graphs-yellow",
    collapseFunc: CollapseFunctions.identical,
    label: L10N.getStr("timeline.label.parseHTML"),
  },
  "Parse XML": {
    group: 1,
    colorName: "graphs-yellow",
    collapseFunc: CollapseFunctions.identical,
    label: L10N.getStr("timeline.label.parseXML"),
  },
  "GarbageCollection": {
    group: 1,
    colorName: "graphs-red",
    collapseFunc: CollapseFunctions.adjacent,
    label: Formatters.GCLabel,
    fields: [
      { property: "causeName", label: "Reason:" },
      { property: "nonincrementalReason", label: "Non-incremental Reason:" }
    ],
  },
  "nsCycleCollector::Collect": {
    group: 1,
    colorName: "graphs-red",
    collapseFunc: either(collapse.parent, collapse.child),
    label: "Cycle Collection",
    fields: Formatters.CycleCollectionFields,
  },
  "nsCycleCollector::ForgetSkippable": {
    group: 1,
    colorName: "graphs-red",
    collapseFunc: either(collapse.parent, collapse.child),
    label: "Cycle Collection",
    fields: Formatters.CycleCollectionFields,
  },
  "Worker": {
    group: 1,
    colorName: "graphs-orange",
    label: L10N.getStr("marker.label.worker"),
    fields: Formatters.WorkerFields
  },

  /* Group 2 - User Controlled */
  "ConsoleTime": {
    group: 2,
    colorName: "graphs-grey",
    label: sublabelForProperty(L10N.getStr("timeline.label.consoleTime"), "causeName"),
    fields: [{
      property: "causeName",
      label: L10N.getStr("timeline.markerDetail.consoleTimerName")
    }],
  },
  "TimeStamp": {
    group: 2,
    colorName: "graphs-blue",
    label: sublabelForProperty(L10N.getStr("timeline.label.timestamp"), "causeName"),
    fields: [{
      property: "causeName",
      label: "Label:"
    }],
  },
};

/**
 * Helper for creating a function that returns the first defined result from
 * a list of functions passed in as params, in order.
 * @param ...function fun
 * @return any
 */
function either(...fun) {
  return function() {
    for (let f of fun) {
      let result = f.apply(null, arguments);
      if (result !== undefined) return result;
    }
  }
}

/**
 * Takes a main label (like "Timestamp") and a property,
 * and returns a marker that will print out the property
 * value for a marker if it exists ("Timestamp (rendering)"),
 * or just the main label if it does not.
 */
function sublabelForProperty (mainLabel, prop) {
  return (marker={}) => marker[prop] ? `${mainLabel} (${marker[prop]})` : mainLabel;
}

// Exported symbols.
exports.TIMELINE_BLUEPRINT = TIMELINE_BLUEPRINT;
