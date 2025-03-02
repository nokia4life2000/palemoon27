/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_UbiNodeCensus_h
#define js_UbiNodeCensus_h

#include "mozilla/Move.h"

#include "jsapi.h"

#include "js/UbiNode.h"
#include "js/UbiNodeBreadthFirst.h"

// A census is a ubi::Node traversal that assigns each node to one or more
// buckets, and returns a report with the size of each bucket.
//
// We summarize the results of a census with counts broken down according to
// criteria selected by the API consumer code that is requesting the census. For
// example, the following breakdown might give an interesting overview of the
// heap:
//
//   - all nodes
//     - objects
//       - objects with a specific [[Class]] *
//     - strings
//     - scripts
//     - all other Node types
//       - nodes with a specific ubi::Node::typeName *
//
// Obviously, the parts of this tree marked with * represent many separate
// counts, depending on how many distinct [[Class]] values and ubi::Node type
// names we encounter.
//
// The supported types of breakdowns are documented in
// js/src/doc/Debugger/Debugger.Memory.md.
//
// When we parse the 'breakdown' argument to takeCensus, we build a tree of
// CountType nodes. For example, for the breakdown shown in the
// Debugger.Memory.prototype.takeCensus, documentation:
//
//    {
//      by: "coarseType",
//      objects: { by: "objectClass" },
//      other:    { by: "internalType" }
//    }
//
// we would build the following tree of CountType subclasses:
//
//    ByCoarseType
//      objects: ByObjectClass
//        each class: SimpleCount
//      scripts: SimpleCount
//      strings: SimpleCount
//      other: ByUbinodeType
//        each type: SimpleCount
//
// The interior nodes are all breakdown types that categorize nodes according to
// one characteristic or another; and the leaf nodes are all SimpleType.
//
// Each CountType has its own concrete C++ type that holds the counts it
// produces. SimpleCount::Count just holds totals. ByObjectClass::Count has a
// hash table whose keys are object class names and whose values are counts of
// some other type (in the example above, SimpleCount).
//
// To keep actual count nodes small, they have no vtable. Instead, each count
// points to its CountType, which knows how to carry out all the operations we
// need on a Count. A CountType can produce new count nodes; process nodes as we
// visit them; build a JS object reporting the results; and destruct count
// nodes.


namespace JS {
namespace ubi {

struct Census;

class CountBase;

struct JS_FRIEND_API(CountDeleter) {
    void operator()(CountBase*);
};

using CountBasePtr = UniquePtr<CountBase, CountDeleter>;

// Abstract base class for CountType nodes.
struct JS_FRIEND_API(CountType) {
    explicit CountType(Census& census) : census(census) { }
    virtual ~CountType() { }

    // Destruct a count tree node that this type instance constructed.
    virtual void destructCount(CountBase& count) = 0;

    // Return a fresh node for the count tree that categorizes nodes according
    // to this type. Return a nullptr on OOM.
    virtual CountBasePtr makeCount() = 0;

    // Trace |count| and all its children, for garbage collection.
    virtual void traceCount(CountBase& count, JSTracer* trc) = 0;

    // Implement the 'count' method for counts returned by this CountType
    // instance's 'newCount' method.
    virtual bool count(CountBase& count, const Node& node) = 0;

    // Implement the 'report' method for counts returned by this CountType
    // instance's 'newCount' method.
    virtual bool report(CountBase& count, MutableHandleValue report) = 0;

  protected:
    Census& census;
};

using CountTypePtr = UniquePtr<CountType, JS::DeletePolicy<CountType>>;

// An abstract base class for count tree nodes.
class JS_FRIEND_API(CountBase) {
    // In lieu of a vtable, each CountBase points to its type, which
    // carries not only the implementations of the CountBase methods, but also
    // additional parameters for the type's behavior, as specified in the
    // breakdown argument passed to takeCensus.
    CountType& type;

  protected:
    ~CountBase() { }

  public:
    explicit CountBase(CountType& type) : type(type), total_(0) { }

    // Categorize and count |node| as appropriate for this count's type.
    bool count(const Node& node) { return type.count(*this, node); }

    // Construct a JavaScript object reporting the counts recorded in this
    // count, and store it in |report|. Return true on success, or false on
    // failure.
    bool report(MutableHandleValue report) { return type.report(*this, report); }

    // Down-cast this CountBase to its true type, based on its 'type' member,
    // and run its destructor.
    void destruct() { return type.destructCount(*this); }

    // Trace this count for garbage collection.
    void trace(JSTracer* trc) { type.traceCount(*this, trc); }

    size_t total_;
};

class RootedCount : JS::CustomAutoRooter {
    CountBasePtr count;

    void trace(JSTracer* trc) override { count->trace(trc); }

  public:
    RootedCount(JSContext* cx, CountBasePtr&& count)
        : CustomAutoRooter(cx),
          count(Move(count))
          { }
    CountBase* operator->() const { return count.get(); }
    explicit operator bool() const { return count.get(); }
    operator CountBasePtr&() { return count; }
};

// Common data for a census traversal, shared across all CountType nodes.
struct JS_FRIEND_API(Census) {
    JSContext* const cx;
    // If the targetZones set is non-empty, then only consider nodes whose zone
    // is an element of the set. If the targetZones set is empty, then nodes in
    // all zones are considered.
    JS::ZoneSet targetZones;
    Zone* atomsZone;

    explicit Census(JSContext* cx) : cx(cx), atomsZone(nullptr) { }

    bool init();

    // A 'new' work-alike that behaves like TempAllocPolicy: report OOM on this
    // census's context, but don't charge the memory allocated to our context's
    // GC pressure counters.
    template<typename T, typename... Args>
    T* new_(Args&&... args) MOZ_HEAP_ALLOCATOR {
        void* memory = js_malloc(sizeof(T));
        if (MOZ_UNLIKELY(!memory)) {
            return nullptr;
        }
        return new(memory) T(mozilla::Forward<Args>(args)...);
    }
};

// A BreadthFirst handler type that conducts a census, using a CountBase to
// categorize and count each node.
class JS_FRIEND_API(CensusHandler) {
    Census& census;
    CountBasePtr& rootCount;

  public:
    CensusHandler(Census& census, CountBasePtr& rootCount)
      : census(census),
        rootCount(rootCount)
    { }

    bool report(MutableHandleValue report) {
        return rootCount->report(report);
    }

    // This class needs to retain no per-node data.
    class NodeData { };

    bool operator() (BreadthFirst<CensusHandler>& traversal,
                     Node origin, const Edge& edge,
                     NodeData* referentData, bool first);
};

using CensusTraversal = BreadthFirst<CensusHandler>;

// Examine the census options supplied by the API consumer, and use that to
// build a CountType tree.
JS_FRIEND_API(bool) ParseCensusOptions(JSContext* cx, Census& census, HandleObject options,
                        CountTypePtr& outResult);

} // namespace ubi
} // namespace JS

#endif // js_UbiNodeCensus_h
