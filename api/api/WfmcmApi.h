#ifndef WFMCM_API_H
#define WFMCM_API_H

#include <src/app-common/api/WfmcmApiComponents.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------------------------------------
                                      API LIBRARY INIT
-------------------------------------------------------------------------------------------*/
/**
 * @brief Initializes the WfmcmApi library.
 * @param paramDir The directory where the parameter files are located.
 * @return 0 on success, !=0 otherwise.
 * @note This must be called once when initializing the application and before any other API
 *       library calls can be made.
*/
WFMCM_API int WFMCM_CALLCONV setupApiLibrary(const char* paramDir);

/**
 * @brief Clean-up all associated resources with the WfmcmApi library.
 * @return 0 on success, !=0 otherwise.
 * @note This must be called once when closing the application. No other API
 *       library calls can be made following this.
*/
WFMCM_API int WFMCM_CALLCONV teardownApiLibrary();

/**
 * @brief Get the version.
 * @return The version string in JSON format.
*/
WFMCM_API const char* WFMCM_CALLCONV apiLibraryVersion();

/*-----------------------------------------------------------------------------------------
                                      Global Logger
-------------------------------------------------------------------------------------------*/
WFMCM_API int WFMCM_CALLCONV initGlobalLogger(const char* dirPath);


/*-----------------------------------------------------------------------------------------
                                      PARAMETER FILE VERSION RESOLVER
-------------------------------------------------------------------------------------------*/

/**
 * @brief Resolve model parameters by model version for the model spec.
 *
 * The version resolver mechanism is used to map the given major.minor.patch
 * version to the appropriate model parameter file
 *
 * @param modelSpec Model specification object handle
 * @param param Enum value indicating which type of model parameter to set
 * @param modelVersion Main version of the model parameter file
 * @param modelParamVersion Sub version of the model parameter file
 * @param histDataDir Historical data folder of the model, where the default data file located
 * @return 0 on success, !=0 otherwise
 */
WFMCM_API int WFMCM_CALLCONV resolveModelParameterVersion(
    Handle modelSpec,
    const char* modelVersion,
    const char* modelParamVersion,
    const char* histDataDir);

/**
 * @brief Resolve greek calculation specs by model version for the model spec.
 *
 * The version resolve mechanism is used to map the given major.minor.patch
 * version to the appropriate greek calculation spec file.
 *
 * @param greekSpec Greek calculation spec object handle
 * @param modelVersion Main version of the model parameter file
 * @param modelParamVersion Sub version of the specification file
 * @return 0 on success, !=0 otherwise
 */
WFMCM_API int WFMCM_CALLCONV resolveGreekSpec(
    Handle greekSpec,
    const char* modelVersion,
    const char* modelParamVersion);

/*-----------------------------------------------------------------------------------------
                                      DATES SPECIFICATION
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a date specification object. This object stores various dates associated with the request.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createDateSpec();

/**
 * @brief Set the date for the projection starting point.
 * @param dateSpec The handle associated with the date specification object.
 * @param date The date. Must be >= 1976/01/01.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setValuationDate(
    Handle dateSpec,
    Date date);

/**
 * @brief Set portfolio date (YYYYMM).
 * @param dateSpec The handle associated with the date specification object.
 * @param date The date. Must be >= 1976/01/01.
 * @note This function is optional. If not set, the value defaults to the valuation date.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setFactorDate(
    Handle dateSpec,
    Date date);

/**
 * @brief Set the market input curve date.
 * @param dateSpec The handle associated with the date specification object.
 * @param date The date. Must be >= 1976/01/01.
 * @return 0 on success, !=0 otherwise.
 * @note This function is optional. If not set, the value defaults to the valuation date.
         See `setValuationDate()`.
*/
WFMCM_API int WFMCM_CALLCONV setMarketCurveDate(
    Handle dateSpec,
    Date date);

/**
 * @brief Set the market date of the volatility input data.
 * @param dateSpec The handle associated with the date specification object.
 * @param date The date. Must be >= 1976/01/01.
 * @return 0 on success, !=0 otherwise.
 * @note This function is optional. If not set, the value defaults to the valuation date.
         See `setValuationDate()`.
*/
WFMCM_API int WFMCM_CALLCONV setMarketVolatilityInputDate(
    Handle dateSpec,
    Date date);

/**
 * @brief Set the date for latest HPI data.
 * @param dateSpec The handle associated with the date specification object.
 * @param date The date. Must be >= 1976/01/01.
 * @return 0 on success, !=0 otherwise.
 * @note This function is optional. If not set, the value defaults to the valuation date.
         See `setValuationDate()`.
*/
WFMCM_API int WFMCM_CALLCONV setHpiAsOfDate(
    Handle dateSpec,
    Date date);

/**
 * @brief Set the asOf date for primary rates.
 * @param dateSpec The handle associated with the date specification object.
 * @param date The date. Must be >= 1976/01/01.
 * @return 0 on success, !=0 otherwise.
 * @note This function is optional. If not set, the value defaults to the valuation date.
         See `setValuationDate()`.
*/
WFMCM_API int WFMCM_CALLCONV setPrimaryRateAsOfDate(
    Handle dateSpec,
    Date date);

/**
 * @brief Set the asOf date for secondary rates.
 * @param dateSpec The handle associated with the date specification object.
 * @param date The date. Must be >= 1976/01/01.
 * @return 0 on success, !=0 otherwise.
 * @note This function is optional. If not set, the value defaults to the valuation date.
         See `setValuationDate()`.
*/
WFMCM_API int WFMCM_CALLCONV setSecondaryRateAsOfDate(
    Handle dateSpec,
    Date date);

/**
 * @brief Set the date of latest historical primary rates data.
 * @param dateSpec The handle associated with the date specification object.
 * @param date The date. Must be >= 1976/01/01.
 * @return 0 on success, !=0 otherwise.
 * @note This function is optional. If not set, the value defaults to the _last business day_ of the previous month containing the valuation date.
         See `setValuationDate()`.
*/
WFMCM_API int WFMCM_CALLCONV setPrimaryRateRefDate(
    Handle dateSpec,
    Date date);

/**
 * @brief Set the date of latest historical secondary rates data.
 * @param dateSpec The handle associated with the date specification object.
 * @param date The date. Must be >= 1976/01/01.
 * @return 0 on success, !=0 otherwise.
 * @note This function is optional. If not set, the value defaults to the _last business day_ of the previous month containing the valuation date.
         See `setValuationDate()`.
*/
WFMCM_API int WFMCM_CALLCONV setSecondaryRateRefDate(
    Handle dateSpec,
    Date date);

