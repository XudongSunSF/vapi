#ifndef WFMCM_API_COMPONENTS_H
#define WFMCM_API_COMPONENTS_H

#ifdef _WIN32
#ifdef WFMCM_SDK
#define WFMCM_API __declspec(dllimport)
#else
#define WFMCM_API __declspec(dllexport)
#endif
#define WFMCM_CALLCONV __stdcall
#else
#define WFMCM_API
#define WFMCM_CALLCONV
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------------------------------------
                                     PUBLIC ENUMS
-------------------------------------------------------------------------------------------*/
/**
 * @brief Enum defining supported request types
*/
enum ApiRequest
{
    ApiRequest_GenRatePathsForMortgageValuation = 1,        ///< Generate paths which can be used by a pricing engine
    ApiRequest_CalcValueForMortgage,                        ///< Mortgage pricing  
    ApiRequest_CalcValueForMortgageWithSofrRates,           ///< Mortgage pricing with sofr rates provided
    ApiRequest_CalcGreeksFromPrices,                        ///< Greeks calculation. Requires user-provided pricing input  
    ApiRequest_AttribValueChangeByWaterfall,                ///< Waterfall attribution calculation
    ApiRequest_GenPrimaryMortgageRatePaths,                 ///< Generate primary mortgage rate paths
    ApiRequest_GenSecondaryMortgageRatePaths,               ///< Generate secondary mortgage rate paths       
    ApiRequest_CalcValueForMonthEndRoll,                    ///< Month end roll 
    ApiRequest_GenRatePathsForWaterfallAttribution,         ///< Generate paths for waterfall attribution
    ApiRequest_CalcBehavioralSpeedFromPrimaryRate,          ///< Calculate behavioral model speeds from primary rates
    ApiRequest_CalcValueForMortgageFromRates,               ///< Calculate Mortgage pricing with rates provided
    ApiRequest_CalcProfitabilityFromBehavioralSpeeds,      ///< Calculate profitability from behavioral speeds
    ApiRequest_UNKNOWN                                      ///< Unknown value
};

/**
 * @brief Enum defining supported models.
*/
enum ApiModel
{
    ApiModel_BehavioralPldm,            ///< Behavioral model - Pool level default model (PLDM), Pool & Loan level
    ApiModel_BehavioralAdco,            ///< Behavioral model - ADCo suite of models 
    ApiModel_BehavioralJftm,            ///< Behavioral model - Jumbo Fixed Transition Model (JFTM)
    ApiModel_BehavioralJatm,            ///< Behavioral model - Jumbo ARM Transition Model (JATM)
    ApiModel_BehavioralCrt,             ///< Behavioral model - Agency Default Transition Model, Pool level
    ApiModel_BehavioralCfpm,            ///< Behavioral model - Conventional Fixed Pool Level Prepayment Model (CFPM)
    ApiModel_BehavioralGfpm,            ///< Behavioral model - Government Fixed Pool Level Prepayment Model (GFPM)
    ApiModel_BehavioralCapm,            ///< Behavioral model - Conventional ARM Pool Level Prepayment Model (CAPM)
    ApiModel_BehavioralAdcoTuning,      ///< Behavioral model - ADCo tuning
    ApiModel_BehavioralModelMap,        ///< Behavioral model Map - not a model, will be applied on portfolio before execute
    ApiModel_PrimaryRateDpss,           ///< Primary Mortgage Rate Model - Dynamic primary secondary spread (DPSS)
    ApiModel_PrimaryRateAdco,           ///< Primary Mortgage Rate Model - ADCo primary rate model
    ApiModel_SecondaryRateTbaMarket,    ///< Secondary Mortgage Rate Model - Basis model 1  {FIXME}: deprecated model type
    ApiModel_SecondaryRateStatistical,  ///< Secondary Mortgage Rate Model - Basis model 2-4, Statistical Basis model
    ApiModel_InterestRateConstant,      ///< Interest Rate Model - Generate constant rate path
    ApiModel_InterestRateStatic,        ///< Interest Rate Model - Generate static rate path from the curve
    ApiModel_InterestRateMonteCarlo,    ///< Interest Rate Model - Generate monte carlo rate paths
    ApiModel_CurveBuilder,              ///< Model that builds the curve in accordance with CurveModificationSpec
    ApiModel_CurveInterpolation,        ///< Set curve interpolation method in accordance with CurveInterpolationSpec
    ApiModel_CashflowModelMsr,          ///< Cashflow Model - for msr
    ApiModel_CashflowModelMbsFixed,     ///< Cashflow Model - for mbs fixed rate
    ApiModel_CashflowModelMbsFloat,     ///< Cashflow Model - for mbs float rate
    ApiModel_CashflowModelWholeLoanFixed,    ///< Cashflow Model - for whole loans fixed rate
    ApiModel_DiscountingPoly,           ///< Discounting Model - Poly
    ApiModel_DiscountingBusch,          ///< Discounting Model - Busch
    ApiModel_DiscountingXva,            ///< Discounting Model - Xva
    ApiModel_Sensitivities,             ///< Risk sensitivities
    ApiModel_Profitability,             ///< Profitability model
    ApiModel_UNKNOWN                    ///< Unknown value
};

