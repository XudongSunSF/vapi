/**
 * @file interface.i
 * @author Wells Fargo MMDC
 * @brief SWIG C++ core interface
 * @copyright 2025 Wells Fargo MMDC
 */

// provides non-ISO C Windows type support
%include <windows.i>

%begin %{
#include <src/app-common/api/WfmcmApiInternal.h>  // for Handle operator==
%}

// SWIG ownership annotations for Pando Handle factory/deleter pairs.
//
// Factory functions returning Handle create Java/C#/Python proxy objects that
// own their SWIG proxy storage. Deleter functions consume the native Pando
// resource represented by the Handle and must also clear target-language SWIG
// ownership state so later finalizers do not delete the same proxy/resource
// again.
//
// Keep this list in sync with WfmcmApi.h and WfmcmVasaraApi.h whenever a
// new Handle-returning factory or Handle-consuming destroy/delete function is
// added to the public API.
%newobject createDateSpec;
%newobject createRequest;
%newobject createHistoricalData;
%newobject createRatesData;
%newobject createMarketData;
%newobject createVolatilityBasket;
%newobject createModelSpec;
%newobject createSessionSpec;
%newobject createModelOptions;
%newobject createInstrumentPortfolio;
%newobject createPricingSpec;
%newobject createUserScenarios;
%newobject createRatePathCollection;
%newobject createTenorRatePathsMap;
%newobject createPrimaryRateOverride;
%newobject createSofrRatePaths;
%newobject createSofrRatePaths2;
%newobject createGreekSpec;
%newobject createExecOptions;
%newobject cloneHandle;
%newobject createHandleArray;
%newobject createFloatingPointMatrix;
%newobject createModelSession;
%newobject createRequestContext;
%newobject createRequestContextFromHandles;
%newobject calcBehavioralSpeedForInstrumentObj;
%newobject calcValueForInstrumentObj;

// note:
//
// We deliberately do NOT annotate deleteHandle with %delobject. %delobject is
// meant to clear the target-language proxy's ownership flag so a later
// finalizer won't re-delete the object, but it only matches when the handle is
// passed as a wrapped pointer/reference argument. Pando's deleteHandle takes a
// Handle *by value*, so %delobject does not reliably disown the proxy and gives
// a false sense of safety. Delete-idempotence is instead guaranteed by the
// isValidHandle() guard in the Handle destructor below, which is the single
// mechanism that makes explicit teardown + GC finalization safe across Java,
// C#, and Python.

// must be included *before* WfmcmApiComponents.h to allow WFMCM_API be known
// to SWIG since SWIG does *not* follow transitive includes
%include "src/app-common/api/WfmcmApiComponents.h"
%include "src/app-common/api/WfmcmApi.h"
%include "vasara_api.i"

%inline %{
static const char* swigBridgeBuildTimestamp()
{
    return __DATE__ " " __TIME__;
}
%}

%immutable Handle::internal_;

%extend Handle {
    /**
     * Custom handle "ctor".
     *
     * This replaces the default construction function that SWIG provides to
     * allow object creation in target languages.
     */
    Handle()
    {
        return new Handle(nullHandle());
    }

    /**
     * Handle "dtor".
     *
     * This replaces the default destruction function that SWIG provides to
     * allow object destruction during garbage collection.
     */
    ~Handle()
    {
        //
        // Handle destruction reaches here on two occasions:
        //
        //   1. explicit teardown from the target language -- e.g. Java keeps
        //      every created handle on a LIFO stack and calls deleteHandle for
        //      each at end of session; and
        //
        //   2. target-language garbage collection of proxy objects that were
        //      never deleted explicitly -- notably Python, which has no
        //      explicit teardown and relies entirely on this finalizer.
        //
        // Native cleanup must therefore be *idempotent*: only remove the
        // native handle if it is still live. isValidHandle() verifies that the
        // container still exists, the handle is non-null, and the entry is
        // present -- the same guard ScopedHandle uses throughout the internal
        // API. Using it here (instead of the weaker `handles && *$self !=
        // NullHandle` test) fixes two problems:
        //
        //   * Double-delete / spurious exception. When a handle is deleted
        //     explicitly (e.g. Java's deleteHandle() wrapper runs
        //     deleteHandleNative() and then Handle.delete()), this finalizer
        //     would otherwise call deleteHandle() a second time on an
        //     already-removed handle. That second call returns
        //     ApiErrorHandleNotFound and sets the per-thread error indicator,
        //     which the %exception handler then raises as an uncatchable,
        //     spurious RuntimeException. Skipping the native delete when the
        //     handle is no longer valid eliminates this.
        //
        //   * Orphaned handles after teardown. Once the library is torn down
        //     (handles == nullptr) any proxy still owned by the target-language
        //     runtime is treated as already-dead, so no error indicator is set
        //     and no ignored-finalizer-exception is reported (e.g. Python
        //     complaining about delete_Handle).
        //
        // Under Java's contract -- all handles deleted explicitly in LIFO order
        // before teardown -- every proxy is already invalid by the time GC
        // runs, so this finalizer never frees a native handle, exactly as
        // required. Under Python it is the sole cleanup path and still works.
        //
        // Known limitation: handle identity is the underlying pointer value, so
        // if an address is reused isValidHandle() can match a *different* live
        // handle (ABA). This is inherent to the current handle==pointer design
        // and is called out separately in the review; it is not introduced by
        // this guard.
        //
        if (isValidHandle(*$self)) {
            deleteHandle(*$self);
        }
        delete $self;
    }
};

%extend Date {
    Date() { Date* p = new Date{0}; return p; }
    Date(int yyyymmdd) { Date* p = new Date{(unsigned int)yyyymmdd}; return p; }
};

/* Do not remove: may be needed later
%extend StringBuf {
    StringBuf() { StringBuf* p = new StringBuf(); initStringBuf(p); return p; }
    ~StringBuf() { delete [] self->data_; delete self; }
};
*/