/**
 * @brief Associate the date specification to a particular request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_GenRatePathsForMortgageValuation (single spec) <br>
        #ApiRequest_CalcValueForMortgage (single spec) <br>
        #ApiRequest_CalcGreeksFromPrices (single spec, valuation date only) <br>
        #ApiRequest_AttribValueChangeByWaterfall (single spec) <br>
        #ApiRequest_GenPrimaryMortgageRatePaths (single spec) <br>
        #ApiRequest_GenSecondaryMortgageRatePaths (single spec) <br>
        #ApiRequest_CalcValueForMonthEndRoll (single spec) <br>
        #ApiRequest_GenRatePathsForScenario (single spec, valuation date only) <br>
        #ApiRequest_GenRatePathsForWaterfallAttribution (single spec) <br>
 * @param dateSpecOrDateSpecArray A handle to a date spec object or a handle array object containing date spec handles.
        See `createDateSpec()` and `createHandleArray()`.
 * @return 0 on success, !=0 otherwise.
 * @note Requests which require multiple date specs **must** pass-in a handle array. If a request requiring
 *       a **single** date spec is passed an array, only the first date spec in the array will be considered.
 * @note For now the date spec array is reserved for future use.
*/
WFMCM_API int WFMCM_CALLCONV bindDateSpec(
    Handle request,
    Handle dateSpecOrDateSpecArray);

/*-----------------------------------------------------------------------------------------
                                       REQUESTS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a specific request using default request options.
 * @param requestType The type of request.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createRequest(
    ApiRequest requestType);

/**
 * @brief Determine which paths will be generated.
 * @param request The handle to the request object. Allowed requests are: <br>
	    #ApiRequest_GenRatePathsForMortgageValuation <br>
	    #ApiRequest_CalcValueForMonthEndRoll <br>
        #ApiRequest_GenRatePathsForScenario <br>
        #ApiRequest_GenRatePathsForWaterfallAttribution <br>
 * @param pathOutputTypes An array of #ApiPathOutputType enums.
 * @param numPathOutputTypes The size of the array.
 * @return 0 on success, !=0 otherwise.
 * @note Default values are:<br>
        #ApiPathOutputType_BaseSwapRates <br>
        #ApiPathOutputType_BasePrimaryRates <br>
        #ApiPathOutputType_SwapRates <br>
        #ApiPathOutputType_PrimaryRates <br>
*/
WFMCM_API int WFMCM_CALLCONV setOutputPaths(
    Handle request,
    const ApiPathOutputType* pathOutputTypes,
    int numPathOutputTypes);

/**
 * @brief Set the required output sofr swap tenors for this request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_GenRatePathsForMortgageValuation <br>
 * @param requiredSofrSwapTenors An array of int representing swap rate tenors.
 * @param numTenors The size of the array.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setRequiredSofrSwapTenors(
    Handle request,
    const int* requiredSofrSwapTenors,
    int numTenors);

/**
 * @brief Set the required output treasury rate tenors for this request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_GenRatePathsForMortgageValuation <br>
 * @param requiredTreasuryTenors An array of int representing treasury rate tenors.
 * @param numTenors The size of the array.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setRequiredTreasuryTenors(
    Handle request,
    const int* requiredTreasuryTenors,
    int numTenors);

/**
 * @brief Set the required primary mortgage rates for this request.
 * @param request The handle to the request object. Allowed requests are: <br>
	    #ApiRequest_GenRatePathsForMortgageValuation <br>
	    #ApiRequest_GenPrimaryMortgageRatePaths <br>
	    #ApiRequest_CalcValueForMonthEndRoll <br>
        #ApiRequest_GenRatePathsForScenario <br>
        #ApiRequest_GenRatePathsForWaterfallAttribution <br>
 * @param requiredPrimaryRateTypes An array of #ApiPrimaryRateType enums.
 * @param numPrimaryRateTypes The size of the array.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setRequiredPrimaryRates(
    Handle request,
    const ApiPrimaryRateType* requiredPrimaryRateTypes,
    int numPrimaryRateTypes);

/**
 * @brief Set the required primary and key rates for this request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_CalcBehavioralSpeedFromPrimaryRateRequest <br>
 * @param content The secondary mortgage rates as calculated by #ApiRequest_GenSecondaryMortgageRatePaths <br>
   or the path to a file containing parameter data <br>
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @param asOf The date. Must be >= 1976/01/01.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setPrimaryKeyRatesforBehavioralSpeed(
    Handle request,
    Date asOf,
    const char* content,
    int contentLen);

/**
 * @brief Set the secondary mortgage rate paths required to generate the primary mortgage rate paths.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_GenPrimaryMortgageRatePaths <br>
 * @param content The secondary mortgage rates as calculated by #ApiRequest_GenSecondaryMortgageRatePaths <br>
   or the path to a file containing parameter data <br>
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setSecondaryMortgageRatePaths(
    Handle request,
    const char* content,
    int contentLen);

/**
 * @brief Set the required secondary mortgage rates for this request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_GenSecondaryMortgageRatePaths <br>
        #ApiRequest_GenRatePathsForMortgageValuation <br>
 * @param requiredSecondaryRateTypes An array of #ApiSecondaryRateType enums.
 * @param numSecondaryRateTypes The size of the array.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setRequiredSecondaryRates(
    Handle request,
    const ApiSecondaryRateType* requiredSecondaryRateTypes,
    int numSecondaryRateTypes);

/**
 * @brief Set the simulation months for this request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_GenRatePathsForMortgageValuation <br>
	    #ApiRequest_CalcValueForMonthEndRoll <br>
        #ApiRequest_GenRatePathsForScenario <br>
        #ApiRequest_GenRatePathsForWaterfallAttribution <br>
 * @param simMonths Simulation months, default is 360 if not set
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setSimulationMonths(
    Handle request,
    int simMonths);

/**
 * @brief Set DebugInfo for this request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_CalcValueForMortgage <br>
 * @param debugInfoTypes An array of #ApiDebugInfoType enums.
 * @param numDebugInfoTypes The size of the array.
 * @return 0 on success, !=0 otherwise.
*/

WFMCM_API int WFMCM_CALLCONV setValueMortgageRequestDebugInfo(
    Handle request,
    const ApiDebugInfoType* debugInfoTypes,
    int numDebugInfoTypes);

