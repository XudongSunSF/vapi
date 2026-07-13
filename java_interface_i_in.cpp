/**
 * @file java_interface.i
 * @author Wells Fargo MMDC
 * @brief SWIG C++ Java interface
 * @copyright 2025 Wells Fargo MMDC
 */

%module "Pando"

%{
#include <src/app-common/api/WfmcmApi.h>
#include <src/app-common/api/WfmcmApiInternal.h>  // for clearHandleError

namespace pando {
namespace {

/**
 * Set a Java exception if the API error indicator is set.
 *
 * If a Java exception is set the API error indicator is cleared to ensure that
 * other API functions that do not manipulate the error indicator will not
 * end up causing a Java exception to be thrown every time they exit.
 *
 * @param jenv JNI env instance
 *
 * @returns `true` if a Java exception is set, `false` otherwise
 */
bool propagate_error(JNIEnv* jenv)
{
  // If a Java exception is already pending -- e.g. raised by an input array
  // typemap while marshalling before $action ran -- we must not issue further
  // JNI calls that assume no exception is in flight (FindClass/ThrowNew), and
  // stacking a second exception would mask the original. Signal "exception set"
  // so the wrapper returns the null value and lets the in-flight exception
  // propagate. Still clear any native per-thread error so it cannot bleed into
  // the next API call on this thread (clearHandleError is not a JNI call and is
  // safe to invoke with a pending exception).
  if (jenv->ExceptionCheck()) {
    clearHandleError(NullHandle);
    return true;
  }

  // if there is a per-thread error, set an exception
  if (hasError()) {
    // note: the SWIG JNI functions will use "jenv" as the JNIENV* identifier
    // note: we don't check return because this is a builtin class
    auto exc_class = jenv->FindClass("java/lang/RuntimeException");
    // throw (returns <0 on failure but we can't handle that)
    jenv->ThrowNew(exc_class, getError());
    // reset error indicator for next API call
    clearHandleError(NullHandle);
    return true;
  }
  // no error
  return false;
}

}  // namespace
}  // namespace pando
%}

// {TODO}
//
// determine if unused. see https://www.swig.org/Doc4.0/Library.html#Library_nn7
//
%include "cdata.i"
%include "typemaps.i"

// for enhanced multi-argument array typemaps
%include "mustl/typemaps.i"

// ensure use of real Java enums with compile-time instead of run-time values
%include "enums.swg"
%javaconst(1);

// arrays with lengths
// note: size type doesn't exactly match, but we just copy anyways. SWIG
// requires that multi-argument typemaps have the types and names match
MUSTL_APPLY_IN_ARRAY(int, std::size_t) {
  (const int* tenors, int numTenors),
  (const int* requiredSofrSwapTenors, int numTenors),
  (const int* requiredTreasuryTenors, int numTenors),
  (const int* expiries, int numExpiries)
};
MUSTL_APPLY_IN_ARRAY(double, std::size_t) {
  (const double* pathValues, int pathLen)
};
// note: we don't %apply the default (char* STRING, size_t LENGTH) typemap
// because like the other Java array typemaps, the cleanup step will fail to
// run if we run our %exception block and exit early. so we apply our typemap
MUSTL_APPLY_IN_ARRAY(char, std::size_t) {
  (const char* content, int contentLen)
};

// define enum array typemaps
MUSTL_IN_ENUM_ARRAY_TYPEMAP(ApiDebugInfoType, std::size_t, DebugInfoType)
MUSTL_IN_ENUM_ARRAY_TYPEMAP(ApiMonthEndRollStep, std::size_t, MonthEndRollStep)
MUSTL_IN_ENUM_ARRAY_TYPEMAP(ApiPathOutputType, std::size_t, PathOutputType)
MUSTL_IN_ENUM_ARRAY_TYPEMAP(ApiPrimaryRateType, std::size_t, PrimaryRateType)
MUSTL_IN_ENUM_ARRAY_TYPEMAP(ApiSecondaryRateType, std::size_t, SecondaryRateType)
MUSTL_IN_ENUM_ARRAY_TYPEMAP(ApiWaterfallStep, std::size_t, WaterfallStep)