/**
 * @brief Enum defining the format of curve input data.
*/
enum ApiCurveInputFormat
{
    ApiCurveInputFormat_LiborCurve,         ///< Curve input expressed in LIBOR market convention
    ApiCurveInputFormat_SofrCurve,          ///< Curve input expressed in SOFR market convention
    ApiCurveInputFormat_ForwardRates,       ///< Curve input expressed as 1M forward rates 
    ApiCurveInputFormat_DiscountFactors,    ///< Curve input expressed as time and discount factors
    ApiCurveInputFormat_UNKNOWN             ///< Unknown value
};

/**
 * @brief Enum defining curve interpolation method.
*/
enum ApiInterpolationMethod
{
    ApiInterpolationMethod_Constant,        ///< Flat (piecewise-constant) interpolation
    ApiInterpolationMethod_Linear,          ///< Linear interpolation
    ApiInterpolationMethod_LogLinear,       ///< Log-linear interpolation
    ApiInterpolationMethod_Cubic,           ///< Cubic interpolation
    ApiInterpolationMethod_MonotoneConvex,  ///< Monotone-convex interpolation
    ApiInterpolationMethod_UNKNOWN          ///< Unknown value
};

/**
 * @brief Enum defining the allowed model configuration options.
*/
enum ApiModelParameter
{
    ApiModelParameter_ModelParams = 0,          ///< Parameters of a model
    ApiModelParameter_BusinessDays,             ///< Number of business days in a month
    ApiModelParameter_AverageBusinessDays,      ///< Number of average business days in a year
    ApiModelParameter_CohortDefaultValues,      ///< For behavioral models, default values of a MBS pool when collateral characteristics are missing
    ApiModelParameter_LlpaData,                 ///< For behavioral models, LLPA matrices
    ApiModelParameter_LlpaMetadata,             ///< For behavioral models, meta data that describes the LLPA matrices
    ApiModelParameter_ConformingLimits,         ///< For behavioral models, GSE comforming limits for each state
    ApiModelParameter_OwacHistory,              ///< For behavioral models, original WAC time series for each product
    ApiModelParameter_ServicerSpeed,            ///< For behavioral models, servicer speed assumptions
    ApiModelParameter_MonteCarloCorrelation,    ///< For MonteCarlo IR models, correlation of forward rates
    // ApiModelParameter_MonteCarloSettings,       ///< For MonteCarlo IR models, settings includes dynamics, vol grid axes
    ApiModelParameter_AdcoDataPath,             ///< Path to folder in which ADCo data files are stored
    ApiModelParameter_AdcoDllPath,              ///< Path to folder in which ADCo dll is located
    ApiModelParameter_AdcoModelVersion,         ///< Adco model version. The DLL should be located under 'AdcoDllPath/AdcoModelVersion'.
    ApiModelParameter_UNKNOWN                   ///< Unknown value
};