/**
 * @brief Set DebugInfo for this request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_CalcValueForMortgageFromRatesRequest <br>
 * @param debugInfoTypes An array of #ApiDebugInfoType enums.
 * @param numDebugInfoTypes The size of the array.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setValueMortgageFromRatesRequestDebugInfo(
    Handle request,
    const ApiDebugInfoType* debugInfoTypes,
    int numDebugInfoTypes);

/**
 * @brief Set the index rate mapping that redirects an index rate to another type. Needed when a behavior model's required index is not available.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_CalcValueForMortgage <br>
        #ApiRequest_CalcValueForMortgageFromRatesRequest <br>
 * @param content The user-defined index rate mapping CSV file
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setIndexRateMapping(
    Handle request,
    const char* content,
    int contentLen);

/*-----------------------------------------------------------------------------------------
                                 VALUE MORTGAGE REQUEST
-------------------------------------------------------------------------------------------*/
#if 0
WFMCM_API int WFMCM_CALLCONV setValueMortgageRequestRecalibratedBasis(
    Handle request,
    bool value);
WFMCM_API int WFMCM_CALLCONV setValueMortgageRequestUseBaseOAS(
    Handle request,
    bool value);
#endif

/*-----------------------------------------------------------------------------------------
                                    HISTORICAL DATA
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a historical data object.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createHistoricalData();

/**
 * @brief Set the historical data.
 * @param hist The handle to a historical data object. See `createHistoricalData()`.
 * @param dataType The type of data to set.
 * @param content The historical data content or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
 * @note This function may be called multiple times in order to accumulate historical data from multiple sources.
 *       In case of conflict, the existing data takes precedence so call order is important.
 * @note This function is meant to replace the older setHistoricalXXX() api.
*/
WFMCM_API int WFMCM_CALLCONV setHistoricalData(
    Handle hist,
    ApiHistoricalData dataType,
    const char* content,
    int contentLen);

/**
 * @brief Associate historical data with a particular request.
 * @param request A handle to a request object. Allowed requests are: <br>
        #ApiRequest_GenRatePathsForMortgageValuation <br>
        #ApiRequest_CalcValueForMortgage <br>
        #ApiRequest_AttribValueChangeByWaterfall <br>
        #ApiRequest_GenSecondaryMortgageRatePaths <br>
        #ApiRequest_CalcValueForMonthEndRoll <br>
        #ApiRequest_GenRatePathsForScenario <br>
        #ApiRequest_GenRatePathsForWaterfallAttribution <br>
 * @param historicalData A handle to a historical data object. See `createHistoricalData()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV bindHistoricalData(
    Handle request,
    Handle historicalData);

/*-----------------------------------------------------------------------------------------
                                      MARKET DATA
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a market data object.
 * @param marketDate The desired market date.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createMarketData(Date marketDate);

/**
 * @brief Set the raw curve input to be used for a request.
 * @param market A handle to a market data object. See `createMarketData()`.
 * @param dateSpec The handle associated with the date specification object.
 * @param inputType The format convention for the curve input.
 * @param content The raw curve data or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setMarketRawCurveInput(
    Handle market,
    Handle dateSpec,
    ApiCurveInputFormat inputType,
    const char* content,
    int contentLen);

/**
 * @brief Set the treasury curve input to be used for a request.
 * @param market A handle to a market data object. See `createMarketData()`.
 * @param dateSpec The handle associated with the date specification object.
 * @param inputType The format convention for the US Treasury curve input.
 * @param content The US Treasury curve data or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setMarketUSTreasuryCurveInput(
    Handle market,
    Handle dateSpec,
    ApiCurveInputFormat inputType,
    const char* content,
    int contentLen);

/**
 * @brief Set the swaption volatility input.
 * @param market A handle to a market data object. See `createMarketData()`.
 * @param dateSpec The handle associated with the date specification object.
 * @param atTheMoney Set to true if moneyness should be 0. 
 * @param content The swaption volatility data or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setMarketSwaptionVolatilityInput(
    Handle market,
    Handle dateSpec,
    bool atTheMoney,
    const char* content,
    int contentLen);

/**
 * @brief Set secondary mortgage rates.
 * @param market A handle to a market data object. See `createMarketData()`.
 * @param content The mortgage rate data or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setMarketSecondaryRates(
    Handle market,
    const char* content,
    int contentLen);

/**
 * @brief Set primary mortgage rates.
 * @param market A handle to a market data object. See `createMarketData()`.
 * @param content The primary rate data or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setMarketPrimaryRates(
    Handle market,
    const char* content,
    int contentLen);

/**
 * @brief Associate market data with a particular request.
 * @param request A handle to a request object. Allowed requests are: <br>
        #ApiRequest_GenRatePathsForMortgageValuation (single market) <br>
        #ApiRequest_CalcValueForMortgage (single market) <br>
        #ApiRequest_GenPrimaryMortgageRatePaths (single market) <br>
        #ApiRequest_GenSecondaryMortgageRatePaths (single market) <br>
        #ApiRequest_AttribValueChangeByWaterfall (two markets: start + end) <br>
        #ApiRequest_CalcValueForMonthEndRoll (two markets: start + end) <br>
        #ApiRequest_GenRatePathsForScenario (single market) <br>
        #ApiRequest_GenRatePathsForWaterfallAttribution (two markets: start + end) <br>
 * @param marketDataOrMarketDataArray A handle to a market data object or a handle array object containing market data handles.
          See `createMarketData()` and `createHandleArray()`.
 * @return 0 on success, !=0 otherwise.
 * @note Requests which require more than a single market data **must** pass-in a handle array. If a request requiring
 *       a **single** market data is passed an array, only the first market data in the array will be considered.
*/
WFMCM_API int WFMCM_CALLCONV bindMarketData(
    Handle request,
    Handle marketDataOrMarketDataArray);

/*-----------------------------------------------------------------------------------------
                                      VOLATILITY BASKET
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a volatility basket object.
 * @param tenors Tenors (in months) of swaptions used to calibrate the interest rate market model.
 *        Default values are [ 12, 24, 36, 60, 120, 240 ].
 * @param numTenors The number of tenors in the array.
 * @param expiries Expiries (in months) of swaptions used to calibrate the interest rate market model.
 *        Default values are [ 3, 6, 12, 24, 36, 60, 120 ].
 * @param numExpiries The number of expiries in the array.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createVolatilityBasket(
    const int* tenors,
    int numTenors,
    const int* expiries,
    int numExpiries);

/**
 * @brief Associate volatility basket with a particular request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_CalcGreeksFromPrices <br>
 * @param volBasket The handle to a volatility basket object. See `createVolatilityBasket()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV bindVolatilityBasket(
    Handle request,
    Handle volBasket);
/*-----------------------------------------------------------------------------------------
                                      MODELS & SESSIONS SPECS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a model specification. The model specification stores various configuration
 *        options which are specific to a model.
 * @param modelType The type of model.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation. The model specification must be added to a
 *       model options object.
*/
WFMCM_API Handle WFMCM_CALLCONV createModelSpec(ApiModel modelType);

