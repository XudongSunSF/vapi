#ifndef WFMCM_VASARA_API_H
#define WFMCM_VASARA_API_H

#include <src/app-common/api/WfmcmApiComponents.h>

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------------------------------------
 * Idempotent / distributable API
-------------------------------------------------------------------------------------------*/

/**
 * Tier 1: model parameters + behavioral model map (worker-cached).
 *
 * When a non-empty modelMapCsv is supplied, the behavioral model-map spec is
 * parsed and its native handle is created internally; it is NOT returned to the
 * caller and must NOT be tracked in the caller's handle stack. That internal
 * spec handle is owned by the returned model-config handle and is released
 * automatically when the model-config handle is passed to deleteHandle. There is
 * no separate destroy entry point for it.
 *
 * @return a model-config handle, or NullHandle on failure with the per-thread
 *         error indicator set (e.g. when the CSV or parameters fail to parse).
 */
WFMCM_API Handle WFMCM_CALLCONV createModelConfig(
    const char* modelParams,
    int modelParamsLen,
    const char* modelMapCsv,
    int modelMapCsvLen);

/**
 * Tier 2 (bytes variant): RESERVED FOR FUTURE PHASES.
 *
 * Intended for cross-JVM / Grid distribution where serialized DTO payloads
 * are shipped to worker processes. The DTO parsing layer is not yet wired,
 * so this entry point currently accepts only empty payloads
 * (historicalDataLen == 0 && marketDataLen == 0). Non-empty payloads are
 * rejected with ApiErrorOperationNotSupported to fail fast in customer
 * environments rather than producing silently incorrect results.
 *
 * Production callers MUST use createRequestContextFromHandles instead.
 */
WFMCM_API Handle WFMCM_CALLCONV createRequestContext(
    Handle modelConfig,
    const char* historicalData,
    int historicalDataLen,
    const char* marketData,
    int marketDataLen,
    Date valuationDate);

/**
 * Tier 2 using existing API handles on this worker (no legacy request handle).
 * @param marketData May be NullHandle for behavioral-only contexts.
 */
WFMCM_API Handle WFMCM_CALLCONV createRequestContextFromHandles(
    Handle modelConfig,
    Handle historicalData,
    Handle dateSpec,
    Handle modelOptions,
    Handle marketData);

/**
 * Per-instrument behavioral speed (idempotent on immutable requestContext).
 * Result pointer is thread-local; valid until the next calc on this thread or
 * until the requestContext handle is deleted (deleteHandle).
 */
WFMCM_API const char* WFMCM_CALLCONV calcBehavioralSpeedForInstrument(
    Handle requestContext,
    const char* instrumentData,
    int instrumentDataLen,
    const char* ratePaths,
    int ratePathsLen);

/**
 * Object-returning behavioral speed result. Caller owns the returned handle and
 * must release it with deleteHandle.
 *
 * The returned handle wraps the response whether it succeeded or failed, so the
 * error detail is never lost. Callers MUST check resultIsError() before reading
 * result fields, and use resultGetError() for the message on the error path.
 * Returns NullHandle only on a hard failure to produce any response (with the
 * per-thread error indicator set).
 */
WFMCM_API Handle WFMCM_CALLCONV calcBehavioralSpeedForInstrumentObj(
    Handle requestContext,
    const char* instrumentData,
    int instrumentDataLen,
    const char* ratePaths,
    int ratePathsLen);

/**
 * Full MBS pricing per instrument (idempotent on immutable requestContext).
 * Requires a context built via createRequestContextFromHandles with valid
 * historicalData, dateSpec, modelOptions, and marketData handles.
 * Result pointer is thread-local; valid until the next calc on this thread or
 * until the requestContext handle is deleted (deleteHandle).
 */
WFMCM_API const char* WFMCM_CALLCONV calcValueForInstrument(
    Handle requestContext,
    const char* instrumentData,
    int instrumentDataLen);

/**
 * Object-returning MBS pricing result. Caller owns the returned handle and must
 * release it with deleteHandle.
 *
 * The returned handle wraps the response whether it succeeded or failed, so the
 * error detail is never lost. Callers MUST check resultIsError() before reading
 * result fields, and use resultGetError() for the message on the error path.
 * Returns NullHandle only on a hard failure to produce any response (with the
 * per-thread error indicator set).
 */
WFMCM_API Handle WFMCM_CALLCONV calcValueForInstrumentObj(
    Handle requestContext,
    const char* instrumentData,
    int instrumentDataLen);

/**
 * Generates base rate-path data for a request context using the rate-generation
 * engine directly (no request/response round trip). The returned handle is a
 * CalcRatePaths result containing multiple components for the base scenario,
 * including swap/SOFR paths, UST paths, primary rates, secondary rates,
 * discount rates, and incremental primary-rate history. The market data,
 * historical data, date spec, and model options are taken from the context
 * built via createRequestContextFromHandles; the context's marketData handle
 * must carry a SOFR raw curve input, volatility input, and secondary rates,
 * and its historicalData handle must carry HPI, unemployment, and primary-rate
 * history.
 *
 * @param requestContext Context built via createRequestContextFromHandles.
 * @return a CalcRatePaths handle for the base scenario; the caller owns it and
 *         must release it with deleteHandle, or NullHandle on failure with the
 *         per-thread error indicator set.
 */