/**
 * @brief Enum defining the allowed session configuration options.
*/
enum ApiSessionParameter
{
    ApiSessionParameter_PrepayMultiplier,       ///< Total SMM multiplier knob (in deciamal format - default is 1.0)
    ApiSessionParameter_LossMultiplier,         ///< Total SMM multiplier knob (in deciamal format - default is 1.0)
    ApiSessionParameter_RefinanceMultiplier,    ///< Refinance multipler knob (in deciamal format - default is 1.0)
    ApiSessionParameter_TurnoverMultiplier,     ///< Turnover multipler knob (in deciamal format - default is 1.0)
    ApiSessionParameter_Multipliers,            ///< For behavioral models, multiplier definitions
    ApiSessionParameter_Dials,                  ///< Model dials exposed to users for further tuning
    ApiSessionParameter_PrimaryRateSource,      ///< Source of the primary rate. "Calculation": calculate from model (default). "Market": read from market.
    ApiSessionParameter_OutputDir,              ///< {DUPLICATE} Directory to store the run output
    ApiSessionParameter_AggregatedDebugOutput,  ///< For behavioral models, output aggregated speeds
    ApiSessionParameter_DetailedDebugOutput,    ///< For behavioral models, output intermeidate results including speeds
    ApiSessionParameter_InstrumentFileOutputMode,///< When printing instrument diagnostics, indicate the mode to use. 
                                                 ///< Values are: 'GlobalSingleShared', 'PerThreadSeparateDir', 'PerThreadSingleDir'.
    ApiSessionParameter_ModelOutputAppend,      ///< If true, appends to the debug output file. By default the file is always trucated.
    ApiSessionParameter_BoundBasisOption,       ///< For the Statistical Basis Model, whether to bound the basis in a range
    ApiSessionParameter_ProjectionLength,       ///< For behavioral models, projection periods
    ApiSessionParameter_HistoricalIndex,          ///< {?} Historical secondary mortgage rates
    ApiSessionParameter_CalibrationBasket,      ///< For IR models, but used in vol data reading and greek algo building, Monte Carlo vol calibration benchmark
    ApiSessionParameter_CalibratedVolParams,    ///< For Monte Carlo IR models, calibrated vol params
    ApiSessionParameter_RateHistory,            ///< For behavioral models, economic data including rate histories
    ApiSessionParameter_EconScenario,           ///< For GN model, economic data including weights, p2y etc, excluding rate histories
    ApiSessionParameter_MonteCarloPaths,        ///< For Monte Carlo IR models, monte carlo path number
    ApiSessionParameter_YieldCurveLog,             ///< Boolean switch control on generating yield curve log
    ApiSessionParameter_SommCalibrationLog,    ///< Boolean switch control on generating somm calibration log
    ApiSessionParameter_SommSimulationLog,    ///< Boolean switch control on generating somm simulation log
    ApiSessionParameter_VolatilitySurfaceLog,    ///< Boolean switch control on generating volatility surface log
    ApiSessionParameter_DiagLogForBaseScnOnly,  ///< Boolean switch control on whether diagnostic log is for base scenario only or all scenarios
    ApiSessionParameter_ProfitabilityEquityType, ///< For profitability model, the type of equity to use (book value, market value, etc)
    ApiSessionParameter_ProfitabilityFalloutAdjust, ///< For profitability model, whether to adjust profitability with fallout
    ApiSessionParameter_ProfitabilityDataType, ///< For profitability model, the type of data to use (actual, smoothed, etc)
    ApiSessionParameter_ProfitabilityFullCost, ///< For profitability model, whether to use full cost or contribution margin
	ApiSessionParameter_ProfitabilityRunWithABRD, ///< For profitability model, whether to run with ABRD or not
    ApiSessionParameter_ProfitabilityAssumptionsFolder, ///< For profitability model, assumptions such as discount rate, terminal value multiple, etc
    ApiSessionParameter_ProfitabilityInputCashflowFile, ///< For profitability model, the input cashflow file
    ApiSessionParameter_ProfitabilityPricingDate, ///< For profitability model, Pricing Date
    ApiSessionParameter_ProfitabilityMonthlyCashflow, /// < For profitability model, print monthly cashflows or not
    ApiSessionParameter_UNKNOWN                 ///< Unknown value
};