/**
 * @brief Add a configurtion parameter to a model specification.
 * @param modelSpec A handle to a model specification object. See `createModelSpec()`.
 * @param param The parameter type. If a model does not support a particular option, an error will be returned.
 *        To find out the available parameters per model, use `getSupportedModelParameters()`.
 * @param content The parameter data or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setModelParameter(
    Handle modelSpec,
    ApiModelParameter param,
    const char* content,
    int contentLen);

/**
 * @brief Create a model session specification. The session specification stores various configuration
 *        options which are specific to a model's session.
 * @param modelType The type of model. All types are allowed except #ApiModel_CurveBuilder.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation. The session specification must be bound via `bindModelSession()`
 *       to a specific model in order to be used.
*/
WFMCM_API Handle WFMCM_CALLCONV createSessionSpec(ApiModel modelType);

/**
 * @brief Add a configurtion parameter to a session specification.
 * @param sessionSpec A handle to a session specification object. See `createSessionSpec()`.
 * @param param The parameter type. If a session does not support a particular option, an error will be returned.
 *        To find out the available parameters per session type, use `getSupportedSessionParameters()`.
 * @param content The parameter data or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setSessionParameter(
    Handle sessionSpec,
    ApiSessionParameter param,
    const char* content,
    int contentLen);

/**
 * @brief Associate a session to a model.
 * @param modelSpec The handle to a model spec object. See `createModelSpec()`.
 * @param sessionSpec The handle to a session spec object. See `createSessionSpec()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV bindModelSession(
    Handle modelSpec,
    Handle sessionSpec);

/**
 * @brief Return the list of supported specification parameters for this model.
 * @param modelType The model type.
 * @return The list in JSON format.
*/
WFMCM_API const char* WFMCM_CALLCONV getSupportedModelParameters(ApiModel modelType);

/**
 * @brief Return the list of supported specification parameters for this session.
 * @param modelType The model type to which the session belongs to.
 * @return The list in JSON format.
*/
WFMCM_API const char* WFMCM_CALLCONV getSupportedSessionParameters(ApiModel modelType);

/*-----------------------------------------------------------------------------------------
                                      MODELS OPTIONS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a model option object. The model options contains all the model (and bound session)
 *        specs which will be used for a particular request.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createModelOptions();

/**
 * @brief Adds a model specification to the model options object.
 * @param modelOptions The handle to a model options object. See `createModelOptions()`.
 * @param modelSpec The handle to a model specification object. See `createModelSpec()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV addModelOptionsSpec(
    Handle modelOptions,
    Handle modelSpec);

/**
 * @brief Associate model options with a particular request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_GenRatePathsForMortgageValuation <br>
        #ApiRequest_CalcValueForMortgage <br>
        #ApiRequest_CalcGreeksFromPrices <br>
        #ApiRequest_AttribValueChangeByWaterfall <br>
        #ApiRequest_GenPrimaryMortgageRatePaths <br>
        #ApiRequest_GenSecondaryMortgageRatePaths <br>
        #ApiRequest_CalcValueForMonthEndRoll <br>
        #ApiRequest_GenRatePathsForScenario <br>
        #ApiRequest_GenRatePathsForWaterfallAttribution <br>
 * @param modelOptions The handle to a model options object. See `createModelOptions()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV bindModelOptions(
    Handle request,
    Handle modelOptions);

/*-----------------------------------------------------------------------------------------
                                      PORTFOLIOS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a portfolio.
 * @param portfolioType The type of the portfolio.
 * @param content The portfolio data or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createInstrumentPortfolio(
    ApiPortfolioType portfolioType,
    const char* content,
    int contentLen);

/**
 * @brief Map Behavioral Model.
 * @param portfolio The portfolio handle.
 * @param factorDate The factor date of the portfolio.
 * @param content The Behavioral Model Spec or the path to a file containing parameter data or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV mapBehavioralModel(
    Handle portfolio,
    Handle modelSpec,
    Date factorDate);

/**
 * @brief Associate a portfolio with a specific request.
 * @param request The request object. Allowed requests are: <br>
	    #ApiRequest_CalcValueForMortgage <br>
	    #ApiRequest_AttribValueChangeByWaterfall <br>
 * @param portfolio An mbs or msr portfolio. See `createInstrumentPortfolio()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV bindInstrumentPortfolio(
    Handle request,
    Handle portfolio);

/*-----------------------------------------------------------------------------------------
                                      PRICING SPEC
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a pricing spec.
 * @param pricingOutputType The type of the pricing output.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createPricingSpec(
    ApiPricingOutputType pricingOutputType);

 /**
 * @brief Sets the base OAS flag a specific request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_CalcValueForMortgage <br>
        #ApiRequest_AttribValueChangeByWaterfall <br>
 * @param useBaseOAS is the flag for using base OAS. OAS is turned off by default.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setBaseOASFlag(
    Handle request,
    bool useBaseOAS);

/**
 * @brief Sets the base OAS flag a specific request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_CalcValueForMortgage <br>
        #ApiRequest_AttribValueChangeByWaterfall <br>
 * @param isCleanPrice is the flag for using clean price. Clean price is enabled by default.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setCleanPriceFlag(
    Handle request,
    bool isCleanPrice);

/**
 * @brief Associate a pricing spec with a specific request.
 * @param request The request object. Allowed requests are: <br>
        #ApiRequest_CalcValueForMortgage <br>
        #ApiRequest_AttribValueChangeByWaterfall <br>
 * @param pricingSpec A pricingSpec. See `createPricingSpec()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV bindPricingSpec(
    Handle request,
    Handle pricingSpec);


/*-----------------------------------------------------------------------------------------
                                   USER SCENARIOS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create user scenarios for valuation.
 * @param content The scenario data or the path to a file containing parameter data or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createUserScenarios(
    const char* content,
    int contentLen);

/**
 * @brief Associate user scenarios to a particular request.
 * @param request The handle to the request object. Allowed requests are: <br>
	    #ApiRequest_GenRatePathsForMortgageValuation,
	    #ApiRequest_CalcValueForMortgage,
	    #ApiRequest_CalcValueForMonthEndRoll
 * @param userScenarios The handle to a valuation scenarios object. See `createUserScenarios()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV bindUserScenarios(
    Handle request,
    Handle userScenarios);

/*-----------------------------------------------------------------------------------------
                                    RATE PATH COLLECTION
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a collection of rate paths.
 * @param numPaths The number of paths in the collection e.g. 256.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createRatePathCollection(int numPaths);

/**
 * @brief Add a path to the collection.
 * @param ratePathCollection A handle to a rate path collection object. See `createRatePathCollection()`.
 * @param pathValues An array of doubles representing path values.
 * @param pathLen The length of this path. Note that all paths in a collection must have the same length.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV ratePathCollectionAdd(
    Handle ratePathCollection,
    const double* pathValues,
    int pathLen);

/*-----------------------------------------------------------------------------------------
                                   TENOR RATE PATHS MAP
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create an associative container which maps tenors to rate paths.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createTenorRatePathsMap();

/**
 * @brief Add a rate path collection to a specific tenor.
 * @param tenorRatePathsMap A handle representing a tenor to rate paths map. See `createTenorRatePathsMap()`.
 * @param tenor The tenor in months. This is the index into the map.
 * @param ratePathCollection A handle to the rate paths collection object. See `createRatePathCollection()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setRatePathsForTenor(
    Handle tenorRatePathsMap,
    int tenor,
    Handle ratePathCollection);

/*-----------------------------------------------------------------------------------------
                                 User Override Primary Rate Inputs at T0
-------------------------------------------------------------------------------------------*/