WFMCM_API Handle WFMCM_CALLCONV genRatePaths(Handle requestContext);

/**
 * Per-component JSON getters for a CalcRatePaths handle (produced by
 * genRatePaths). Each function serializes only its field; getSwapRates returns
 * until the next API string call on this thread.
 *
 * An empty string is returned in two distinct situations, distinguishable via the
 * per-thread error indicator (getError):
 *   1. Failure (uninitialized library, null/wrong-type handle, serialization
 *      error): the per-thread error indicator IS set.
 *   2. The requested optional component is legitimately absent on an otherwise
 *      valid handle (e.g. incrementalPrimaryRateHistory when no incremental
 *      history was generated): the per-thread error indicator is NOT set.
 * Callers that must tell "absent component" from "failure" apart should check the
 * error indicator after an empty return: error set => failure; error unset =>
 * absent component.
 */
WFMCM_API const char* WFMCM_CALLCONV getSwapRates(Handle ratePaths);
WFMCM_API const char* WFMCM_CALLCONV getPrimaryRates(Handle ratePaths);
WFMCM_API const char* WFMCM_CALLCONV getSecondaryRates(Handle ratePaths);
WFMCM_API const char* WFMCM_CALLCONV getDiscountRates(Handle ratePaths);
WFMCM_API const char* WFMCM_CALLCONV getIncrementalPrimaryRateHistory(Handle ratePaths);


/**
 * Prices an instrument by injecting a full precomputed set of rate paths
 * directly, with no re-projection. The rate paths (swap/SOFR/UST + discount +
 * primary + secondary, base scenario) come from a CalcRatePaths handle produced
 * by genRatePaths; the market data, historical data, date spec, model options,
 * and behavioral model map come from the request context built via
 * createRequestContextFromHandles. The instrument portfolio is parsed from the
 * supplied MBS CSV.
 *
 * @param requestContext  Context built via createRequestContextFromHandles.
 * @param ratePaths       A CalcRatePaths handle (from genRatePaths) whose paths
 *                        are injected directly as the pricing rates.
 * @param instrumentData  MBS instrument portfolio CSV bytes.
 * @param instrumentDataLen Length of instrumentData in bytes.
 * @param pricingOutputType Pricing output to compute: ApiPricingOutputType_Price
 *                        (price from OAS), ApiPricingOutputType_OAS (OAS from the
 *                        instrument's target price), or
 *                        ApiPricingOutputType_ConstantYield. For OAS / constant
 *                        yield the instrument CSV must carry the target price.
 * @param cleanPrice      true for clean (ex-accrued) price handling, false for
 *                        dirty (with accrued). For Price output this selects
 *                        which figure is produced; for OAS / constant yield it
 *                        selects how the instrument CSV target price is
 *                        interpreted. The dirty price is read back via
 *                        resultGetCleanPrice, which falls back to the dirty
 *                        price when clean is not set.
 * @return a ResultData handle the caller owns and must release with deleteHandle
 *         (query with resultIsError / resultGetCleanPrice / resultGetOas / ...),
 *         or NullHandle on failure with the per-thread error indicator set.
 */
WFMCM_API Handle WFMCM_CALLCONV calcValueForMortgageFromRates(
    Handle requestContext,
    Handle ratePaths,
    const char* instrumentData,
    int instrumentDataLen,
    ApiPricingOutputType pricingOutputType,
    bool cleanPrice);

/**
 * Non-zero when the wrapped result is an error response, zero otherwise.
 * NOTE: an invalid or wrong-type handle also returns 0 (and sets the per-thread
 * error indicator), so 0 alone does not prove a valid success result -- validate
 * the handle first if that distinction matters.
 */
WFMCM_API int WFMCM_CALLCONV resultIsError(Handle resultData);

/** Error string from wrapped response or empty string when no error exists. */
WFMCM_API const char* WFMCM_CALLCONV resultGetError(Handle resultData);

/**
 * First instrument clean price from a wrapped CalcValue response.
 * Falls back to dirty price when clean price is not set (legacy caller
 * compatibility); returns 0.0 when the response has no pricing result. Use
 * resultGetPricingResultJson to distinguish clean vs dirty explicitly.
 */
WFMCM_API double WFMCM_CALLCONV resultGetCleanPrice(Handle resultData);

/** First instrument holding from a wrapped CalcValue response. */
WFMCM_API double WFMCM_CALLCONV resultGetHolding(Handle resultData);

/** First instrument OAS from a wrapped CalcValue response. */
WFMCM_API double WFMCM_CALLCONV resultGetOas(Handle resultData);

/** First instrument id from a wrapped CalcValue response. */
WFMCM_API const char* WFMCM_CALLCONV resultGetInstrumentId(Handle resultData);

/** First instrument settle-date (YYYYMMDD int) from a wrapped CalcValue response. */
WFMCM_API int WFMCM_CALLCONV resultGetSettleDate(Handle resultData);

/**
 * Compact JSON for first pricing result fields from a wrapped CalcValue response.
 * Intended as a flexible accessor so newly added native fields are available without
 * changing Java/C API field-by-field getters.
 */
WFMCM_API const char* WFMCM_CALLCONV resultGetPricingResultJson(Handle resultData);

#ifdef __cplusplus
}
#endif

#endif