/**
 * @brief Enum defining the supported Greeks.
*/
enum ApiGreekType
{
    ApiGreekType_DV01 = 0,          ///< Parallel rate shock DV01 
    ApiGreekType_CV01,              ///< Parallel rate shock CV01
    ApiGreekType_KeyRateDV01,       ///< Key rate shock DV01
    ApiGreekType_KeyRateCV01,       ///< Key rate shock CV01
    ApiGreekType_PartialVega,       ///< Key vol shock vega
    ApiGreekType_PartialVolga,      ///< Key vol shock volga
    ApiGreekType_ParallelVega,      ///< Parallel vol shock vega
    ApiGreekType_ParallelVolga,      ///< Parallel vol shock volga
    ApiGreekType_KeyMortgageBasis01,   ///< Key mortgage basis shock DV01
    ApiGreekType_KeyMortgageBasisCV01, ///< Key mortgage basis shock CV01
    ApiGreekType_MortgageBasis01,   ///< Total mortgage basis shock DV01
    ApiGreekType_MortgageBasisCV01, ///< Total mortgage basis shock CV01
#ifdef TimeValueDisabled
    ApiGreekType_TimeValue,         ///< Value due to time lapse
#endif
    ApiGreekType_HPI01,             ///< Home price appreciation rate shock DV01
    ApiGreekType_HPICV01,           ///< Home price appreciation rate shock CV01
    ApiGreekType_Unemployment01,    ///< Unemployment rate shock DV01
    ApiGreekType_UnemploymentCV01,  ///< Unemployment rate shock CV01
    ApiGreekType_Banna,             ///< Rate shock and FNCC shock
    ApiGreekType_Vanna,             ///< Rate shock and vol shock
    ApiGreekType_Manna,             ///< Vol shock and FNCC shock
    ApiGreekType_CrossGamma,        ///< Cross gamma
    ApiGreekType_TimeDecay,         ///< Time decay
    ApiGreekType_TemporaryPss01,    ///< Short term pss DV01
    ApiGreekType_TemporaryPssCV01,  ///< Short term pss CV01
    ApiGreekType_PermanentPss01,    ///< Permanent pss DV01
    ApiGreekType_PermanentPssCV01,  ///< Permanent pss CV01
    ApiGreekType_SofrDV01,          ///< Parallel Sofr curve shock DV01
    ApiGreekType_UstDV01,           ///< Parallel Ust curve shock DV01
    ApiGreekType_ROE01,             ///< ROE rate shock DV01
    ApiGreekType_ROECV01,           ///< ROE rate shock CV01
    ApiGreekType_OAS01,             ///< Option adjusted spd shock DV01
    ApiGreekType_OASCV01,           ///< Option adjusted spd shock CV01
    ApiGreekType_Prepayment01,      ///< Prepay speed shock DV01
    ApiGreekType_PrepaymentCV01,    ///< Prepay speed shock CV01
    ApiGreekType_Default01,         ///< Default rate shock DV01
    ApiGreekType_DefaultCV01,       ///< Default rate shock CV01
    ApiGreekType_Refinance01,       ///< Prepayment refinance factor shock DV01
    ApiGreekType_RefinanceCV01,     ///< Prepayment refinance factor shock CV01
    ApiGreekType_Turnover01,        ///< Prepayment turnover factor shock DV01
    ApiGreekType_TurnoverCV01,      ///< Prepayment turnover factor shock CV01
    ApiGreekType_UNKNOWN            ///< Unknown value
};

/**
 * @brief Enum defining the date shift strategy.
*/
enum ApiCurveDateShift
{
    ApiCurveDateShift_KeepZeroRateSame,         ///< Keep spot rates in the calibrate curve the same
    ApiCurveDateShift_KeepSwapRateSame,         ///< Keep input swap rates to curve calibration the same 
    ApiCurveDateShift_KeepForwardRateSame,      ///< Keep the curve implied forward rate the same
    ApiCurveDateShift_KeepDiscountFactorSame,   ///< Keep the curve implied discount factors the same
    ApiCurveDateShift_UNKNOWN                   ///< Unknown value
};

/**
 * @brief Enum defining the waterfall step.
*/
enum ApiWaterfallStep
{
    //step 0 is reserved
    ApiWaterfallStep_Volatility = 1,            ///< Update market volatility from T_0 to T_1
    ApiWaterfallStep_Rates,                     ///< Update market swap rates from T_0 to T_1
    ApiWaterfallStep_MortgageRates,             ///< Update market mortgage rates from T_0 to T_1,
    ApiWaterfallStep_UNKNOWN                    ///< Unknown value
};