/**
 * @brief Create a map that contains user-defined specified T0 primary rates.
 * @param content The filepath or content of user-defined T0 primary rates csv file.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createPrimaryRateOverride(
    const char* content,
    int contentLen);

/**
 * @brief Associate user-defined T0 primary rate inputs with a particular request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_GenRatePathsForMortgageValuation <br>
        #ApiRequest_CalcValueForMortgage <br>
 * @param primaryRatesOverride The handle to a primary rate input map object.
 *        See `createPrimaryRateOverride()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV bindPrimaryRateOverride(
    Handle request, Handle primaryRatesOverride);



/*-----------------------------------------------------------------------------------------
                                   SOFR RATE PATHS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a SOFR rate paths object.
 * @param content The SOFR paths or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createSofrRatePaths(
    const char* content,
    int contentLen);

/**
 * @brief Similar to `createSofrRatePaths()`, this function creates a rate paths object
 *        by specifying individual cash and swap rate maps.
 * @param cashTenorToRatePathsMap (optional) A handle to a cash rates map (1m rate). Used for pricing. See `createTenorRatePathsMap()`.
 * @param swapTenorToRatePathsMap A handle to a swap rates map. See `createTenorRatePathsMap()`.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
 * @note Use `nullHandle()` if optional argument is not used.
*/
WFMCM_API Handle WFMCM_CALLCONV createSofrRatePaths2(
    Handle cashTenorToRatePathsMap,
    Handle swapTenorToRatePathsMap);

/**
 * @brief Provide user-defined interest rate SOFR paths.
 * @param request The handle to the request object. Allowed requests are: <br>
	    #ApiRequest_GenRatePathsForMortgageValuation <br>
	    #ApiRequest_CalcValueForMortgage <br>
	    #ApiRequest_AttribValueChangeByWaterfall <br>
	    #ApiRequest_GenSecondaryMortgageRatePaths <br>
	    #ApiRequest_CalcValueForMonthEndRoll <br>
        #ApiRequest_GenRatePathsForWaterfallAttribution <br>
 * @param sofrRatePaths The handle to a SOFR rates object.
 *        See `createSofrRatePaths()` or `createSofrRatePaths2()`.
 * @return 0 on success, !=0 otherwise.
 * @note Calling `bindSofrRatePaths()` and creating an interest rate model/session spec(s) are mutually exclusive.
 *        Only one should be provided.
*/
WFMCM_API int WFMCM_CALLCONV bindSofrRatePaths(
    Handle request,
    Handle sofrRatePaths);

/*-----------------------------------------------------------------------------------------
                                   CALC RATE PATHS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a fully precomputed rate paths object.
 * @param content Either a #ApiRequest_GenRatePathsForMortgageValuation response
 *        envelope (JSON), or a bare `CalcRatePaths` object (JSON) supplied directly.
 *        The input shape is detected automatically. May also be the JSON content itself.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return A valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createCalcRatePaths(
    const char* content,
    int contentLen);

/**
 * @brief Provide a fully precomputed rate-path set to price against (no projection).
 * @param request The handle to the request object. Allowed requests are: <br>
 *      #ApiRequest_CalcValueForMortgageFromRates <br>
 * @param ratePaths The handle to a calc rate paths object. See `createCalcRatePaths()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV bindCalcRatePaths(
    Handle request,
    Handle ratePaths);

/*-----------------------------------------------------------------------------------------
                              Curve Modification Spec
-------------------------------------------------------------------------------------------*/
/**
 * @brief Set how to shift the curve when valuation date and market data date dont match.
 * @param curveModSpec The handle to a curve modification spec. See `createModelSpec()`.
 * @param shift The shift type. Default is #ApiCurveDateShift_KeepSwapRateSame
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setCurveDateShift(
    Handle curveModSpec,
    ApiCurveDateShift shift);

/**
 * @brief Set the swap tenors required by the interest rate proxy curve.
 * @param curveModSpec The handle to curve modification spec. See `createModelSpec()`.
 * @param tenors A tenor array. Tenors are expressed in months.
 * @param numTenors The number of tenors in the array.
 * @return 0 on success, !=0 otherwise.
 * @note Default values are [ 1, 3, 6, 9, 12, 24, 36, 48, 60, 84, 120, 144, 180, 240, 300, 360, 480, 600 ]
*/
WFMCM_API int WFMCM_CALLCONV setProxyCurveDefinition(
    Handle curveModSpec,
    const int* tenors,
    int numTenors);