// strongly-typed enum arrays. again, multi-argument typemaps are only matched
// if the types and names match exactly for all the arguments
MUSTL_APPLY_IN_ENUM_ARRAY(ApiDebugInfoType, std::size_t) {
  (const ApiDebugInfoType* debugInfoTypes, int numDebugInfoTypes)
};
MUSTL_APPLY_IN_ENUM_ARRAY(ApiMonthEndRollStep, std::size_t) {
  (const ApiMonthEndRollStep* rollSteps, int numRollSteps)
};
MUSTL_APPLY_IN_ENUM_ARRAY(ApiPathOutputType, std::size_t) {
  (const ApiPathOutputType* pathOutputTypes, int numPathOutputTypes)
};
MUSTL_APPLY_IN_ENUM_ARRAY(ApiSecondaryRateType, std::size_t) {
  (const ApiSecondaryRateType* requiredSecondaryRateTypes,
    int numSecondaryRateTypes)
};
MUSTL_APPLY_IN_ENUM_ARRAY(ApiPrimaryRateType, std::size_t) {
  (const ApiPrimaryRateType* requiredPrimaryRateTypes,
    int numPrimaryRateTypes)
};
MUSTL_APPLY_IN_ENUM_ARRAY(ApiWaterfallStep, std::size_t) {
  (const ApiWaterfallStep* waterfallSteps,
    int numWaterfallSteps)
};

// load the native SWIG bridge library in the JNI wrapper class
// note: Java style is 4 spaces but SWIG always uses 2 spaces
%pragma(java) jniclasscode=%{
  static {
    try {
      System.loadLibrary("$SwigBridge");
    }
    catch (UnsatisfiedLinkError e) {
      System.err.println("Native library $SwigBridge failed to load.\n" + e);
    }
  }
%}

// remove "Api" from enums and their members
// note: until we use scoped enums, we are somewhat stuck with this yucky kind
// of MyUnscopedEnum.MyUnscopedEnum_MemberName usage in Java
%rename("%(strip:[Api])s", %$isenum) "";
%rename("%(strip:[Api])s", %$isenumitem) "";
// rename functions with "[aA]pi" in them
%rename(init) "setupApiLibrary";
%rename(teardown) "teardownApiLibrary";
// {TODO} should provide a function that gives an actual string not JSON
%rename(versionJson) "apiLibraryVersion";

%rename(deleteHandleNative) "deleteHandle";

%pragma(java) modulecode=%{
  public static int deleteHandle(Handle handle) {
    int rc = deleteHandleNative(handle);
    if (rc == 0) {
      clearHandleProxyOwnership(handle);
    }
    return rc;
  }

  private static void clearHandleProxyOwnership(Handle handle) {
    if (handle != null) {
      handle.delete();
    }
  }
%}

// Priority 3: Use a common interface on the Handle to support cross platform
// integration for consistent native memory management, such as Vasara's quant 
// sesion framework.

%typemap(javabase, notderived="1") Handle "SwigNativeAbstract"
%typemap(javaimports) Handle %{
import com.wellsfargo.vasara.jni.SwigNativeAbstract;
%}


// provide error handling for per-thread error indicator
//
// note:
//
// Java + C# don't have a SWIG_fail macro definition and therefore will skip
// the cleanup steps for things like accessing array values (for Java JNI, it's
// possible that a copy is even made). we don't need to worry about C# since
// the P/Invoke marshalling is taken care of by the C# runtime environment. but
// for Java, this is an issue, as for example, when using GetByteArrayElements()
// to convert byte[] to const char*, if we invoke this exception handler and
// return early, we will see that the corresponding ReleaseByteArrayElements
// call is *not* invoked. this leads to us holding an extra data reference to
// the byte[] data and/or even making a copy that gets lost into the nether.
//
// this is why we use the enhanced MUSTL multi-argument array typemaps, which
// not only combine a (const T*, std::size_t) pair of arguments into a single
// Java argument, but use RAII scoping to prevent this resource leak.
//
%exception {
  // can add a try-catch if we think another exceptions will be leaked.
  // generally the API functions swallow exceptions however
  $action
  // if there is a per-thread error, set an exception
  if (pando::propagate_error(jenv))
    return $null;
}

%include "interface.i"