enum ApiMonthEndRollStep
{
    //step 0 is reserved
    ApiMonthEndRollStep_Curve = 1,
    ApiMonthEndRollStep_Volatility,
    ApiMonthEndRollStep_HistSecondaryRate,
    ApiMonthEndRollStep_Hpi,
    ApiMonthEndRollStep_AsOfSecondaryRate,
    ApiMonthEndRollStep_HistPrimaryRate,
    ApiMonthEndRollStep_AsOfPrimaryRate,
    ApiMonthEndRollStep_UNKNOWN                ///< Unknown value
};

/**
 * @brief Enum defining the desired output from a request.
*/
enum ApiPathOutputType
{
    ApiPathOutputType_SwapRates,                ///< Scenario SOFR swap rates
    ApiPathOutputType_BaseSwapRates,            ///< Base SOFR swap rates
    ApiPathOutputType_PrimaryRates,             ///< Scenario primary mortgage rates
    ApiPathOutputType_BasePrimaryRates,         ///< Base primary mortgage rates
    ApiPathOutputType_SecondaryRates,           ///< Scenario secondary mortgage rates
    ApiPathOutputType_BaseSecondaryRates,       ///< Base secondary mortgage rates
    ApiPathOutputType_UstSwapRates,             ///< Scenario US Treasury swap rates
    ApiPathOutputType_BaseUstSwapRates,         ///< Base US Treasury swap rates
    ApiPathOutputType_DiscountRates,             ///< Scenario discount rates
    ApiPathOutputType_BaseDiscountRates,         ///< Base discount rates
    ApiPathOutputType_UNKNOWN                   ///< Unknown value
};

/**
 * @brief Enum defining the type of pricing to be calculated.
*/
enum ApiPricingOutputType
{
    ApiPricingOutputType_Price,             ///< Calculate price from OAS
    ApiPricingOutputType_OAS,               ///< Calculate OAS from price
    ApiPricingOutputType_ConstantYield,     ///< Calculate constant yield from price
    ApiPricingOutputType_UNKNOWN            ///< Unknown value
};

/**
 * @brief Enum defining the additional debug info requested.
*/
enum ApiDebugInfoType 
{
    ApiDebugInfoType_SwapRates,             ///< {?}{DUPLICATE} Scenario SOFR swap rates
    ApiDebugInfoType_BaseSwapRates,
    ApiDebugInfoType_PrimaryRates,
    ApiDebugInfoType_BasePrimaryRates,
    ApiDebugInfoType_SecondaryRates,
    ApiDebugInfoType_BaseSecondaryRates,
    ApiDebugInfoType_BehavioralModelOutputs,
    ApiDebugInfoType_CashFlowModelOutputs,
    ApiDebugInfoType_DiscountingModelOutputs,
    ApiDebugInfoType_UNKNOWN                ///< Unknown value
};

/**
 * @brief Enum defining the secondary mortgage types supported.
*/
enum ApiSecondaryRateType
{
    ApiSecondaryRateType_FN30,       ///< Secondary mortgage rate conventional fixed 30
    ApiSecondaryRateType_FN15,       ///< Secondary mortgage rate conventional fixed 15
    ApiSecondaryRateType_GN30,       ///< Secondary mortgage rate GN fixed 30
    ApiSecondaryRateType_GN15,       ///< Secondary mortgage rate GN fixed 15
    ApiSecondaryRateType_UNKNOWN     ///< Unknown value
};