/*-----------------------------------------------------------------------------------------
                              Curve Interpolation Spec
-------------------------------------------------------------------------------------------*/
/**
 * @brief Set the interpolation method of Sofr curve.
 * @param curveInterpSpec The handle to a curve interpolation spec. See `createModelSpec()`.
 * @param interp The interpolation method type.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setSofrCurveInterpolationMethod(
    Handle curveInterpSpec,
    ApiInterpolationMethod interp);

/**
 * @brief Set the interpolation method of US Treasury curve.
 * @param curveInterpSpec The handle to a curve interpolation spec. See `createModelSpec()`.
 * @param interp The interpolation method type.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setUstCurveInterpolationMethod(
    Handle curveInterpSpec,
    ApiInterpolationMethod interp);


/*-----------------------------------------------------------------------------------------
                                      GREEK SPEC
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a Greek specification object
 * @param content The Greek specification or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createGreekSpec(
    const char* content,
    int contentLen);

/**
 * @brief Associate the greek specification with a particular request.
 * @param request The handle to the request object. Allowed requests are: <br>
	    #ApiRequest_GenRatePathsForMortgageValuation <br>
	    #ApiRequest_CalcValueForMortgage <br>
	    #ApiRequest_CalcGreeksFromPrices <br>
        #ApiRequest_GenRatePathsForScenario <br>
 * @param greekSpec The handle to a greek specification object. See `createGreekSpec()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV bindGreekSpec(
    Handle request,
    Handle greekSpec);

/*-----------------------------------------------------------------------------------------
                                WATERFALL ATTRIBUTION REQUEST
-------------------------------------------------------------------------------------------*/
/**
 * @brief Set the steps used for the waterfall attribution request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_AttribValueChangeByWaterfall <br>
        #ApiRequest_GenRatePathsForWaterfallAttribution <br>
 * @param waterfallSteps An series of steps.
 * @param numWaterfallSteps The number of steps in the series.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setWaterfallAttributionSteps(
    Handle request,
    const ApiWaterfallStep* waterfallSteps,
    int numWaterfallSteps);

/**
 * @brief Set the risk factor derivative calculation.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_AttribValueChangeByWaterfall <br>
        #ApiRequest_GenRatePathsForWaterfallAttribution <br>
 * @param derivType Partial or Total. See #ApiDerivativeCalculation for details.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setRiskFactorDerivativeCalculation(
    Handle request,
    ApiDerivativeCalculation derivType);

/*-----------------------------------------------------------------------------------------
                                MONTH END ROLL REQUEST
-------------------------------------------------------------------------------------------*/
/**
 * @brief Set the steps used for the month end roll request.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_CalcValueForMonthEndRoll <br>
 * @param rollSteps An series of steps.
 * @param numRollSteps The number of steps in the series.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setMonthEndRollSteps(
    Handle request,
    const ApiMonthEndRollStep* rollSteps,
    int numRollSteps);

/*-----------------------------------------------------------------------------------------
                                  CALCUALTE GREEKS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Set the scenario price inputs for which Greeks will be calculated.
 * @param request The handle to the request object. Allowed requests are: <br>
        #ApiRequest_CalcGreeksFromPrices <br>
 * @param content The prices or the path to a file containing parameter data.
 * @param contentLen The length of the `content` string. May set to `-1` if the content is null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setGreekScenarioPrices(
    Handle request,
    const char* content,
    int contentLen);

/*-----------------------------------------------------------------------------------------
                                    EXECUTION OPTIONS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create request execution options.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createExecOptions();

/**
 * @brief If set, the request will not run to completion and will only validate inputs.
 * @param execOptions A handle to a request options object. See `createRequestOptions()`.
 * @param value The value to set. Default False.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setInputValidationOnly(
    Handle execOptions,
    bool value);

/**
 * @brief Allows certain models to execute serially or in parallel (if applicable).
 * @param execOptions A handle to a request options object. See `createRequestOptions()`.
 * @param mode The execution mode to run in. Default #ApiExecMode_Parallel.
 * @return 0 on success, !=0 otherwise.
 * @warning Setting this to ApiExecMode_Serial may considerably degrade performance.
            This flag should be used for debugging purposes only.
*/
WFMCM_API int WFMCM_CALLCONV setExecutionMode(
    Handle execOptions,
    ApiExecMode mode);

/**
 * @brief Set the executor type to use for processing the request.
 * @param execOptions A handle to a request options object. See `createExecOptions()`.
 * @param execType The executor type to use. Default #ApiExecutor_Threadpool.
 * @return 0 on success, !=0 otherwise.
 */
WFMCM_API int WFMCM_CALLCONV setExecutorType(
    Handle execOptions,
    ApiExecutorType execType);

/**
 * @brief Set the precision for floating point data returned in the result.
 * @param execOptions A handle to a request options object. See `createRequestOptions()`.
 * @param precision The the number of significant decimal digits. Allowed values are in the range [0,16]. Default is 16.
 * @return 0 on success, !=0 otherwise.
 * @note Lowering the precision significantly reduces the output size.
*/
WFMCM_API int WFMCM_CALLCONV setFloatingPointPrecision(
    Handle execOptions,
    int precision);

/**
 * @brief If enabled, results will be returned in multiple smaller slices (chunks)
          when calling `getResult()`.
 * @param execOptions A handle to a request options object. See `createRequestOptions()`.
 * @return 0 on success, !=0 otherwise.
 * @note  Each slice contain exactly one scenario. In case an instrument portfolio was used in the request,
          each slice contain 100 instruments (or less) for a particular scenario. Therefore
          scenarios with more than 100 instruments may be further divided.
          Calling `getResult()` repeatedly along with `hasResult()` will indicate if there's any slices left.
          If the response contains error(s), they can be retrieved separately via `getResultError()`,
          however result errors are not sliced and will be returned as a single message.
 * @note Each slice represents exactly the same JSON type as the original response. The response header
 *       shall contain two fields `fragmentId` and `totalFragments` which help the application re-order
 *       the slices in the case the response is read from multiple threads using `getResult()`.
 *       `framgmentId` is bounded by the range [0, `totalFragments`)
 * @warning This should be used when a very large result set is expected and it will prevent allocating
 *          large memory buffers to pass data back to the application. It is up to the caller
 *          to recompose the full result from the multiple slices returned if needed.
*/
WFMCM_API int WFMCM_CALLCONV enableResponseSlicing(
    Handle execOptions);

