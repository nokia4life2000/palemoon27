/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * Appends a div to the document body.
 *
 * @param t  The testharness.js Test object. If provided, this will be used
 *           to register a cleanup callback to remove the div when the test
 *           finishes.
 */
function addDiv(t) {
  var div = document.createElement('div');
  document.body.appendChild(div);
  if (t && typeof t.add_cleanup === 'function') {
    t.add_cleanup(function() { div.remove(); });
  }
  return div;
}

/**
 * Some tests cause animations to continue to exist even after their target
 * element has been removed from the document tree. To ensure that these
 * animations do not impact other tests we should cancel them when the test
 * is complete.
 */
function cancelAllAnimationsOnEnd(t) {
  t.add_cleanup(function() {
    document.timeline.getAnimations().forEach(animation => animation.cancel());
  });
}

/**
 * Promise wrapper for requestAnimationFrame.
 */
function waitForFrame() {
  return new Promise(function(resolve, reject) {
    window.requestAnimationFrame(resolve);
  });
}

/**
 * Wrapper that takes a sequence of N animations and returns:
 *
 *   Promise.all([animations[0].ready, animations[1].ready, ... animations[N-1].ready]);
 */
function waitForAllAnimations(animations) {
  return Promise.all(animations.map(function(animation) {
    return animation.ready;
  }));
}

/**
 * Flush the computed style for the given element. This is useful, for example,
 * when we are testing a transition and need the initial value of a property
 * to be computed so that when we synchronouslyet set it to a different value
 * we actually get a transition instead of that being the initial value.
 */
function flushComputedStyle(elem) {
  var cs = window.getComputedStyle(elem);
  cs.marginLeft;
}

if (opener) {
  for (var funcName of ["async_test", "assert_not_equals", "assert_equals",
                        "assert_approx_equals", "assert_less_than",
                        "assert_less_than_equal", "assert_greater_than",
                        "assert_between_inclusive",
                        "assert_true", "assert_false",
                        "assert_class_string", "assert_throws",
                        "assert_unreached", "test"]) {
    window[funcName] = opener[funcName].bind(opener);
  }

  window.EventWatcher = opener.EventWatcher;

  function done() {
    opener.add_completion_callback(function() {
      self.close();
    });
    opener.done();
  }
}