/**
 * @brief Enum defining the primary mortgage types supported.
*/
enum ApiPrimaryRateType
{
    //FIXED RATE
    ApiPrimaryRateType_fhmrate,		    
    ApiPrimaryRateType_fhcr15,	
    ApiPrimaryRateType_fhmrate_icon,
    ApiPrimaryRateType_fhcr15_icon,
    ApiPrimaryRateType_fhmrate_pmms,
    ApiPrimaryRateType_fhcr15_pmms,
    ApiPrimaryRateType_conv_fixed20_pmms,
    ApiPrimaryRateType_conv_fixed10_pmms,
    ApiPrimaryRateType_fha15_pmms,
    ApiPrimaryRateType_va15_pmms,
    ApiPrimaryRateType_conf2fhmrate,
    ApiPrimaryRateType_conf2fhcr15,
    ApiPrimaryRateType_jumbofhmrate,
    ApiPrimaryRateType_jumbofhcr15,
    ApiPrimaryRateType_fhafhmrate,
    ApiPrimaryRateType_fhafhmrate_icon,
    ApiPrimaryRateType_fhafhmrate_icon_purchase,
    ApiPrimaryRateType_fhafhcr15,	
    ApiPrimaryRateType_vafhmrate,
    ApiPrimaryRateType_vafhmrate_icon,
    ApiPrimaryRateType_vafhmrate_icon_purchase,
    //ApiPrimaryRateType_vafhcr15,
    ApiPrimaryRateType_altafhmrate,
    ApiPrimaryRateType_gsejumbofhmrate,
    ApiPrimaryRateType_gsejumbofhmrate_icon_purchase,
    ApiPrimaryRateType_gsejumbofhcr15,
    ApiPrimaryRateType_gsejumbofhcr15_icon_purchase,
    ApiPrimaryRateType_nafhmrate,
    ApiPrimaryRateType_nafhcr15,
    ApiPrimaryRateType_najumbofhmrate,
    ApiPrimaryRateType_najumbofhcr15,
    ApiPrimaryRateType_nagsejumbofhmrate,
    ApiPrimaryRateType_nagsejumbofhcr15,
    //ARM
    ApiPrimaryRateType_carm31,
    ApiPrimaryRateType_carm51,
    ApiPrimaryRateType_carm71,
    ApiPrimaryRateType_carm101,
    ApiPrimaryRateType_carm51_icon,
    ApiPrimaryRateType_carm71_icon,
    ApiPrimaryRateType_carm101_icon,
    ApiPrimaryRateType_jarm31,
    ApiPrimaryRateType_jarm51,
    ApiPrimaryRateType_jarm71,
    ApiPrimaryRateType_jarm101,
    ApiPrimaryRateType_jarm31_icon,
    ApiPrimaryRateType_jarm51_icon,
    ApiPrimaryRateType_jarm71_icon,
    ApiPrimaryRateType_jarm101_icon,
    ApiPrimaryRateType_jarm51_icon_purchase,
    ApiPrimaryRateType_jarm71_icon_purchase,
    ApiPrimaryRateType_jarm101_icon_purchase,
    ApiPrimaryRateType_gsejarm31,
    ApiPrimaryRateType_gsejarm51,
    ApiPrimaryRateType_gsejarm71,
    ApiPrimaryRateType_gsejarm101,
    //NON AGENCY
    ApiPrimaryRateType_jumbofhmrate_icon,
    ApiPrimaryRateType_jumbofhcr15_icon,
    ApiPrimaryRateType_jumbofhmrate_icon_purchase,
    ApiPrimaryRateType_jumbofhcr15_icon_purchase,
    //CAPM
    ApiPrimaryRateType_carm51_icon_purchase,
    ApiPrimaryRateType_carm71_icon_purchase,
    ApiPrimaryRateType_carm101_icon_purchase,
    ApiPrimaryRateType_UNKNOWN              ///< Unknown value
};

enum ApiPortfolioType
{
    ApiPortfolioType_Mbs,    ///< Mortgage backed securities 
    ApiPortfolioType_Msr,    ///< Mortgage servicing rights 
    ApiPortfolioType_UNKNOWN ///< Unknown value
};

/**
 * @brief Indicates if the derivative **dV/dX** is total or partial, where `V` is a valuation function, and `X` is an input to the valuation function. 
          Suppose **V = f(Swap, Fncc(Swap), ...)** is the mortgage valuation function. 
          For total derivative, we calculate **V' = f(Swap+d, Fncc(Swap+d), ...)**. 
          For the partial derivative, we calculate **V' = f(Swap+d, Fncc(Swap), ...)**.
*/
enum ApiDerivativeCalculation
{
    ApiDerivativeCalculation_Partial,   ///< Use partial derivative
    ApiDerivativeCalculation_Total,     ///< Use total derivative
    ApiDerivativeCalculation_UNKNOWN    ///< Unknown value
};