// {NOTE} setErrorBehavior is intentionally disabled (#if 0) because enabling it
// would change the error-handling contract for ALL existing API consumers
// (C#, Java, Python).  The legacy handlers (e.g. CalcBehavioralSpeedFromPrimaryRate-
// RequestHandler) were only tested with EarlyTermination semantics.  Exposing
// RunToCompletion through the public C API requires auditing every handler first.
// The new model-suite architecture (useNewBehavioralArchitecture) handles
// RunToCompletion internally via WorkflowRequest::execControl_.errorBehavior_
// and does NOT rely on this C API surface.
#if 0
/**
 * @brief Set how the calculation engine should behave should an error arise.
 * @param execOptions A handle to a request options object. See `createRequestOptions()`.
 * @param behavior The behavior for errors. Default is #ApiErrorBehavior_EarlyTermination.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setErrorBehavior(
    Handle execOptions,
    ApiErrorBehavior behavior);
#endif

/**
 * @brief Optionally return the stack trace where the errors occured.
 * @param execOptions A handle to a request options object. See `createRequestOptions()`.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV includeStackTrace(
    Handle execOptions);

/**
 * @brief Enable the new model-suite behavioral architecture for execution.
 * When enabled, CalcBehavioralSpeedFromPrimaryRate requests will be routed
 * through the ModelSuiteLayer/WorkflowOrchestrator instead of the legacy
 * CalcBehavioralSpeedFromPrimaryRateRequestHandler.
 * @param execOptions A handle to execution options. See `createExecOptions()`.
 * @param value true to enable, false to use legacy handler.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setUseNewBehavioralArchitecture(
    Handle execOptions,
    bool value);

/**
 * @brief Set the suite registry configuration file path for the new behavioral architecture.
 * Required when `setUseNewBehavioralArchitecture` is enabled.
 * @param execOptions A handle to execution options. See `createExecOptions()`.
 * @param path Path to the suite registry JSON configuration file.
 * @param pathLen Length of the path string. May set to -1 if null-terminated.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setSuiteRegistryConfigFile(
    Handle execOptions,
    const char* path,
    int pathLen);

/*-----------------------------------------------------------------------------------------
                                      REQUEST EXECUTION
-------------------------------------------------------------------------------------------*/
/**
 * @brief Execute a request using default execution options.
 * @param request A handle to a request object.
 * @return 0 on success, !=0 otherwise.
 * @note This function returns immediately upon sending the request. User must call
 *       `getResult()` or `getResultWithTimeout()` to wait and receive the response.
*/
WFMCM_API int WFMCM_CALLCONV execute(
    Handle request);

/**
 * @brief Execute a request using user-supplied options.
 * @param request A handle to a request object. All request types are permitted. See `createRequest()`.
 * @param execOptions A handle to execution optons. See `createExecOptions()`.
 * @return 0 on success, !=0 otherwise.
 * @note This function returns immediately upon sending the request. User must call
 *       `getResult()` or `getResultWithTimeout()` to wait and receive the response.
*/
WFMCM_API int WFMCM_CALLCONV execute2(
    Handle request,
    Handle execOptions);

/**
 * @brief Waits indefinitely for the response to arrive. If the response has already arrived,
 *        or if the response has already been processed via calls to `getResult()` or `getResultError()`,
 *        this function returns immediately.
 * @param request A handle to a request object.
 * @return 0 on success, !=0 otherwise.
 * @note If the application uses multiple threads to call `getResult()`, it is recommended to
 *       call this function and wait for the response to arrive before dispatching the reader
 *       threads.
*/
WFMCM_API int WFMCM_CALLCONV waitForResult(
    Handle request);

/**
 * @brief Similar to `waitForResult()`, this function waits for the result with a time limit.
 * @param request A handle to a request object.
 * @param timeoutSec The max time to wait in seconds. Must be 0 or greater.
 *                   If timeout expires before data arrives, the function returns non-zero and
                     `getHandleError()` will indicate `ApiErrorTimeout`.
                     Passing -1 is equivalent to calling `waitForResult()`.
 * @return 0 on success, !=0 otherwise.
 * @note If the application uses multiple threads to call `getResult()`, it is recommended to
 *       call this function and wait for the response to arrive before dispatching the reader
 *       threads.
*/
WFMCM_API int WFMCM_CALLCONV waitForResultWithTimeout(
    Handle request,
    int timeoutSec);

/**
 * @brief Get the (partial) result of a request.
 * @param request A handle to a request object.
 * @return The response in JSON format or an empty string if there's no more data.
 * @note This function blocks until there is data by calling `waitForResult()`.
 *       It may be called multiple times as long as `hasResult()` returns true.
 * @note This function may be called from multiple threads. The application needs to
 *       check the validity of the return string (i.e. null), even if `hasResult()` returns true
 *       prior to this call. It is recommended to call `waitForResult()` from main,
 *       prior to dispatching multiple reader threads.
*/
WFMCM_API const char* WFMCM_CALLCONV getResult(Handle request);

/**
 * @brief Similar to `getResult()`, this function waits for the result with a time limit.
 * @param request A handle to a request object.
 * @param timeoutSec The max time to wait in seconds. Must be 0 or greater.
 *                   If timeout expires before data arrives, the function returns null string and
                     `getHandleError()` will indicate `ApiErrorTimeout`.
                     Passing -1 is equivalent to calling `getResult()`.
 * @return The response in JSON format or an empty string if there's no more data.
 * @note This function blocks by calling `waitForResultWithTimeout()` internally.
 * @note This function may be called from multiple threads. The application needs to
 *       check the validity of the return string (i.e. null), even if `hasResult()` returns true
 *       prior to this call. It is recommended to call `waitForResult()` from main,
 *       prior to dispatching multiple reader threads.
*/
WFMCM_API const char* WFMCM_CALLCONV getResultWithTimeout(
    Handle request,
    int timeoutSec);

/**
 * @brief Indicates if response is pending or if it has arrived and there's
 *        still data to be fetched by the application.
 * @param request A handle to a request object.
 * @return `ApiResultStatus`
 * @note The application may call `getResult()` if this function returns true.
*/
WFMCM_API int WFMCM_CALLCONV hasResult(Handle request);

/**
 * @brief Get the error of a response if `hasResultError()` returns true.
 * @param request A handle to a request object.
 * @return The error in JSON format or an empty string if there's no errors.
 * @note This function blocks until there is data by calling `waitForResult()`.
 * @note Setting `enableResponseSlicing()` has no effect on this function.
 *       All errors are returned in a single message.
*/
WFMCM_API const char* WFMCM_CALLCONV getResultError(Handle request);

/**
 * @brief Similar to `getResultError()`, this function waits for the result with a time limit.
 * @param request A handle to a request object.
 * @param timeoutSec The max time to wait in seconds. Must be 0 or greater.
 *                   If timeout expires before data arrives, the function returns null string and
                     `getHandleError()` will indicate `ApiErrorTimeout`.
                     Passing -1 is equivalent to calling `getResult()`.
 * @return The error in JSON format or an empty string if there's no errors.
 * @note This function blocks by calling `waitForResultWithTimeout()` internally.
 * @note Setting `enableResponseSlicing()` has no effect on this function.
 *       All errors are returned in a single message.
*/
WFMCM_API const char* WFMCM_CALLCONV getResultErrorWithTimeout(
    Handle request,
    int timeoutSec);