/**
 * @brief Determines how certain models project or price data.
*/
enum ApiExecMode
{
    ApiExecMode_Parallel,   ///< Run in parallel mode
    ApiExecMode_Serial,     ///< Run in serial mode
    ApiExecMode_UNKNOWN     ///< Unknown value
};

/**
 * @brief Determines which executor type to use.
*/
enum ApiExecutorType
{
    ApiExecutor_Ppl,   ///< use PPL based executor
    ApiExecutor_Threadpool,     ///< use thread pool based executor
    ApiExecutor_UNKNOWN     ///< Unknown value
};

/**
 * @brief Determines how certain models project or price data.
*/
enum ApiErrorBehavior
{
    ApiErrorBehavior_EarlyTermination,   ///< Terminates execution at the first error (default)
    ApiErrorBehavior_RunToCompletion,    ///< Accumulates errors and runs to completion
    ApiErrorBehavior_UNKNOWN             ///< Unknown value
};

/**
 * @brief Various types of historical rates
 */
enum ApiHistoricalData
{
    ApiHistoricalData_Hpi,                      ///< HPI data
    ApiHistoricalData_Unemployment,             ///< Unemployment data
    ApiHistoricalData_EconomicScenario,         ///< Economic scenario data
    ApiHistoricalData_MbsPrimaryMortgageRates,  ///< Mbs primary mortgage rates
    ApiHistoricalData_SecondaryMortgageRates,   ///< Secondary historical rates
    ApiHistoricalData_SofrDailyRates,           ///< Sofr daily historical rates
    ApiHistoricalData_GFee,                     ///< GFee
    ApiHistoricalData_UNKNOWN
};

enum ApiResultStatus
{
    ApiResultStatus_Pending,               ///< Request is still processing. Response pending.
    ApiResultStatus_Available,             ///< Data has arrived and can be extracted.
    ApiResultStatus_NotAvailable,          ///< Data has been extracted and there's no more left.
    ApiResultStatus_UNKNOWN
};

/**
* @brief Various types of horizon scenario type
*/
enum ApiHorizonScenarioType
{
    ApiHorizonScenarioType_Prescribed,      ///< prescribed horizon scenario, user will give the swap rate, vol, cc and primary rate in the horizon json file
    ApiHorizonScenarioType_Forward,         ///< forward horizon scenario, user will not give any rates in the horizon
    ApiHorizonScenarioType_Filebased,       ///< {FIXME}: deprecated, can be removed after GenRatesForScenario is removed. file-based horizon, user will provide the override csv file for the rates in the horizon
    ApiHorizonScenarioType_UNKNOWN
};

/*-----------------------------------------------------------------------------------------
                                        STRUCTS
-------------------------------------------------------------------------------------------*/
/**
 * @brief The handle represents an reference to a shared, thread-safe object (aka resource).
 * @note Handles can be referenced and used by multiple objects simultaneously. When a setting
 *       of a handle changes, it becomes immediately visible to all objects using it. 
 *       When a handle goes out of scope (or when `deleteHandle()` is explicitly called),
 *       the resource belonging to the handle will be released as long as no other objects are 
 *       referencing it. Once a handle is destroyed, it can no longer be used.
 * @note A null (invalid) handle can be generated with `nullHandle()`. 
 * @note Use `isValidHandle()` to determine if a handle is valid.
*/
struct Handle
{
    unsigned long long internal_; ///< Opaque data (read-only)
};

/**
 * @brief A simple date object which contains years, months and days only. 
 * @note A valid date is >= 1976/01/01 as this is the oldest historical data available.
 * @note A default-initialized (invalid) date can be generated with `nullDate()`. 
 * @note Use `isValidDate()` to determine if a date is valid.
*/
struct Date
{
    unsigned int yyyymmdd_; ///< An integer in YYYYMMDD format.
};

#ifdef BRIDGE_API_INTERNAL //Do not remove: may be needed later
struct StringBuf
{
    const char* data_;
};

#endif

#ifdef __cplusplus
}
#endif

#endif