/**
 * @brief Indicates if response is pending or if it has arrived and it contains
 *        errors which can be fetched by the application.
 * @param request A handle to a request object.
 * @return `ApiResultStatus`
 * @note The application may call `getResultError()` if this function returns true.
 * @note This function is not equivalent to `hasHandleError()` which indicates an
 *       an error or fault in the library itself. `hasResultError()` indicates a business
 *       error which occured on the backend.
*/
WFMCM_API int WFMCM_CALLCONV hasResultError(Handle request);

/**
 * @brief Cancel an ongoing request.
 * @param request A handle to a request object.
 * @return 0 on success, !=0 otherwise.
 * @note This can be called immediately after `execute()` and before one of the `getResult()` functions is called.
 *       To cancel a request while `getResult()` or `getResultWithTimeout()` is pending, it must be called from
 *       another thread. Upon success, `OperationCancelled` will be returned for the actual request
 *       being cancelled.
 * @warning Use `getError()` to get the error if needed. Do not call `getHandleError()` as it
            will return the error for the request handle which is probably not what's intended.
*/
WFMCM_API int WFMCM_CALLCONV cancelRequest(Handle request);

/*-----------------------------------------------------------------------------------------
                                      GLOBAL AND HANDLE ERRORS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Get the last global error (if any) after an API function has been invoked.
 * @return The error (or success) string in JSON format.
 * @note This error will reset with each new API call. This error is per-thread, meaning that
 *       separate threads invoking API functions may have different errors
 *       at the same time.
*/
WFMCM_API const char* WFMCM_CALLCONV getError();

/**
 * @brief Checks if there was an execution error in the last API function call.
 * @return True if yes, False otherwise.
*/
WFMCM_API bool WFMCM_CALLCONV hasError();

/**
 * @brief Similar to `getError()` but specific to a particular handle.
 * @param handle A handle to an object.
 * @return The error (or success) string in JSON format.
 * @note If a handle error occurs, the global error is also set, therefore `getError()`
 *       shall return the same value. However, unlike `getError()` this function only
 *       resets when the next API call is made using this handle.
*/
WFMCM_API const char* WFMCM_CALLCONV getHandleError(Handle handle);

/**
 * @brief Similar to `hasError()`, this function indicates if a handle has an error.
 * @param handle A handle to an object.
 * @return True if yes, False otherwise. If the handle is invalid this function
 *         returns False.
*/
WFMCM_API bool WFMCM_CALLCONV hasHandleError(Handle handle);

/*-----------------------------------------------------------------------------------------
                                      RESOURCE MANAGEMENT
-------------------------------------------------------------------------------------------*/
/**
 * @brief Delete a handle and the object associated with it.
 * @param handle A handle to an object.
 * @return 0 on success, !=0 otherwise.
 * @note This function should only be called when using the C API directly. When using
 *       higher-level language bindings (C#, Java, Python), this function will be
 *       automatically called when the Handle instance goes out of scope.
*/
WFMCM_API int WFMCM_CALLCONV deleteHandle(Handle handle);

/**
 * @brief Clone an existing handle object by value.
 * @param handle A handle to an object.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
 * @note If this handle references (binds to) other handles, these bindings are preserved in the clone.
         If other handles reference the original handle, the cloning operation shall not affect that binding.
*/
WFMCM_API Handle WFMCM_CALLCONV cloneHandle(Handle handle);

/**
 * @brief Indicates if this handle references a valid object or is null.
 * @param handle The handle to check.
 * @return True if valid, False if null.
*/
WFMCM_API bool WFMCM_CALLCONV isValidHandle(Handle handle);

/**
 * @brief Utility function which returns a null handle.
          This can be used in places where optional handles are allowed.
 * @return A null handle.
*/
WFMCM_API Handle WFMCM_CALLCONV nullHandle();

/**
 * @brief Indicates if a `Date` object is valid.
 * @param date The date to check.
 * @return True if date >= 1976/01/01, False otherwise.
*/
WFMCM_API bool WFMCM_CALLCONV isValidDate(Date date);

/**
 * @brief Utility function which returns a default-initialized date ("0000-00-00").
          This can be used in places where optional dates are allowed.
 * @return A null date.
*/
WFMCM_API Date WFMCM_CALLCONV nullDate();

/*-----------------------------------------------------------------------------------------
                                     HANDLE ARRAY
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a bounded Handle array of _homogenous_ handle types.
 * @param size The length of the array. Must be > 0.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV createHandleArray(int size);

/**
 * @brief Insert a handle at a specific position.
 * @param handleArray The array handle object.
 * @param position The position at which to insert the element. Must be [0, size). See `createHandleArray()`.
 * @param handle The handle to insert.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV insertHandleAt(Handle handleArray, int position, Handle handle);

/**
 * @brief Get a handle at a specific position.
 * @param handleArray The array handle object.
 * @param position The position at which to insert the element. Must be [0, size). See `createHandleArray()`.
 * @return An valid object handle, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
*/
WFMCM_API Handle WFMCM_CALLCONV getHandleAt(Handle handleArray, int position);

/*-----------------------------------------------------------------------------------------
                                FLOATING POINT MATRIX
-------------------------------------------------------------------------------------------*/
/**
 * @brief Create a floating point matrix (contains `double` values).
 * @param rows The max number of rows in the matrix.
 * @param cols The max number of columns in the matrix.
 * @return An valid object handle on success, a null one otherwise.
 * @note Use `isValidHandle()` for validation.
 * @note Reserved for later use.
*/
WFMCM_API Handle WFMCM_CALLCONV createFloatingPointMatrix(
    int rows,
    int cols);

/**
 * @brief Set a value in the matrix.
 * @param matrix A handle to a matrix object. See `createFloatingPointMatrix()`.
 * @param row The row index, where index < max number of rows in the matrix.
 * @param col The column index, where index < max number of columns in the matrix.
 * @param value The value to set.
 * @return 0 on success, !=0 otherwise.
*/
WFMCM_API int WFMCM_CALLCONV setFloatingPointMatrixValue(
    Handle matrix,
    int row,
    int col,
    double value);

#ifdef __cplusplus
}
#endif

#endif
