#include <src/app-common/api/WfmcmApiInternal.h>

// {TODO} fix include later

// 
#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/io/TimeSeriesReader.h>
#include <src/io/DiscountFactorsInputReader.h>
#include <src/io/SofrSwapCurveInputReader.h>
#include <src/io/LiborSwapCurveInputReader.h>
#include <src/io/ForwardRatesInputReader.h>
#include <src/io/MSRPortfolioReader.h>
#include <src/io/VolatilityCubeReader.h>
#include <src/io/HistoricalIndexReader.h>
#include <src/io/HistoricalRatesReader.h>
#include <src/io/MortgagePoolPortfolioReader.h>
#include <src/io/ScenarioReader.h>
#include <src/io/PathReader.h>
#include <src/io/LmmReaders.h>
#include <src/io/GreekSpecReader.h>
#include <src/io/GreekPricesReader.h>
#include <src/io/PrimaryRatesOverrideReader.h>
#include <src/io/IndexMapperReader.h>
#include <src/core/utilities/date_util.h>
#include <src/core/utilities/user_calendar_loader.h>
#include <src/core/MortgageAllocator.h>
#include <src/core/utilities/date_util.h>
#include <mortgage/utility/types/constants.h>
#include <mortgage/utility/types/optional.h>
#include <mortgage/utility/streams/icharstream.h>
#include <mortgage/utility/memory/thread_local.h>
#include <mortgage/utility/containers/cache/caching.h>
#include <src/app-common/ModelSpecifications.h>
#include <src/app-common/messages/requests/Request.h>
#include <src/app-common/messages/LocalConnector.h>
#include <src/app-common/messages/RequestManager.h>
#include <src/app-common/messages/RequestProcessor.h>
#include <src/app-common/run-config/RunConfigResolver.h>
#include <resources/mortval/version.h>
#include <src/app-common/macros.h>
#include <mortgage/utility/types/polyvar/polyvar.h>
#include <src/core/behavioral/BehavioralModelMap.h>

// New behavioral architecture includes
#include <src/core/model-suite/ModelSuitesLayer.h>
#include <src/core/model-suite/WorkflowOrchestrator.h>
#include <src/core/data-access/DataAccessLayer.h>
#include <src/app-common/messages/handlers/BaseRequestHandler.h>
#include <src/app-common/messages/responses/ErrorHandling.h>

#include <filesystem>

#include <ql/time/calendars/usercalendar.hpp>

#include <format>
#include <thread>

using namespace std;
using namespace wfmcm;
using namespace wfmcm::io;
using namespace app;
using namespace wf::mortgage::utility;
namespace msg = app::messages;
using namespace std::chrono_literals;

#ifdef BRIDGE_API_INTERNAL //For later
/*-----------------------------------------------------------------------------------------
                                      PERSISTING DATA
-------------------------------------------------------------------------------------------*/
//Some handle data can be persisted for re-usability across applicatons
Handle getPersistentData(
    const char* persistentDataId);
const char* generatePersistentDataId();
int persistData(
    Handle handle,
    const char* persistentDataId);
int persistDataForDuration(
    Handle handle,
    const char* persistentDataId,
    int durationInMinutes);
int unpersistDataById(
    const char* persistentDataId);
int unpersistDataByHandle(
    Handle handle);
#endif

/*-----------------------------------------------------------------------------------------
                                      API LIBRARY INIT
-------------------------------------------------------------------------------------------*/
once_flag initLibFlag, teardownLibFlag, globalLogFlag;

int setupApiLibrary(const char* paramDir)
{
    TRY

    if (handles)
        RETURN_API_ERROR(NullHandle, ApiErrorAlreadyInitialized, "Already initialized");
    HandleError error;
    call_once(initLibFlag, [&error, paramDir]() {
        handles = std::make_unique<app::HandleContainer>();
        //init cache
        wfmutil::caching::global::init(makeGlobalCacheSettings());

        //version resolver
        string nullDir = "";
        string modelParamDir = paramDir;
        resolver = std::make_unique<RunConfigResolver>();
        resolver->resolveRootDirectories(modelParamDir, nullDir, nullDir, nullDir, true);

        //load market calendar
        loadUserCalendar(FULL_FILE_NAME(paramDir, "calendars"));
    });
    RETURN_API_SUCCESS(NullHandle);

    EXCEPTION_API_ERROR(NullHandle);
}

int teardownApiLibrary()
{
    TRY

    if (!handles)
        RETURN_API_ERROR(NullHandle, ApiErrorLibTeardown, "Already torn down");
    call_once(teardownLibFlag, []() {
        handles.reset();
        resolver.reset();
        //delete cache
        wfmutil::caching::global::shutdown();
    });
    RETURN_API_SUCCESS(NullHandle);

    EXCEPTION_API_ERROR(NullHandle);
}

const char* apiLibraryVersion()
{
    TRY

    //get the return thread local buffer
    auto& str = store<string, id::api_response>();
    str = app::version::json();
    RETURN_SUCCESS(NullHandle, str.c_str());

    EXCEPTION_ERROR(NullHandle, empty_value<string>.c_str());
}

/*-----------------------------------------------------------------------------------------
                                      PARAMETER FILE VERSION RESOLVER
-------------------------------------------------------------------------------------------*/

int resolveModelParameterVersion(
    Handle modelSpec,
    // ApiModelParameter param,
    const char* modelVersion,
    const char* modelParamVersion,
    const char* histDataDir)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(modelSpec, HandleType::ModelSpec)
    if (modelVersion == nullptr) {
        RETURN_API_ERROR(modelSpec, ApiErrorNullArgument, "Null input [modelVersion]");
    }
    if (modelParamVersion == nullptr) {
        RETURN_API_ERROR(modelSpec, ApiErrorNullArgument, "Null input [modelParamVersion]");
    }
    WRITE_LOCK_HANDLE(modelSpec);
    auto& spec = handleObject<HandleType::ModelSpec>(modelSpec);
    string paramDir = resolver->getModelParamDir();
    string modelParamFile;
    string multipFile;
    app::VersionIndexInfo verInfo;
    string histDataDirStr = histDataDir ? string(histDataDir) : string();

    ApiModelTypes modelType = convert(spec.modelType_);
    string modelTypeStr = std::visit([](auto&& type) -> string {
        return to_string(type); }, modelType);

    // {FIXME}: need to merge this logic with the run config resolver

    // for TbaMarket secondary rate model, there is no model in version_index file
    if (modelTypeStr == "TbaMarket") {
        RETURN_API_ERROR(modelSpec, ApiErrorInvalidModelType, std::format("{} no longer supported", to_string(modelType)));
    }
    else if (modelTypeStr == "MonteCarlo") {
        // {FIXME}: For monte carlo, the to_string result and string in version_index doesn't match
        // and other interest rate model doesn't have any parameter file
        verInfo = resolver->resolveVersion("SoMM", modelVersion, modelParamVersion);
        modelParamFile = FULL_FILE_NAME(paramDir, verInfo.getParamFilename());
    }
    else {
        verInfo = resolver->resolveVersion(modelTypeStr, modelVersion, modelParamVersion);
        if (!verInfo.getParamFilename().empty())
            modelParamFile = FULL_FILE_NAME(paramDir, verInfo.getParamFilename());
    }

    if (holds_alternative<MortgageBehavioralModelType>(modelType)) {
        auto& innerModelType = get<MortgageBehavioralModelType>(modelType);
        if (histDataDirStr.empty()) {
            RETURN_API_ERROR(modelSpec, ApiErrorNullContent, format("For {} historical data folder is required, but received an empty string", to_string(innerModelType)));
        }
        spec.settings_.modelSpec_ = makeBehavioralModelSpec(innerModelType, modelParamFile, paramDir, histDataDir);
        if (innerModelType != MortgageBehavioralModelType::AdcoTuning)
        {
            // for other behavioral model except AdcoTuning, add dial file in the parameter folder to session spec
            spec.settings_.sessionSpec_[SessionParamName::Multipliers] = FULL_FILE_NAME(paramDir, verInfo.getDialFilename());
        }
    }
    else if (holds_alternative<PrimaryMortgageRateModelType>(modelType)) {
        auto& innerModelType = get<PrimaryMortgageRateModelType>(modelType);
        if (histDataDirStr.empty())
            RETURN_API_ERROR(modelSpec, ApiErrorNullContent, format("For {} historical data folder is required, but received an empty string", to_string(innerModelType)));
        spec.settings_.modelSpec_ = makePrimaryRateModelSpec(innerModelType, modelParamFile);
        spec.settings_.sessionSpec_[SessionParamName::Dials] = FULL_FILE_NAME(paramDir, verInfo.getDialFilename());
        // historical index is data, but we plug this into model spec here
        spec.settings_.sessionSpec_[SessionParamName::HistoricalIndex] = FULL_FILE_NAME(histDataDir, DefaultInputs::Dpss::HistIndexFile);
    }
    else if (holds_alternative<SecondaryMortgageRateModelType>(modelType)) {
        auto& innerModelType = get<SecondaryMortgageRateModelType>(modelType);
        spec.settings_.modelSpec_ = makeSecondaryRateModelSpec(innerModelType, modelParamFile);
        if (histDataDirStr.empty())
            RETURN_API_ERROR(modelSpec, ApiErrorNullContent, format("For {} historical data folder is required, but received an empty string", to_string(innerModelType)));
        // historical index is data, but we plug this into model spec here
        spec.settings_.sessionSpec_[SessionParamName::HistoricalIndex] = FULL_FILE_NAME(histDataDir, DefaultInputs::StatisticalBasis::HistIndexFile);
    }
    else if (holds_alternative<InterestRateModelType>(modelType)) {
        // {FIXME}: to call makeInterestRateModelSpec, we need the monte carlo correlation file as well, but this file is not included in the version_index
        spec.settings_.modelSpec_[ModelParamName::ModelParams] = modelParamFile;
        spec.settings_.sessionSpec_[SessionParamName::CalibrationBasket] = FULL_FILE_NAME(paramDir, DefaultInputs::VolatilityCalibrationBasketFile);
    }
    else if (holds_alternative<DiscountingModelType>(modelType)) {
        auto& innerModelType = get<DiscountingModelType>(modelType);
        spec.settings_.modelSpec_ = makeDiscountingModelSpec(innerModelType, modelParamFile);
    }
    else if (holds_alternative<MortgageBehavioralModelMapType>(modelType)) {
        auto& innerModelType = get<MortgageBehavioralModelMapType>(modelType);
        spec.settings_.modelSpec_ = makeBehavioralModelMapSpec(innerModelType, modelParamFile);
    }
    else {
        RETURN_API_ERROR(modelSpec, ApiErrorInvalidModelType, std::format("Unsupported model type {}", to_string(modelType)));
    }

    RETURN_API_SUCCESS(modelSpec);

    EXCEPTION_API_ERROR(modelSpec);

}

int resolveGreekSpec(
    Handle greekSpec,
    const char* modelVersion,
    const char* modelParamVersion)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(greekSpec, HandleType::GreekSpec)
    if (modelVersion == nullptr) {
        RETURN_API_ERROR(greekSpec, ApiErrorNullArgument, "Null input [modelVersion]");
    }
    if (modelParamVersion == nullptr) {
        RETURN_API_ERROR(greekSpec, ApiErrorNullArgument, "Null input [modelParamVersion]");
    }
    WRITE_LOCK_HANDLE(greekSpec);
    auto& spec = handleObject<HandleType::GreekSpec>(greekSpec);

    string paramDir = resolver->getModelParamDir();
    app::VersionIndexInfo verInfo = resolver->resolveVersion("GreekSpec", string(modelVersion), string(modelParamVersion));

    auto newSpec = GreekSpecReader::readFromJsonFile(FULL_FILE_NAME(paramDir, verInfo.getParamFilename()));
    spec = std::move(get<1>(newSpec));
    RETURN_API_SUCCESS(greekSpec);

    EXCEPTION_API_ERROR(greekSpec);
}

/*-----------------------------------------------------------------------------------------
                                      Global Logger
-------------------------------------------------------------------------------------------*/
int initGlobalLogger(const char* dirPath) {
    TRY

    call_once(globalLogFlag, [dirPath]() {
        logging::global::init(makeGlobalLogSettings(
            "globalLogging.log",
            string(dirPath),
            logging::log_level::debug,
            logging::log_level::error));

        wfmcm::scoped_function localLoggers([]() { logging::thread::local::shutdown(); });

        GLOG_INFO("*** Starting ***");
        });

    RETURN_API_SUCCESS(NullHandle);

    EXCEPTION_API_ERROR(NullHandle);
}

/*-----------------------------------------------------------------------------------------
                                CREATE REQUEST
-------------------------------------------------------------------------------------------*/
Handle createRequestObject(ApiRequest requestType)
{
    TRY

    CHECK_INIT2;
    switch(requestType)
    {
    case ApiRequest_GenRatePathsForMortgageValuation:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::GenRatePathsForMortgageValuationRequest,
            HandleObjectType<HandleType::GenRatePathsForMortgageValuationRequest>{}));
    case ApiRequest_CalcGreeksFromPrices:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::CalcGreeksFromPricesRequest,
            HandleObjectType<HandleType::CalcGreeksFromPricesRequest>{}));
    case ApiRequest_CalcValueForMonthEndRoll:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::CalcValueForMonthEndRollRequest,
            HandleObjectType<HandleType::CalcValueForMonthEndRollRequest>{}));
    case ApiRequest_GenRatePathsForWaterfallAttribution:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::GeneratePathForWaterfallAttributionRequest,
            HandleObjectType<HandleType::GeneratePathForWaterfallAttributionRequest>{}));
    case ApiRequest_CalcBehavioralSpeedFromPrimaryRate:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::CalcBehavioralSpeedFromPrimaryRateRequest,
            HandleObjectType<HandleType::CalcBehavioralSpeedFromPrimaryRateRequest>{}));
    case ApiRequest_GenPrimaryMortgageRatePaths:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::GenPrimaryMortgageRatePathsRequest,
            HandleObjectType<HandleType::GenPrimaryMortgageRatePathsRequest>{}));
    case ApiRequest_GenSecondaryMortgageRatePaths:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::GenSecondaryMortgageRatePathsRequest,
            HandleObjectType<HandleType::GenSecondaryMortgageRatePathsRequest>{}));
    case ApiRequest_CalcValueForMortgage:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::CalcValueForMortgageRequest,
            HandleObjectType<HandleType::CalcValueForMortgageRequest>{}));
    case ApiRequest_CalcValueForMortgageFromRates:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::CalcValueForMortgageFromRatesRequest,
            HandleObjectType<HandleType::CalcValueForMortgageFromRatesRequest>{}));
    case ApiRequest_CalcProfitabilityFromBehavioralSpeeds:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::CalcProfitabilityFromBehavioralSpeedsRequest,
            HandleObjectType<HandleType::CalcProfitabilityFromBehavioralSpeedsRequest>{}));
#if 0 //Enable when supported
    case ApiRequest_AttribValueChangeByWaterfall:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::AttribValueChangeByWaterfallRequest,
            HandleObjectType<HandleType::AttribValueChangeByWaterfallRequest>{}));
    case ApiRequest_CalcBehavioralSpeedFromPrimaryRate:
        return handles->addHandleData(make_shared<RequestHandleData>(
            HandleType::CalcBehavioralSpeedFromPrimaryRateRequest,
            HandleObjectType<HandleType::CalcBehavioralSpeedFromPrimaryRateRequest>{}));
#endif
    default:
        RETURN_ERROR(NullHandle, ApiErrorInvalidRequestType, "Unknown request type: " + std::to_string(requestType), NullHandle);
    }

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

Handle createRequestObject(ApiInternalRequest requestType)
{
    TRY

    CHECK_INIT2;
    switch(requestType)
    {
    case ApiInternalRequest_Cancel:
        return handles->addHandleData(make_shared<RequestHandleData>(
                HandleType::CancelRequest,
                HandleObjectType<HandleType::CancelRequest>{}));
    default:
        RETURN_ERROR(NullHandle, ApiErrorInvalidRequestType, "Unknown internal request type: " + std::to_string(requestType), NullHandle);
    }

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

Handle createRequest(ApiRequest requestType)
{
    return createRequestObject(requestType);
}

/*-----------------------------------------------------------------------------------------
                                    MODEL OPTIONS
-------------------------------------------------------------------------------------------*/
int bindModelOptions(Handle request, Handle modelOptions)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_HANDLE(modelOptions, HandleType::ModelOptions);
    WRITE_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(modelOptions)

    switch (handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(to).modelOptions_ =
                    handleObject<HandleType::ModelOptions>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageRequest:
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageRequest>(to).modelOptions_ =
                    handleObject<HandleType::ModelOptions>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageWithSofrRatesRequest:
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageWithSofrRatesRequest>(to).modelOptions_ =
                    handleObject<HandleType::ModelOptions>(from);
                return {};
            });
        break;
    case HandleType::AttribValueChangeByWaterfallRequest:
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::AttribValueChangeByWaterfallRequest>(to).modelOptions_ =
                    handleObject<HandleType::ModelOptions>(from);
                return {};
            });
        break;
    case HandleType::GenPrimaryMortgageRatePathsRequest:
    {
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                if (!handleObject<HandleType::ModelOptions>(from).primaryRateModelOptions_.empty()) {
                    auto& opt = handleObject<HandleType::GenPrimaryMortgageRatePathsRequest>(to).modelOptions_;
                    opt.modelType_ = handleObject<HandleType::ModelOptions>(from).primaryRateModelOptions_.begin()->first;
                    opt.settings_ = handleObject<HandleType::ModelOptions>(from).primaryRateModelOptions_.begin()->second;
                }
                return {};
            });
        break;
    }
    case HandleType::GenSecondaryMortgageRatePathsRequest:
    {
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                if (!handleObject<HandleType::ModelOptions>(from).secondaryRateModelOptions_.empty()) {
                    auto& opt = handleObject<HandleType::GenSecondaryMortgageRatePathsRequest>(to).modelOptions_;
                    opt.modelType_ = handleObject<HandleType::ModelOptions>(from).secondaryRateModelOptions_.begin()->first;
                    opt.settings_ = handleObject<HandleType::ModelOptions>(from).secondaryRateModelOptions_.begin()->second;
                }
                return {};
            });
        break;
    }
    case HandleType::CalcValueForMonthEndRollRequest:
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMonthEndRollRequest>(to).modelOptions_ =
                    handleObject<HandleType::ModelOptions>(from);
                return {};
            });
        break;
    case HandleType::GeneratePathForWaterfallAttributionRequest:
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::GeneratePathForWaterfallAttributionRequest>(to).modelOptions_ =
                    handleObject<HandleType::ModelOptions>(from);
                return {};
            });
        break;
    case HandleType::CalcBehavioralSpeedFromPrimaryRateRequest:
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcBehavioralSpeedFromPrimaryRateRequest>(to).prepayModelOptions_ =
                    handleObject<HandleType::ModelOptions>(from).prepayModelOptions_;
                handleObject<HandleType::CalcBehavioralSpeedFromPrimaryRateRequest>(to).lossModelOptions_ =
                    handleObject<HandleType::ModelOptions>(from).lossModelOptions_;
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageFromRatesRequest:
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageFromRatesRequest>(to).modelOptions_ =
                    handleObject<HandleType::ModelOptions>(from);
                return {};
            });
        break;
    case HandleType::CalcProfitabilityFromBehavioralSpeedsRequest:
        BIND_HANDLES(request, modelOptions, HandleBinding::ModelOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcProfitabilityFromBehavioralSpeedsRequest>(to).modelOptions_ =
                    handleObject<HandleType::ModelOptions>(from);
                return {};
            });
        break;
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setOutputPaths(
    Handle request,
    const ApiPathOutputType* pathOutputTypes,
    int numPathOutputTypes)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_ARRAY(request, pathOutputTypes, numPathOutputTypes);
    WRITE_LOCK_HANDLE(request)
    std::set<PathOutputType>* requestedOutput = nullptr;
    switch(handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        requestedOutput = &handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(request).requestedOutput_;
        break;
    case HandleType::CalcValueForMonthEndRollRequest:
        requestedOutput = &handleObject<HandleType::CalcValueForMonthEndRollRequest>(request).requestedOutput_;
        break;
    case HandleType::GeneratePathForWaterfallAttributionRequest:
        requestedOutput = &handleObject<HandleType::GeneratePathForWaterfallAttributionRequest>(request).requestedOutput_;
        break;
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    requestedOutput->clear();
    for (int i = 0; i < numPathOutputTypes; ++i) {
        requestedOutput->insert(static_cast<app::PathOutputType>(pathOutputTypes[i]));
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setRequiredSofrSwapTenors(
    Handle request,
    const int* requiredSofrSwapTenors,
    int numTenors)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_ARRAY(request, requiredSofrSwapTenors, numTenors);
    WRITE_LOCK_HANDLE(request);
    std::pmr::vector<int>* requiredTenors = nullptr;
    switch (handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        requiredTenors = &handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(request).requiredSofrSwapTenors_;
        break;
    case HandleType::CalcValueForMortgageRequest:
        requiredTenors = &handleObject<HandleType::CalcValueForMortgageRequest>(request).requiredSofrSwapTenors_;
        break;
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    requiredTenors->clear();
    requiredTenors->reserve(numTenors);
    for (int i = 0; i < numTenors; ++i) {
        requiredTenors->push_back((requiredSofrSwapTenors[i]));
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setRequiredTreasuryTenors(
    Handle request,
    const int* requiredTreasuryTenors,
    int numTenors)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_ARRAY(request, requiredTreasuryTenors, numTenors);
    WRITE_LOCK_HANDLE(request);
    std::pmr::vector<int>* requiredTenors = nullptr;
    switch (handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        requiredTenors = &handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(request).requiredUstSwapTenors_;
        break;
    case HandleType::CalcValueForMortgageRequest:
        requiredTenors = &handleObject<HandleType::CalcValueForMortgageRequest>(request).requiredUstSwapTenors_;
        break;
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    requiredTenors->clear();
    requiredTenors->reserve(numTenors);
    for (int i = 0; i < numTenors; ++i) {
        requiredTenors->push_back((requiredTreasuryTenors[i]));
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setRequiredPrimaryRates(
    Handle request,
    const ApiPrimaryRateType* requiredPrimaryRateTypes,
    int numPrimaryRateTypes)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_ARRAY(request, requiredPrimaryRateTypes, numPrimaryRateTypes);
    WRITE_LOCK_HANDLE(request);
    std::pmr::vector<PrimaryRateType>* requiredRates = nullptr;
    switch (handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        requiredRates = &handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(request).requiredPrimaryRateTypes_;
        break;
    case HandleType::GenPrimaryMortgageRatePathsRequest:
        requiredRates = &handleObject<HandleType::GenPrimaryMortgageRatePathsRequest>(request).requiredRateTypes_;
        break;
    case HandleType::CalcValueForMonthEndRollRequest:
        requiredRates = &handleObject<HandleType::CalcValueForMonthEndRollRequest>(request).requiredRateTypes_;
        break;
    case HandleType::GeneratePathForWaterfallAttributionRequest:
        requiredRates = &handleObject<HandleType::GeneratePathForWaterfallAttributionRequest>(request).requiredRateTypes_;
        break;
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    requiredRates->clear();
    requiredRates->reserve(numPrimaryRateTypes);
    for (int i = 0; i < numPrimaryRateTypes; ++i) {
        requiredRates->push_back(static_cast<PrimaryRateType>(requiredPrimaryRateTypes[i]));
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setPrimaryKeyRatesforBehavioralSpeed(
    Handle request,
    Date asOf,
    const char* content,
    int contentLen)
{
    TRY
    CHECK_INIT;
    //CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_HANDLE(request, HandleType::CalcBehavioralSpeedFromPrimaryRateRequest)
    CHECK_CONTENT(request, content, contentLen); //accepts -1 for null-terminated content per API doc
    WRITE_LOCK_HANDLE(request);
    // read primary rates from file
    auto primaryRatesTs = TimeSeriesReader::readFromCsv({ content, (size_t)contentLen });
    auto& ratePaths = handleObject<HandleType::CalcBehavioralSpeedFromPrimaryRateRequest>(request).ratePaths_;
    auto& keyRatePaths = handleObject<HandleType::CalcBehavioralSpeedFromPrimaryRateRequest>(request).keyRatePaths_;

    for (auto& item : primaryRatesTs) {
        bool withinTsDateRange =
            ql::monthsBetween(item.second.startDate(), QuantLib::fromYYYYMMDD(asOf.yyyymmdd_)) >= 0
            && ql::monthsBetween(QuantLib::fromYYYYMMDD(asOf.yyyymmdd_), item.second.endDate()) >= 0;

        optional<wfmcm::PrimaryRateType> optPrimRateType;
        try_from_string(item.first, &optPrimRateType);
        if (optPrimRateType) {
            if (withinTsDateRange) {
                ratePaths[optPrimRateType.value()].emplace_back(item.second.at(QuantLib::fromYYYYMMDD(asOf.yyyymmdd_)), item.second.end());
            }
        }

        optional<wfmcm::KeyRate> optKeyRateType;
        try_from_string(item.first, &optKeyRateType);
        if (optKeyRateType) {
            if (withinTsDateRange) {
                keyRatePaths[optKeyRateType.value()].emplace_back(item.second.at(QuantLib::fromYYYYMMDD(asOf.yyyymmdd_)), item.second.end());
            }
        }
    }

    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setSecondaryMortgageRatePaths(
    Handle request,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, HandleType::GenPrimaryMortgageRatePathsRequest);
    CHECK_CONTENT(request, content, contentLen);
    WRITE_LOCK_HANDLE(request);
    handleObject<HandleType::GenPrimaryMortgageRatePathsRequest>(request).secondaryRatePaths_ =
        MCMortgageRatesReader<wfmcm::MortgageRateType>::readFromCsv({ content, (size_t)contentLen });
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setRequiredSecondaryRates(
    Handle request,
    const ApiSecondaryRateType* requiredSecondaryRateTypes,
    int numSecondaryRateTypes)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, HandleType::GenSecondaryMortgageRatePathsRequest,
        HandleType::GenRatePathsForMortgageValuationRequest);
    CHECK_ARRAY(request, requiredSecondaryRateTypes, numSecondaryRateTypes)
    WRITE_LOCK_HANDLE(request);
    std::pmr::vector<MortgageRateType>* requiredRates = nullptr;
    switch (handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        requiredRates = &handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(request).requiredSecondaryRateTypes_;
        break;
    case HandleType::GenSecondaryMortgageRatePathsRequest:
        requiredRates = &handleObject<HandleType::GenSecondaryMortgageRatePathsRequest>(request).requiredRateTypes_;
        break;
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    requiredRates->clear();
    requiredRates->reserve(numSecondaryRateTypes);
    for (int i = 0; i < numSecondaryRateTypes; ++i) {
        requiredRates->push_back(static_cast<MortgageRateType>(requiredSecondaryRateTypes[i]));
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setSimulationMonths(
    Handle request,
    int simMonths)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_SIZE(request, simMonths);
    WRITE_LOCK_HANDLE(request);

    switch (handleData(request).getType())
    {
    case HandleType::GeneratePathForWaterfallAttributionRequest:
        handleObject<HandleType::GeneratePathForWaterfallAttributionRequest>(request).simulationMonths_ = simMonths;
        break;
    case HandleType::GenRatePathsForMortgageValuationRequest:
        handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(request).simulationMonths_ = simMonths;
        break;
    case HandleType::CalcValueForMonthEndRollRequest:
        handleObject<HandleType::CalcValueForMonthEndRollRequest>(request).simulationMonths_ = simMonths;
        break;
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request)
}
/*-----------------------------------------------------------------------------------------
                                  VALUE MORTGAGE REQUEST
-------------------------------------------------------------------------------------------*/
int setValueMortgageRequestRecalibratedBasis(Handle request, bool value)
{
    CHECK_INIT;
    return ApiSuccess;
}

int setValueMortgageRequestUseBaseOAS(Handle request, bool value)
{
    CHECK_INIT;
    return ApiSuccess;
}

int setValueMortgageRequestDebugInfo(
    Handle request,
    const ApiDebugInfoType* debugInfoTypes,
    int numDebugInfoTypes)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, HandleType::CalcValueForMortgageRequest);
    CHECK_ARRAY(request, debugInfoTypes, numDebugInfoTypes)
    WRITE_LOCK_HANDLE(request);
    auto& debugInfo = handleObject<HandleType::CalcValueForMortgageRequest>(request).debugInfo_;
    debugInfo.emplace();
    for (int i = 0; i < numDebugInfoTypes; ++i) {
        debugInfo.value().insert(static_cast<DebugInfoType>(debugInfoTypes[i]));
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);

}

int setValueMortgageFromRatesRequestDebugInfo(
    Handle request,
    const ApiDebugInfoType* debugInfoTypes,
    int numDebugInfoTypes)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, HandleType::CalcValueForMortgageFromRatesRequest);
    CHECK_ARRAY(request, debugInfoTypes, numDebugInfoTypes)
        WRITE_LOCK_HANDLE(request);
    auto& debugInfo = handleObject<HandleType::CalcValueForMortgageFromRatesRequest>(request).debugInfo_;
    debugInfo.emplace();
    for (int i = 0; i < numDebugInfoTypes; ++i) {
        debugInfo.value().insert(static_cast<DebugInfoType>(debugInfoTypes[i]));
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setIndexRateMapping(
    Handle request,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES)
    CHECK_CONTENT(request, content, contentLen); //validates null content and normalizes -1 length
    WRITE_LOCK_HANDLE(request)
        switch (handleData(request).getType())
        {
        case HandleType::CalcValueForMortgageRequest:
            handleObject<HandleType::CalcValueForMortgageRequest>(request).indexMapper_ =
                IndexMapperReader::readFromCsv({ content, (size_t)contentLen });
            break;
        case HandleType::CalcValueForMortgageFromRatesRequest:
            handleObject<HandleType::CalcValueForMortgageFromRatesRequest>(request).indexMapper_ =
                IndexMapperReader::readFromCsv({ content, (size_t)contentLen });
            break;

        default:
            RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
        }
        

    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                      DATES SPECIFICATION
-------------------------------------------------------------------------------------------*/
Handle createDateSpec()
{
    TRY

    CHECK_INIT2;
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::DateSpec, HandleObjectType<HandleType::DateSpec>{}));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int setMarketCurveDate(Handle dateSpec, Date date)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_DATE(dateSpec, date, Market);
    WRITE_LOCK_HANDLE(dateSpec);
    handleObject<HandleType::DateSpec>(dateSpec).setMarketCurveDate(QuantLib::fromYYYYMMDD(date.yyyymmdd_));
    RETURN_API_SUCCESS(dateSpec);

    EXCEPTION_API_ERROR(dateSpec);
}

int setMarketVolatilityInputDate(Handle dateSpec, Date date)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_DATE(dateSpec, date, Market);
    WRITE_LOCK_HANDLE(dateSpec);
    handleObject<HandleType::DateSpec>(dateSpec).setMarketVolDate(QuantLib::fromYYYYMMDD(date.yyyymmdd_));
    RETURN_API_SUCCESS(dateSpec);

    EXCEPTION_API_ERROR(dateSpec);
}

int setValuationDate(Handle dateSpec, Date date)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_DATE(dateSpec, date, Valuation);
    WRITE_LOCK_HANDLE(dateSpec);
    handleObject<HandleType::DateSpec>(dateSpec).setValuationDate(QuantLib::fromYYYYMMDD(date.yyyymmdd_));
    RETURN_API_SUCCESS(dateSpec);

    EXCEPTION_API_ERROR(dateSpec);
}

int setFactorDate(Handle dateSpec, Date date)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_DATE(dateSpec, date, Valuation);
    WRITE_LOCK_HANDLE(dateSpec);
    handleObject<HandleType::DateSpec>(dateSpec).setFactorDate(std::chrono::year_month(QuantLib::fromYYYYMMDD(date.yyyymmdd_)));
    RETURN_API_SUCCESS(dateSpec);

    EXCEPTION_API_ERROR(dateSpec);
}

int setSecondaryRateAsOfDate(Handle dateSpec, Date date)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_DATE(dateSpec, date, Rate asOf);
    WRITE_LOCK_HANDLE(dateSpec);
    handleObject<HandleType::DateSpec>(dateSpec).setSecRateAsOfDate(QuantLib::fromYYYYMMDD(date.yyyymmdd_));
    RETURN_API_SUCCESS(dateSpec);

    EXCEPTION_API_ERROR(dateSpec);
}

int setHpiAsOfDate(Handle dateSpec, Date date)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_DATE(dateSpec, date, Hpi asOf);
    WRITE_LOCK_HANDLE(dateSpec);
    handleObject<HandleType::DateSpec>(dateSpec).setHpiAsOfDate(QuantLib::fromYYYYMMDD(date.yyyymmdd_));
    RETURN_API_SUCCESS(dateSpec);

    EXCEPTION_API_ERROR(dateSpec);
}

int setPrimaryRateAsOfDate(Handle dateSpec, Date date)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_DATE(dateSpec, date, Primary rate asOf);
    WRITE_LOCK_HANDLE(dateSpec);
    handleObject<HandleType::DateSpec>(dateSpec).setPrimRateAsOfDate(QuantLib::fromYYYYMMDD(date.yyyymmdd_));
    RETURN_API_SUCCESS(dateSpec);

    EXCEPTION_API_ERROR(dateSpec);
}

int setPrimaryRateRefDate(Handle dateSpec, Date date)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_DATE(dateSpec, date, Primary rate reference);
    WRITE_LOCK_HANDLE(dateSpec);
    handleObject<HandleType::DateSpec>(dateSpec).setPrimRateAsOfDate(QuantLib::fromYYYYMMDD(date.yyyymmdd_));
    RETURN_API_SUCCESS(dateSpec);

    EXCEPTION_API_ERROR(dateSpec);
}

int setSecondaryRateRefDate(Handle dateSpec, Date date)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_DATE(dateSpec, date, Secondary rate reference);
    WRITE_LOCK_HANDLE(dateSpec);
    handleObject<HandleType::DateSpec>(dateSpec).setSecRateAsOfDate(QuantLib::fromYYYYMMDD(date.yyyymmdd_));
    RETURN_API_SUCCESS(dateSpec);

    EXCEPTION_API_ERROR(dateSpec);
}

int bindDateSpec(Handle request, Handle dateSpecOrDateSpecArray)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES)
    CHECK_HANDLE(dateSpecOrDateSpecArray, HandleType::DateSpec, HandleType::HandleArray)
    Handle dateSpec = handleAtIndex(dateSpecOrDateSpecArray, 0);
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    WRITE_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(dateSpecOrDateSpecArray)

    switch (handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::CalcValueForMortgageRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageWithSofrRatesRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError( ApiErrorInvalidDate, "Valuation date not set" );
                }
                handleObject<HandleType::CalcValueForMortgageWithSofrRatesRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;
    case HandleType::AttribValueChangeByWaterfallRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::AttribValueChangeByWaterfallRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;
    case HandleType::GenPrimaryMortgageRatePathsRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::GenPrimaryMortgageRatePathsRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcGreeksFromPricesRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::CalcGreeksFromPricesRequest>(to).valuationDate_ = dateSpec.getValuationDate();
                return {};
            });
        break;
    case HandleType::GenSecondaryMortgageRatePathsRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::GenSecondaryMortgageRatePathsRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMonthEndRollRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::CalcValueForMonthEndRollRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;
    case HandleType::GeneratePathForWaterfallAttributionRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::GeneratePathForWaterfallAttributionRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcBehavioralSpeedFromPrimaryRateRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::CalcBehavioralSpeedFromPrimaryRateRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageFromRatesRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::CalcValueForMortgageFromRatesRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcProfitabilityFromBehavioralSpeedsRequest:
        BIND_HANDLES(request, dateSpec, HandleBinding::DateSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                const auto& dateSpec = handleObject<HandleType::DateSpec>(from);
                if (dateSpec.getValuationDate().serialNumber() == 0) {
                    return HandleError(ApiErrorInvalidDate, "Valuation date not set");
                }
                handleObject<HandleType::CalcProfitabilityFromBehavioralSpeedsRequest>(to).dateSpec_ =
                    handleObject<HandleType::DateSpec>(from);
                return {};
            });
        break;

    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }

    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                    HISTORICAL DATA
-------------------------------------------------------------------------------------------*/
Handle createHistoricalData()
{
    CHECK_INIT2;
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::HistoricalData,
        HandleObjectType<HandleType::HistoricalData>{}));
}

int setHistoricalData(
    Handle hist,
    ApiHistoricalData dataType,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(hist, HandleType::HistoricalData);
    CHECK_CONTENT(hist, content, contentLen);
    WRITE_LOCK_HANDLE(hist);

    switch (dataType)
    {
        case ApiHistoricalData_Hpi:
        {
            handleObject<HandleType::HistoricalData>(hist).hpi_ = BehavioralModelTableReader<State>::readFromCsv(
                { content, (size_t)contentLen },
                BehavioralModelTableConverter::HpiToState,
                BehavioralModelTableConverter::getMissingState,
                State::V_ZZ);
            break;
        }
        case ApiHistoricalData_Unemployment:
        {
            handleObject<HandleType::HistoricalData>(hist).unemployment_ = BehavioralModelTableReader<State>::readFromCsv(
                { content, (size_t)contentLen },
                BehavioralModelTableConverter::HpiToState,
                BehavioralModelTableConverter::getMissingState,
                State::V_ZZ);
            break;
        }
        case ApiHistoricalData_EconomicScenario:
        {
            HistRateTsMap<std::string> rates = TimeSeriesReader::readFromCsv({ content, (size_t)contentLen });
            value_of(handleObject<HandleType::HistoricalData>(hist).economicScenario_)
                .insert(make_move_iterator(rates.begin()), make_move_iterator(rates.end()));
            break;
        }
        case ApiHistoricalData_MbsPrimaryMortgageRates:
        {
            HistRateTsMap<std::string> rates = TimeSeriesReader::readFromCsv({ content, (size_t)contentLen });
            value_of(handleObject<HandleType::HistoricalData>(hist).mbsPrimaryRates_)
                .insert(make_move_iterator(rates.begin()), make_move_iterator(rates.end()));
            break;
        }
        case ApiHistoricalData_SecondaryMortgageRates:
        {
            HistRateTsMap<std::string> rates = TimeSeriesReader::readFromCsv({ content, (size_t)contentLen });
            auto convertedRates = convertTimeSeriesWithKey<MortgageRateType>(std::move(rates));
            value_of(handleObject<HandleType::HistoricalData>(hist).secondaryRates_)
                .insert(make_move_iterator(convertedRates.begin()), make_move_iterator(convertedRates.end()));
            break;
        }
        case ApiHistoricalData_SofrDailyRates:
        {
            std::pair<std::pmr::vector<QuantLib::Date>, std::pmr::vector<double>> sofrHistory =
                RatesReader::readFromCsv({ content, (size_t)contentLen });
            auto sofrHistoryLookup = lookup_builder<double, ExtendedVectorKey<QuantLib::Date>>()
                .withKey<0>(std::move(sofrHistory.first))
                .withValue(std::move(sofrHistory.second))
                .build();
            handleObject<HandleType::HistoricalData>(hist).historyDailySofr_ = std::move(sofrHistoryLookup);
            break;
        }
        case ApiHistoricalData_GFee:
        {
            auto gfee = ExtendedDateKeyLookupReader<GFeeType>::readFromCsv({ content, (size_t)contentLen });
            handleObject<HandleType::HistoricalData>(hist).gfee_ = std::move(gfee);
            break;
        }
        default:
            RETURN_API_ERROR(hist, ApiErrorInvalidHistoricalData, "Unsupported historical data type");
    };
    RETURN_API_SUCCESS(hist);

    EXCEPTION_API_ERROR(hist);
}

int bindHistoricalData(Handle request, Handle historicalData)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES)
    CHECK_HANDLE(historicalData, HandleType::HistoricalData);
    WRITE_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(historicalData)

    switch (handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        BIND_HANDLES(request, historicalData, HandleBinding::HistoricalData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(to);
                const auto& histData = handleObject<HandleType::HistoricalData>(from);
                return fillHistoricalData(to, histData, &request.historicalData_);
            });
        break;
    case HandleType::CalcValueForMortgageRequest:
        BIND_HANDLES(request, historicalData, HandleBinding::HistoricalData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::CalcValueForMortgageRequest>(to);
                const auto& histData = handleObject<HandleType::HistoricalData>(from);
                return fillHistoricalData(to, histData, &request.historicalData_);
            });
        break;
    case HandleType::CalcValueForMortgageWithSofrRatesRequest:
        BIND_HANDLES(request, historicalData, HandleBinding::HistoricalData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::CalcValueForMortgageWithSofrRatesRequest>(to);
                const auto& histData = handleObject<HandleType::HistoricalData>(from);
                return fillHistoricalData(to, histData, &request.historicalData_);
            });
        break;
    case HandleType::AttribValueChangeByWaterfallRequest:
        BIND_HANDLES(request, historicalData, HandleBinding::HistoricalData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::AttribValueChangeByWaterfallRequest>(to);
                const auto& histData = handleObject<HandleType::HistoricalData>(from);
                return fillHistoricalData(to, histData, &request.historicalData_);
            });
        break;
    case HandleType::GenPrimaryMortgageRatePathsRequest:
        BIND_HANDLES(request, historicalData, HandleBinding::HistoricalData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::GenPrimaryMortgageRatePathsRequest>(to);
                const auto& histData = handleObject<HandleType::HistoricalData>(from);
                return fillHistoricalData(to, histData, &request.historicalData_);
            });
        break;
    case HandleType::GenSecondaryMortgageRatePathsRequest:
        BIND_HANDLES(request, historicalData, HandleBinding::HistoricalData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::GenSecondaryMortgageRatePathsRequest>(to);
                const auto& histData = handleObject<HandleType::HistoricalData>(from);
                return fillHistoricalData(to, histData, &request.historicalData_);
            });
        break;
    case HandleType::CalcValueForMonthEndRollRequest:
        BIND_HANDLES(request, historicalData, HandleBinding::HistoricalData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::CalcValueForMonthEndRollRequest>(to);
                const auto& histData = handleObject<HandleType::HistoricalData>(from);
                return fillHistoricalData(to, histData, &request.historicalData_);
            });
        break;
    case HandleType::GeneratePathForWaterfallAttributionRequest:
        BIND_HANDLES(request, historicalData, HandleBinding::HistoricalData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::GeneratePathForWaterfallAttributionRequest>(to);
                const auto& histData = handleObject<HandleType::HistoricalData>(from);
                return fillHistoricalData(to, histData, &request.historicalData_);
            });
        break;
    case HandleType::CalcBehavioralSpeedFromPrimaryRateRequest:
        BIND_HANDLES(request, historicalData, HandleBinding::HistoricalData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::CalcBehavioralSpeedFromPrimaryRateRequest>(to);
                const auto& histData = handleObject<HandleType::HistoricalData>(from);
                return fillHistoricalData(to, histData, &request.historicalData_);
            });
        break;
    case HandleType::CalcValueForMortgageFromRatesRequest:
        BIND_HANDLES(request, historicalData, HandleBinding::HistoricalData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::CalcValueForMortgageFromRatesRequest>(to);
                const auto& histData = handleObject<HandleType::HistoricalData>(from);
                return fillHistoricalData(to, histData, &request.historicalData_);
            });
        break;

    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                    RATES DATA
-------------------------------------------------------------------------------------------*/
Handle createRatesData()
{
    CHECK_INIT2;
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::RatesData,
        HandleObjectType<HandleType::RatesData>{}));
}

int setRatesData(
    Handle rates,
    ApiRatesData dataType,
    const char* content,
    int contentLen)
{
        TRY

        CHECK_INIT;
        CHECK_HANDLE(rates, HandleType::RatesData);
        CHECK_CONTENT(rates, content, contentLen);
        WRITE_LOCK_HANDLE(rates);

        RatePathsSet<PrimaryRateType> ratePaths;
        RatePathsSet<KeyRate> keyRatePaths;

        switch (dataType)
        {
        case ApiRatesData_PrimaryRates:
        {
            string ratesFile = content;
            auto jsonData = PolyvarReader::readFromJsonFile(ratesFile);
            extractPrimaryRateAndKeyRatePathsFromPolyvar(ratePaths, keyRatePaths, jsonData);
            handleObject<HandleType::RatesData>(rates).ratePaths_ = std::move(ratePaths);
            handleObject<HandleType::RatesData>(rates).keyRatePaths_= std::move(keyRatePaths);
            break;
        }
        default:
            RETURN_API_ERROR(rates, ApiErrorInvalidRatesFiles, "Unsupported Rates data type");
        };
        RETURN_API_SUCCESS(rates);

        EXCEPTION_API_ERROR(rates);
}


int bindRatesData(Handle request, Handle ratesData)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES)
    CHECK_HANDLE(ratesData, HandleType::RatesData);
    WRITE_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(ratesData)

    switch (handleData(request).getType())
    {
    case HandleType::CalcValueForMortgageFromRatesRequest:
        BIND_HANDLES(request, ratesData, HandleBinding::RatesData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& opt = handleObject<HandleType::CalcValueForMortgageFromRatesRequest>(to);
                const auto& ratesData = handleObject<HandleType::RatesData>(from);
                //Copy (do NOT move): the setter runs under a READ lock on 'from' during
                //fill(), and moving emptied the shared RatesData handle so subsequent
                //fills/requests bound to it silently received no rates.
                opt.keyRatePaths_ = ratesData.keyRatePaths_;
                opt.ratePaths_ = ratesData.ratePaths_;
                return {};
            });
        break;

    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRatesFiles, "Request does not support this operation");
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}


/*-----------------------------------------------------------------------------------------
                                      MARKET DATA
-------------------------------------------------------------------------------------------*/
Handle createMarketData(Date marketDate)
{
    TRY

    CHECK_INIT2;
    CHECK_DATE2(NullHandle, marketDate, Market);
    HandleObjectType<HandleType::MarketData> marketData = { .marketDate_ = QuantLib::fromYYYYMMDD(marketDate.yyyymmdd_) };
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::MarketData,
        move(marketData)));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int setMarketRawCurveInput(
    Handle market,
    Handle dateSpec,
    ApiCurveInputFormat inputType,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_HANDLE(market, HandleType::MarketData);
    CHECK_CONTENT(market, content, contentLen);
    WRITE_LOCK_HANDLE(market);
    const auto& date = handleObject<HandleType::DateSpec>(dateSpec).getMarketCurveDate();
    switch (inputType) {
        case ApiCurveInputFormat_DiscountFactors:
            handleObject<HandleType::MarketData>(market).rawCurveInput_ =
                DiscountFactorsInputReader::readFromJson({ content, (size_t)contentLen }, date);
            break;
        case ApiCurveInputFormat_ForwardRates:
            handleObject<HandleType::MarketData>(market).rawCurveInput_ =
                ForwardRatesInputReader::readFromJson({ content, (size_t)contentLen }, date);
            break;
        case ApiCurveInputFormat_SofrCurve:
            handleObject<HandleType::MarketData>(market).rawCurveInput_ =
                SofrSwapCurveInputReader::readFromJson({ content, (size_t)contentLen }, date);
            break;
        default:
            auto errStr = fmt::format("Invalid curve input: {}", (int)inputType);
            RETURN_API_ERROR(market, ApiErrorInvalidCurveInput, errStr);
    }
    RETURN_API_SUCCESS(market);

    EXCEPTION_API_ERROR(market);
}

int setMarketUSTreasuryCurveInput(
    Handle market,
    Handle dateSpec,
    ApiCurveInputFormat inputType,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_HANDLE(market, HandleType::MarketData);
    CHECK_CONTENT(market, content, contentLen);
    WRITE_LOCK_HANDLE(market);
    const auto& date = handleObject<HandleType::DateSpec>(dateSpec).getMarketCurveDate();
    switch (inputType) {
    case ApiCurveInputFormat_DiscountFactors:
        handleObject<HandleType::MarketData>(market).ustCurveInput_ =
            DiscountFactorsInputReader::readFromJson({ content, (size_t)contentLen }, date);
        break;
    case ApiCurveInputFormat_ForwardRates:
        handleObject<HandleType::MarketData>(market).ustCurveInput_ =
            ForwardRatesInputReader::readFromJson({ content, (size_t)contentLen }, date);
        break;
    case ApiCurveInputFormat_LiborCurve:
        handleObject<HandleType::MarketData>(market).ustCurveInput_ =
            LiborSwapCurveInputReader::readFromJson({ content, (size_t)contentLen }, date);
        break;
    default:
        auto errStr = fmt::format("Invalid US Treasury curve input: {}", (int)inputType);
        RETURN_API_ERROR(market, ApiErrorInvalidCurveInput, errStr);
    }
    RETURN_API_SUCCESS(market);

    EXCEPTION_API_ERROR(market);
}

int setMarketSwaptionVolatilityInput(
    Handle market,
    Handle dateSpec,
    bool atTheMoney,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT;

    CHECK_HANDLE(dateSpec, HandleType::DateSpec);
    CHECK_HANDLE(market, HandleType::MarketData);
    CHECK_CONTENT(market, content, contentLen);
    WRITE_LOCK_HANDLE(market);
    const auto& date = handleObject<HandleType::DateSpec>(dateSpec).getMarketVolDate();
    auto rawVolInput = VolatilityStructureDataReader::readFromJson(
    	{ content, (size_t)contentLen }, date, "normal_vol_cube", atTheMoney);
    handleObject<HandleType::MarketData>(market).sofrVolInput_ = move(rawVolInput);

    RETURN_API_SUCCESS(market);

    EXCEPTION_API_ERROR(market);
}

int setMarketSecondaryRates(
    Handle market,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(market, HandleType::MarketData);
    CHECK_CONTENT(market, content, contentLen);
    WRITE_LOCK_HANDLE(market);
    handleObject<HandleType::MarketData>(market).secondaryRates_ =
    	MortgageIndexReader::readFromText({ content, (size_t)contentLen });
    RETURN_API_SUCCESS(market);

    EXCEPTION_API_ERROR(market);
}

int setMarketPrimaryRates(
    Handle market,
    const char* content,
    int contentLen)
{
    static const char delimiters[2] = { '\t', ',' };

    TRY

    CHECK_INIT;
    CHECK_HANDLE(market, HandleType::MarketData);
    CHECK_CONTENT(market, content, contentLen);

    HistoricalIndexProviderBuilder<PrimaryRateType> primaryRates0Builder;
    const char* const contentEnd = content + contentLen;
    auto it = std::find_first_of(content, contentEnd, delimiters, delimiters + 2);
    if (it == contentEnd) {
        //Neither delimiter present: previously *it dereferenced one-past-the-end (UB).
        RETURN_API_ERROR(market, ApiErrorInvalidFormat,
            "Primary rates content is neither tab- nor comma-delimited");
    }
    HistoricalPrimaryRateReaderOutput out = (*it == '\t')
        ? HistoricalPrimaryRateReader::readFromText({ content, (size_t)contentLen })
        : HistoricalPrimaryRateReader::readFromCsv({ content, (size_t)contentLen });
    for (auto&& [key, rate] : out) {
        primaryRates0Builder.addHistory(move(key), move(rate));
    }

    WRITE_LOCK_HANDLE(market);
    handleObject<HandleType::MarketData>(market).primaryRates_ = primaryRates0Builder.build();
    RETURN_API_SUCCESS(market);

    EXCEPTION_API_ERROR(market);
}

bool validateMarketDateSpec(Handle request, Handle mkt)
{
    //Make sure the date spec is the same as the market data
    Handle dateSpecHandle = getBoundHandle(request, HandleBinding::DateSpec);
    return (handleObject<HandleType::MarketData>(mkt).marketDate_ ==
        handleObject<HandleType::DateSpec>(dateSpecHandle).getValuationDate());
}

bool validateEndMarket(Handle startMkt, Handle endMkt)
{
    return (handleObject<HandleType::MarketData>(startMkt).marketDate_ <
        handleObject<HandleType::MarketData>(endMkt).marketDate_);
}

int bindMarketData(Handle request, Handle marketDataOrMarketDataArray)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES)
    CHECK_HANDLE(marketDataOrMarketDataArray, HandleType::MarketData, HandleType::HandleArray)
    WRITE_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(marketDataOrMarketDataArray)
    Handle marketData = handleAtIndex(marketDataOrMarketDataArray, 0);
    CHECK_HANDLE(marketData, HandleType::MarketData);

    switch (handleData(request).getType())
    {
    case HandleType::CalcValueForMortgageRequest:
        BIND_HANDLES(request, marketData, HandleBinding::MarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::CalcValueForMortgageRequest>(to);
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.marketData_);
            });
        break;
    case HandleType::CalcValueForMortgageWithSofrRatesRequest:
        BIND_HANDLES(request, marketData, HandleBinding::MarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::CalcValueForMortgageWithSofrRatesRequest>(to);
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.marketData_);
            });
        break;
    case HandleType::GenRatePathsForMortgageValuationRequest:
        BIND_HANDLES(request, marketData, HandleBinding::MarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(to);
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.marketData_);
            });
        break;
    case HandleType::GenPrimaryMortgageRatePathsRequest:
        BIND_HANDLES(request, marketData, HandleBinding::MarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::GenPrimaryMortgageRatePathsRequest>(to);
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.marketData_);
            });
        break;
    case HandleType::GenSecondaryMortgageRatePathsRequest:
        BIND_HANDLES(request, marketData, HandleBinding::MarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::GenSecondaryMortgageRatePathsRequest>(to);
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.marketData_);
            });
        break;
    case HandleType::AttribValueChangeByWaterfallRequest:
    {
        CHECK_HANDLE(marketDataOrMarketDataArray, HandleType::HandleArray) //only arrays allowed
        //ensure size is >= 2
        if (handleObject<HandleType::HandleArray>(marketDataOrMarketDataArray).size() < 2) {
            RETURN_API_ERROR(request, ApiError::ApiErrorInvalidLength, "Market data array size must be at least 2");
        }
        Handle startMarketData = handleAtIndex(marketDataOrMarketDataArray, 0);
        CHECK_HANDLE(startMarketData, HandleType::MarketData);
        Handle endMarketData = handleAtIndex(marketDataOrMarketDataArray, 1);
        CHECK_HANDLE(endMarketData, HandleType::MarketData);

        BIND_HANDLES(request, startMarketData, HandleBinding::MarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                //check to see if the start market was constructed with the same date spec as the request
                auto& request = handleObject<HandleType::AttribValueChangeByWaterfallRequest>(to);
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.baseMarketData_);
            });
        BIND_HANDLES(request, endMarketData, HandleBinding::EndMarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::AttribValueChangeByWaterfallRequest>(to);
                auto endMarketData = from;
                auto startMarketData = handleData(to).getBoundHandle(static_cast<int>(HandleBinding::MarketData));
                if (!validateEndMarket(startMarketData, endMarketData)) {
                    return HandleError(ApiErrorInvalidDate, "End market validation date must be > base market validation date");
                }
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.endMarketData_);
            });
        break;
    }
    case HandleType::CalcValueForMonthEndRollRequest:
    {
        CHECK_HANDLE(marketDataOrMarketDataArray, HandleType::HandleArray) //only arrays allowed
        //ensure size is >= 2
        if (handleObject<HandleType::HandleArray>(marketDataOrMarketDataArray).size() < 2) {
            RETURN_API_ERROR(request, ApiError::ApiErrorInvalidLength, "Market data array size must be at least 2");
        }
        Handle startMarketData = handleAtIndex(marketDataOrMarketDataArray, 0);
        CHECK_HANDLE(startMarketData, HandleType::MarketData);
        Handle endMarketData = handleAtIndex(marketDataOrMarketDataArray, 1);
        CHECK_HANDLE(endMarketData, HandleType::MarketData);

        BIND_HANDLES(request, startMarketData, HandleBinding::MarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::CalcValueForMonthEndRollRequest>(to);
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.baseMarketData_);
            });
        BIND_HANDLES(request, endMarketData, HandleBinding::EndMarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::CalcValueForMonthEndRollRequest>(to);
                auto endMarketData = from;
                auto startMarketData = handleData(to).getBoundHandle(static_cast<int>(HandleBinding::MarketData));
                if (!validateEndMarket(startMarketData, endMarketData)) {
                    return HandleError(ApiErrorInvalidDate, "End market validation date must be > base market validation date");
                }
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.endMarketData_);
            });
        break;
    }
    case HandleType::GeneratePathForWaterfallAttributionRequest:
    {
        CHECK_HANDLE(marketDataOrMarketDataArray, HandleType::HandleArray) //only arrays allowed
        //ensure size is >= 2
        if (handleObject<HandleType::HandleArray>(marketDataOrMarketDataArray).size() < 2) {
            RETURN_API_ERROR(request, ApiError::ApiErrorInvalidLength, "Market data array size must be at least 2");
        }
        Handle startMarketData = handleAtIndex(marketDataOrMarketDataArray, 0);
        CHECK_HANDLE(startMarketData, HandleType::MarketData);
        Handle endMarketData = handleAtIndex(marketDataOrMarketDataArray, 1);
        CHECK_HANDLE(endMarketData, HandleType::MarketData);

        BIND_HANDLES(request, startMarketData, HandleBinding::MarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                //check to see if the start market was constructed with the same date spec as the request
                auto& request = handleObject<HandleType::GeneratePathForWaterfallAttributionRequest>(to);
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.baseMarketData_);
            });
        BIND_HANDLES(request, endMarketData, HandleBinding::EndMarketData,
            [](Handle to, Handle from, int setterId)->HandleError {
                auto& request = handleObject<HandleType::GeneratePathForWaterfallAttributionRequest>(to);
                auto endMarketData = from;
                auto startMarketData = handleData(to).getBoundHandle(static_cast<int>(HandleBinding::MarketData));
                if (!validateEndMarket(startMarketData, endMarketData)) {
                    return HandleError(ApiErrorInvalidDate, "End market validation date must be > base market validation date");
                }
                return fillMarketData(to, handleObject<HandleType::MarketData>(from), &request.endMarketData_);
            });
        break;
    }
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                      VOLATILITY BASKET
-------------------------------------------------------------------------------------------*/
Handle createVolatilityBasket(
    const int* tenors, int numTenors,
    const int* expiries, int numExpiries)
{
    TRY

    CHECK_INIT2;
    CHECK_ARRAY2(NullHandle, tenors, numTenors)
    CHECK_ARRAY2(NullHandle, expiries, numExpiries)
    if (numTenors == 0) {
        RETURN_ERROR(NullHandle, ApiErrorEmptyTenors, "Number of tenors must be > 0", NullHandle);
    }
    if (numExpiries == 0) {
        RETURN_ERROR(NullHandle, ApiErrorEmptyExpiries, "Number of expiries must be > 0", NullHandle);
    }
    HandleObjectType<HandleType::VolatilityBasket> volBasket;
    volBasket.tenors_.assign(tenors, tenors + numTenors);
    volBasket.expiries_.assign(expiries, expiries + numExpiries);
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::VolatilityBasket,
        move(volBasket)));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int bindVolatilityBasket(
    Handle request,
    Handle volBasket)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, HandleType::CalcGreeksFromPricesRequest);
    CHECK_HANDLE(volBasket, HandleType::VolatilityBasket);
    WRITE_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(volBasket)
    BIND_HANDLES(request, volBasket, HandleBinding::CalibrationBasket,
        [](Handle to, Handle from, int setterId)->HandleError {
            handleObject<HandleType::CalcGreeksFromPricesRequest>(to).calibrationBasket_ =
                handleObject<HandleType::VolatilityBasket>(from);
            return {};
        });

    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}
/*-----------------------------------------------------------------------------------------
                                      MODELS & SESSIONS
-------------------------------------------------------------------------------------------*/
Handle createModelSpec(ApiModel modelType)
{
    TRY

    CHECK_INIT2;
    switch (modelType) {
        case ApiModel_CurveBuilder:
        {
            HandleObjectType<HandleType::CurveModificationSpec> curveModSpec;
            return handles->addHandleData(make_shared<HandleData>(
                HandleType::CurveModificationSpec,
                move(curveModSpec)));
        }
        case ApiModel_CurveInterpolation:
        {
            HandleObjectType<HandleType::CurveInterpolationSpec> curveInterpSpec;
            return handles->addHandleData(make_shared<HandleData>(
                HandleType::CurveInterpolationSpec,
                move(curveInterpSpec)));
        }
        default:
        {
            HandleObjectType<HandleType::ModelSpec> specDetails;
            specDetails.modelType_ = modelType;
            return handles->addHandleData(make_shared<HandleData>(
                HandleType::ModelSpec,
                move(specDetails)));
        }
    } //switch

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int setModelParameter(
    Handle modelSpec,
    ApiModelParameter param,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(modelSpec, HandleType::ModelSpec)
    CHECK_CONTENT(modelSpec, content, contentLen);
    WRITE_LOCK_HANDLE(modelSpec);
    auto& spec = handleObject<HandleType::ModelSpec>(modelSpec);
    auto paramSet = supportedSpecParams(spec.modelType_, true);
    if (paramSet.find(to_string(param)) == paramSet.end()) {
        RETURN_API_ERROR(modelSpec, ApiErrorInvalidModelParameter, "Model does not support this parameter");
    }
    spec.settings_.modelSpec_[static_cast<ModelParamName>(param)] = { content, (size_t)contentLen };
    RETURN_API_SUCCESS(modelSpec);

    EXCEPTION_API_ERROR(modelSpec);
}

Handle createSessionSpec(ApiModel modelType)
{
    TRY

    CHECK_INIT2;

    switch (modelType) {
        case ApiModel_CurveBuilder:
        {
            RETURN_ERROR(NullHandle, ApiErrorInvalidModelType, "Curve builder does not support a session spec", NullHandle);
        }
        case ApiModel_CurveInterpolation:
        {
            RETURN_ERROR(NullHandle, ApiErrorInvalidModelType, "Curve interpolation does not support a session spec", NullHandle);
        }
        default:
        {
            HandleObjectType<HandleType::SessionSpec> specDetails;
            specDetails.modelType_ = modelType;
            return handles->addHandleData(make_shared<HandleData>(
                HandleType::SessionSpec,
                move(specDetails)));
        }
    } //switch

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int setSessionParameter(
    Handle sessionSpec,
    ApiSessionParameter param,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(sessionSpec, HandleType::SessionSpec);
    CHECK_CONTENT(sessionSpec, content, contentLen);
    WRITE_LOCK_HANDLE(sessionSpec);
    auto& spec = handleObject<HandleType::SessionSpec>(sessionSpec);
    auto paramSet = supportedSpecParams(spec.modelType_, false);
    if (paramSet.find(to_string(param)) == paramSet.end()) {
        RETURN_API_ERROR(sessionSpec, ApiErrorInvalidSessionParameter, "Session does not support this parameter");
    }
    spec.settings_.sessionSpec_[static_cast<SessionParamName>(param)] = { content, (size_t)contentLen };
    RETURN_API_SUCCESS(sessionSpec);

    EXCEPTION_API_ERROR(sessionSpec);
}

int bindModelSession(Handle modelSpec, Handle sessionSpec)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(modelSpec, HandleType::ModelSpec);
    CHECK_HANDLE(sessionSpec, HandleType::SessionSpec);
    WRITE_LOCK_HANDLE(modelSpec)
    READ_LOCK_HANDLE(sessionSpec)
    if (handleObject<HandleType::ModelSpec>(modelSpec).modelType_ !=
        handleObject<HandleType::ModelSpec>(sessionSpec).modelType_) {
        RETURN_API_ERROR(modelSpec, ApiErrorMismatchedModelType, "Model and session types don't match");
    }
    BIND_HANDLES(modelSpec, sessionSpec, HandleBinding::ModelSession,
        [](Handle to, Handle from, int setterId)->HandleError {
            // copy the items in session spec in from object and update in object
            for (const auto& [key, value] : handleObject<HandleType::SessionSpec>(from).settings_.sessionSpec_) {
                handleObject<HandleType::ModelSpec>(to).settings_.sessionSpec_[key] = value;
            }
            return {};
        });

    RETURN_API_SUCCESS(modelSpec);

    EXCEPTION_API_ERROR(modelSpec);
}

const char* getSupportedModelParameters(ApiModel modelType)
{
    TRY

    if (!handles) {
	    RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", empty_value<string>.c_str());
    }
    auto& str = store<string, id::api_response>();
    polyvar p("parameters");
    p << supportedSpecParams(modelType, true);
    str = to_string(p);
    RETURN_SUCCESS(NullHandle, str.c_str());

    EXCEPTION_ERROR(NullHandle, empty_value<string>.c_str());
}

const char* getSupportedSessionParameters(ApiModel modelType)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", empty_value<string>.c_str());
    }
    auto& str = store<string, id::api_response>();
    polyvar p("parameters");
    p << supportedSpecParams(modelType, false);
    str = to_string(p);
    RETURN_SUCCESS(NullHandle, str.c_str());

    EXCEPTION_ERROR(NullHandle, empty_value<string>.c_str());
}

/*-----------------------------------------------------------------------------------------
                                      MODELS OPTIONS
-------------------------------------------------------------------------------------------*/
Handle createModelOptions()
{
    TRY

    CHECK_INIT2;
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::ModelOptions,
        HandleObjectType<HandleType::ModelOptions>{}));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int addModelOptionsSpec(
    Handle modelOptions,
    Handle modelSpec)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(modelOptions, HandleType::ModelOptions);
    CHECK_HANDLE(modelSpec, HandleType::ModelSpec, HandleType::CurveModificationSpec, HandleType::CurveInterpolationSpec);
    READ_LOCK_HANDLE(modelOptions)
    READ_LOCK_HANDLE(modelSpec)
    int setterId = static_cast<int>(HandleBinding::Unknown);
    if (handleData(modelSpec).getType() == HandleType::ModelSpec) {
        setterId = getModelOptionsBinding(handleObject<HandleType::ModelSpec>(modelSpec).modelType_);
    }
    else if(handleData(modelSpec).getType() == HandleType::CurveModificationSpec){
        setterId = static_cast<int>(HandleBinding::CurveModificationSpec) * ApiModel::ApiModel_UNKNOWN;
    }
    else {
        setterId = static_cast<int>(HandleBinding::CurveInterpolationSpec) * ApiModel::ApiModel_UNKNOWN;
    }
    if (setterId == static_cast<int>(HandleBinding::Unknown) * ApiModel::ApiModel_UNKNOWN) {
        RETURN_API_ERROR(modelOptions, ApiErrorInvalidModelType, "Could not perform spec binding: invalid model type");
    }
    BIND_HANDLES(modelOptions, modelSpec, setterId,
        [](Handle to, Handle from, int setterId)->HandleError {
            auto& modelOptions = handleObject<HandleType::ModelOptions>(to);
            msg::ModelSettings* modelSettings = nullptr;
            auto binding = getBoundHandle(setterId);
            //NOTE: we move the session spec since it gets re-filled each time from the session binding.
            switch (binding) {
                case HandleBinding::PrimaryRateModel:
                {
                    auto& specDetails = handleObject<HandleType::ModelSpec>(from);
                    auto modelType = static_cast<PrimaryMortgageRateModelType>(specDetails.modelType_);
                    auto& opt = modelOptions.primaryRateModelOptions_[modelType];
                    opt.modelSpec_ = specDetails.settings_.modelSpec_;
                    opt.sessionSpec_ = move(specDetails.settings_.sessionSpec_);
                    break;
                }
                case HandleBinding::SecondaryRateModel:
                {
                    auto& specDetails = handleObject<HandleType::ModelSpec>(from);
                    auto modelType = static_cast<SecondaryMortgageRateModelType>(specDetails.modelType_);
                    auto& opt = modelOptions.secondaryRateModelOptions_[modelType];
                    opt.modelSpec_ = specDetails.settings_.modelSpec_;
                    opt.sessionSpec_ = move(specDetails.settings_.sessionSpec_);
                    break;
                }
                case HandleBinding::PrepayModel:
                {
                    //{FIXME} case static_cast<int>(HandleBinding::PrepayModel)...((static_cast<int>(HandleBinding::LossModel) -1)): does not work
                    auto& specDetails = handleObject<HandleType::ModelSpec>(from);
                    auto modelType = static_cast<MortgageBehavioralModelType>(specDetails.modelType_);
                    auto& opt = modelOptions.prepayModelOptions_[modelType];
                    opt.modelSpec_ = specDetails.settings_.modelSpec_;
                    opt.sessionSpec_ = move(specDetails.settings_.sessionSpec_);
                    break;
                }
                case HandleBinding::LossModel:
                {
                    auto& specDetails = handleObject<HandleType::ModelSpec>(from);
                    auto modelType = static_cast<MortgageBehavioralModelType>(specDetails.modelType_);
                    auto& opt = modelOptions.lossModelOptions_[modelType];
                    opt.modelSpec_ = specDetails.settings_.modelSpec_;
                    opt.sessionSpec_ = move(specDetails.settings_.sessionSpec_);
                    break;
                }
                case HandleBinding::CashflowModel: {
                    auto& specDetails = handleObject<HandleType::ModelSpec>(from);
                    auto modelType = static_cast<CashflowModelType>(specDetails.modelType_);
                    auto& opt = value_of(modelOptions.cashflowModelOptions_)[modelType];
                    opt.modelSpec_ = specDetails.settings_.modelSpec_;
                    opt.sessionSpec_ = move(specDetails.settings_.sessionSpec_);
                    break;
                }
                case HandleBinding::DiscountingModel: {
                    auto& specDetails = handleObject<HandleType::ModelSpec>(from);
                    auto& opt = value_of(modelOptions.discountingModelOptions_);
                    opt.modelType_ = static_cast<DiscountingModelType>(specDetails.modelType_);
                    opt.settings_.modelSpec_ = specDetails.settings_.modelSpec_;
                    opt.settings_.sessionSpec_ = move(specDetails.settings_.sessionSpec_);
                    break;
                }
                case HandleBinding::InterestRateModel:
                {
                    auto& specDetails = handleObject<HandleType::ModelSpec>(from);
                    auto& opt = modelOptions.irModelOptions_;
                    opt.modelType_ = static_cast<InterestRateModelType>(specDetails.modelType_);
                    opt.settings_.modelSpec_ = specDetails.settings_.modelSpec_;
                    opt.settings_.sessionSpec_ = move(specDetails.settings_.sessionSpec_);
                    opt.calibrationBasket_ = getVolatilityBasket(opt.settings_.sessionSpec_);
                    break;
                }
                case HandleBinding::CurveModificationSpec:
                {
                    modelOptions.curveModSpec_ = handleObject<HandleType::CurveModificationSpec>(from);
                    break;
                }
                case HandleBinding::CurveInterpolationSpec:
                {
                    modelOptions.curveInterpSpec_ = handleObject<HandleType::CurveInterpolationSpec>(from);
                    break;
                }
                default: break;
            }
            return {};
        });
    RETURN_API_SUCCESS(modelOptions);

    EXCEPTION_API_ERROR(modelOptions);
}


/*-----------------------------------------------------------------------------------------
                                      PORTFOLIO
-------------------------------------------------------------------------------------------*/
Handle createInstrumentPortfolio(
    ApiPortfolioType portfolioType,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT2;
    CHECK_CONTENT2(NullHandle, content, contentLen);
    if (portfolioType == ApiPortfolioType_Mbs) {
        MortgageValuationPortfolio origPortfolio;
        origPortfolio.isMsr = false;
        std::tie(origPortfolio.pools, origPortfolio.pricingInput, std::ignore) =
            MortgagePoolPortfolioReader::readFromCsv({ content, (size_t)contentLen });
        return handles->addHandleData(make_shared<HandleData>(
            HandleType::Portfolio, move(origPortfolio)));
    }
    else {
        MortgageValuationPortfolio msrPortfolio = MsrPortfolioReader::readFromCsv({ content, (size_t)contentLen });
        return handles->addHandleData(make_shared<HandleData>(
            HandleType::Portfolio, move(msrPortfolio)));
    }

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int mapBehavioralModel(
    Handle portfolio,
    Handle modelSpec,
    Date factorDate) {

    TRY

    CHECK_INIT;
    CHECK_HANDLE(portfolio, HandleType::Portfolio);
    CHECK_HANDLE(modelSpec, HandleType::ModelSpec);
    WRITE_LOCK_HANDLE(portfolio)
        READ_LOCK_HANDLE(modelSpec)

        auto& spec = handleObject<HandleType::ModelSpec>(modelSpec);
    // check if the model type is behavioral model map
    if (spec.modelType_ != ApiModel::ApiModel_BehavioralModelMap) {
        RETURN_API_ERROR(modelSpec, ApiErrorInvalidModelType, std::format("Expect ApiModel_BehavioralModelMap as input modelSpec, but got {} instead", to_string(spec.modelType_)));
    }

    // check if the behavioral model map is set
    if (spec.settings_.modelSpec_.find(ModelParamName::ModelParams) == spec.settings_.modelSpec_.end()) {
        RETURN_API_ERROR(modelSpec, ApiErrorInvalidModelParameter, "Please set behavioral model map before map behavioral model in portfolio.");
    }

    // the inputs can now be used to assign the models
    wfmcm::BehavioralModelMapBuilder behavioralModelMapBuilder;
    auto behavModelMap = behavioralModelMapBuilder
        .withSpec(spec.settings_.modelSpec_)
        .build();

    auto& pools = handleObject<HandleType::Portfolio>(portfolio).pools;
    for (size_t i = 0; i < pools.size(); ++i)
    {
        // Set the residential mortgage FactorDate here
        ensure_dates(pools, std::chrono::year_month_day(QuantLib::fromYYYYMMDD(factorDate.yyyymmdd_)));
        behavModelMap.assignSubModelType(pools[i]);
    }

    RETURN_API_SUCCESS(portfolio);

    EXCEPTION_API_ERROR(portfolio);
}

int bindInstrumentPortfolio(
    Handle request,
    Handle portfolio)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES)
    CHECK_HANDLE(portfolio, HandleType::Portfolio)
    READ_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(portfolio)

    switch (handleData(request).getType())
    {
    case HandleType::CalcValueForMortgageRequest:
        BIND_HANDLES(request, portfolio, HandleBinding::Portfolio,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageRequest>(to).portfolio_ =
                    handleObject<HandleType::Portfolio>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageWithSofrRatesRequest:
        BIND_HANDLES(request, portfolio, HandleBinding::Portfolio,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageWithSofrRatesRequest>(to).portfolio_ =
                    handleObject<HandleType::Portfolio>(from);
                return {};
            });
        break;
    case HandleType::AttribValueChangeByWaterfallRequest:
        BIND_HANDLES(request, portfolio, HandleBinding::Portfolio,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::AttribValueChangeByWaterfallRequest>(to).portfolio_ =
                    handleObject<HandleType::Portfolio>(from);
                return {};
            });
        break;
    case HandleType::CalcBehavioralSpeedFromPrimaryRateRequest:
        BIND_HANDLES(request, portfolio, HandleBinding::Portfolio,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcBehavioralSpeedFromPrimaryRateRequest>(to).portfolio_ =
                    handleObject<HandleType::Portfolio>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageFromRatesRequest:
        BIND_HANDLES(request, portfolio, HandleBinding::Portfolio,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageFromRatesRequest>(to).portfolio_ =
                    handleObject<HandleType::Portfolio>(from);
                return {};
            });
        break;
    case HandleType::CalcProfitabilityFromBehavioralSpeedsRequest:
        BIND_HANDLES(request, portfolio, HandleBinding::Portfolio,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcProfitabilityFromBehavioralSpeedsRequest>(to).portfolio_ =
                    handleObject<HandleType::Portfolio>(from);
                return {};
            });
        break;

    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }

    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

///*-----------------------------------------------------------------------------------------
//                                      PRICING SPEC
//-------------------------------------------------------------------------------------------*/
Handle createPricingSpec(
    ApiPricingOutputType pricingOutputType)
{
    TRY

    CHECK_INIT2;
    PricingOutputType type = static_cast<PricingOutputType>(pricingOutputType);
    return handles->addHandleData(make_shared<HandleData>(
            HandleType::PricingSpec, PricingSpec(type)));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int setBaseOASFlag(
    Handle request,
    bool useBaseOAS)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, HandleType::PricingSpec);
    WRITE_LOCK_HANDLE(request);
    handleObject<HandleType::PricingSpec>(request).setBaseOASFlag(useBaseOAS);
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setCleanPriceFlag(
    Handle request,
    bool isCleanPrice)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, HandleType::PricingSpec);
    WRITE_LOCK_HANDLE(request);
    handleObject<HandleType::PricingSpec>(request).setCleanPriceFlag(isCleanPrice);
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int bindPricingSpec(
    Handle request,
    Handle pricingSpec)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES)
    CHECK_HANDLE(pricingSpec, HandleType::PricingSpec)
    READ_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(pricingSpec)

    switch (handleData(request).getType())
    {
    case HandleType::CalcValueForMortgageRequest:
        BIND_HANDLES(request, pricingSpec, HandleBinding::PricingSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageRequest>(to).pricingSpec_ =
                    handleObject<HandleType::PricingSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageWithSofrRatesRequest:
        BIND_HANDLES(request, pricingSpec, HandleBinding::PricingSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageWithSofrRatesRequest>(to).pricingSpec_ =
                    handleObject<HandleType::PricingSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageFromRatesRequest:
        BIND_HANDLES(request, pricingSpec, HandleBinding::PricingSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageFromRatesRequest>(to).pricingSpec_ =
                    handleObject<HandleType::PricingSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcProfitabilityFromBehavioralSpeedsRequest:
        BIND_HANDLES(request, pricingSpec, HandleBinding::PricingSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcProfitabilityFromBehavioralSpeedsRequest>(to).pricingSpec_ =
                    handleObject<HandleType::PricingSpec>(from);
                return {};
            });
        break;

    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }

    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                   USER SCENARIOS
-------------------------------------------------------------------------------------------*/
Handle createUserScenarios(
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT2;
    CHECK_CONTENT2(NullHandle, content, contentLen);
    HandleObjectType<HandleType::UserScenarios> scenarios;
    tie(scenarios.scenarioGroups_, scenarios.scenarios_, scenarios.scenarioRiskFactorDerivativeCalc_) =
        ValuationScenarioReader::readFromJson({ content, (size_t)contentLen });
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::UserScenarios,
        move(scenarios)));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int bindUserScenarios(Handle request, Handle userScenarios)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_HANDLE(userScenarios, HandleType::UserScenarios);
    READ_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(userScenarios)

    switch (handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        BIND_HANDLES(request, userScenarios, HandleBinding::Scenarios,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(to).scenarios_ =
                    handleObject<HandleType::UserScenarios>(from).scenarios_;
                handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(to).scenarioGroupMap_ =
                    handleObject<HandleType::UserScenarios>(from).scenarioGroups_;
                handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(to).scenarioRiskFactorDerivativeCalc_ =
                    handleObject<HandleType::UserScenarios>(from).scenarioRiskFactorDerivativeCalc_;
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageRequest:
        BIND_HANDLES(request, userScenarios, HandleBinding::Scenarios,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageRequest>(to).scenarios_ =
                    handleObject<HandleType::UserScenarios>(from).scenarios_;
                handleObject<HandleType::CalcValueForMortgageRequest>(to).scenarioGroupMap_ =
                    handleObject<HandleType::UserScenarios>(from).scenarioGroups_;
                handleObject<HandleType::CalcValueForMortgageRequest>(to).scenarioRiskFactorDerivativeCalc_ =
                    handleObject<HandleType::UserScenarios>(from).scenarioRiskFactorDerivativeCalc_;
                return {};
            });
        break;
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }

    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                 User Override Primary Rate Inputs at T0
-------------------------------------------------------------------------------------------*/

Handle createPrimaryRateOverride(
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT2;
    CHECK_CONTENT2(NullHandle, content, contentLen);
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::PrimaryRateOverride,
        PrimaryRatesOverrideReader::readFromCsv({ content, (size_t)contentLen })));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int bindPrimaryRateOverride(
    Handle request,
    Handle primaryRatesOverride)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_HANDLE(primaryRatesOverride, HandleType::PrimaryRateOverride);
    READ_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(primaryRatesOverride)

        switch (handleData(request).getType())
        {
        case HandleType::GenRatePathsForMortgageValuationRequest:
            BIND_HANDLES(request, primaryRatesOverride, HandleBinding::PrimaryRateInputs,
                [](Handle to, Handle from, int setterId)->HandleError {
                    handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(to).timeZeroPrimaryRatesOverride_ =
                        handleObject<HandleType::PrimaryRateOverride>(from);
                    return {};
                });
            break;
        case HandleType::CalcValueForMortgageRequest:
            BIND_HANDLES(request, primaryRatesOverride, HandleBinding::PrimaryRateInputs,
                [](Handle to, Handle from, int setterId)->HandleError {
                    handleObject<HandleType::CalcValueForMortgageRequest>(to).timeZeroPrimaryRatesOverride_ =
                        handleObject<HandleType::PrimaryRateOverride>(from);
                    return {};
                });
            break;
        default:
            RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
        }

    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                   SOFR RATE PATHS
-------------------------------------------------------------------------------------------*/
Handle createSofrRatePaths(
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT2;
    CHECK_CONTENT2(NullHandle, content, contentLen);

    return handles->addHandleData(make_shared<HandleData>(
        HandleType::SofrRatePaths,
        MCSwapRatesReader::readFromCsv({ content, (size_t)contentLen })));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

Handle createSofrRatePaths2(
    Handle cashTenorToRatePathsMap,
    Handle swapTenorToRatePathsMap)
{
    TRY

    CHECK_INIT2;
    CHECK_HANDLE_TYPE2(NullHandle, cashTenorToRatePathsMap, HandleType::TenorRatePathsMap);
    CHECK_HANDLE2(NullHandle, swapTenorToRatePathsMap, HandleType::TenorRatePathsMap);
    Handle sofrHandle = handles->addHandleData(make_shared<HandleData>(
        HandleType::SofrRatePaths,
        HandleObjectType<HandleType::SofrRatePaths>{}));

    READ_LOCK_HANDLE(cashTenorToRatePathsMap)
    READ_LOCK_HANDLE(swapTenorToRatePathsMap)

    //bind cash rates
    BIND_HANDLES(sofrHandle, cashTenorToRatePathsMap, HandleBinding::CashRates,
        [](Handle to, Handle from, int setterId)->HandleError {
            auto& rates = handleObject<HandleType::SofrRatePaths>(to);
            for (auto& [tenor, paths] : handleObject<HandleType::TenorRatePathsMap>(from)) {
                rates.emplace(KeyRate{ProductType::LiborSwap, tenor}, move(paths));
            }
            return {};
        });

    //bind swap rates
    BIND_HANDLES(sofrHandle, swapTenorToRatePathsMap, HandleBinding::SwapRates,
        [](Handle to, Handle from, int setterId)->HandleError {
            auto& rates = handleObject<HandleType::SofrRatePaths>(to);
            for (auto& [tenor, paths] : handleObject<HandleType::TenorRatePathsMap>(from)) {
                rates.emplace(KeyRate{ ProductType::SofrSwap, tenor }, move(paths));
            }
            return {};
        });

    return sofrHandle;

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int bindSofrRatePaths(
    Handle request,
    Handle sofrRatePaths)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_HANDLE(sofrRatePaths, HandleType::SofrRatePaths);
    READ_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(sofrRatePaths)

    switch (handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        BIND_HANDLES(request, sofrRatePaths, HandleBinding::SofrRatePathsOrGenSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(to).sofrRatePaths_ =
                    handleObject<HandleType::SofrRatePaths>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageWithSofrRatesRequest:
        BIND_HANDLES(request, sofrRatePaths, HandleBinding::SofrRatePathsOrGenSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageWithSofrRatesRequest>(to).sofrRatePaths_ =
                    handleObject<HandleType::SofrRatePaths>(from);
                return {};
            });
        break;
    case HandleType::GenSecondaryMortgageRatePathsRequest:
        BIND_HANDLES(request, sofrRatePaths, HandleBinding::SofrRatePathsOrGenSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::GenSecondaryMortgageRatePathsRequest>(to).sofrRatePaths_ =
                    handleObject<HandleType::SofrRatePaths>(from);
                return {};
            });
        break;
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}


/*-----------------------------------------------------------------------------------------
                                  MATRIX and RATE PATH SET
-------------------------------------------------------------------------------------------*/
Handle createFloatingPointMatrix(int rows, int cols)
{
    TRY

    CHECK_INIT2;
    CHECK_SIZE2(NullHandle, rows);
    CHECK_SIZE2(NullHandle, cols);
    HandleObjectType<HandleType::FloatingPointMatrix> m;
    multi_resize(m, array<size_t, 2>{(size_t)rows, (size_t)cols});
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::FloatingPointMatrix,
        move(m)));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int setFloatingPointMatrixValue(
    Handle matrix,
    int row,
    int col,
    double value)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(matrix, HandleType::FloatingPointMatrix);
    WRITE_LOCK_HANDLE(matrix);
    auto& m = handleObject<HandleType::FloatingPointMatrix>(matrix);
    CHECK_RANGE(matrix, row, 0, m.size()-1);
    CHECK_RANGE(matrix, col, 0, m[0].size() - 1);
    m[row][col] = value;
    RETURN_API_SUCCESS(matrix);

    EXCEPTION_API_ERROR(matrix);
}

Handle createRatePathCollection(int numPaths)
{
    TRY

    CHECK_INIT2;
    CHECK_SIZE2(NullHandle, numPaths);
    HandleObjectType<HandleType::RatePathCollection> paths;
    paths.reserve(numPaths);
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::RatePathCollection,
        move(paths)));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int ratePathCollectionAdd(
    Handle ratePathCollection,
    const double* pathValues,
    int pathLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(ratePathCollection, HandleType::RatePathCollection);
    CHECK_ARRAY(ratePathCollection, pathValues, pathLen);
    WRITE_LOCK_HANDLE(ratePathCollection);
    auto& paths = handleObject<HandleType::FloatingPointMatrix>(ratePathCollection);
    if (!paths.empty() && paths[0].size() != pathLen) {
        RETURN_API_ERROR(ratePathCollection, ApiErrorPathLengthMismatch, fmt::format("Path length must be {}", paths[0].size()));
    }
    paths.emplace_back(pathValues, pathValues + pathLen);
    RETURN_API_SUCCESS(ratePathCollection);

    EXCEPTION_API_ERROR(ratePathCollection);
}

Handle createTenorRatePathsMap()
{
    TRY

    CHECK_INIT2;
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::TenorRatePathsMap,
        HandleObjectType<HandleType::TenorRatePathsMap>{}));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int setRatePathsForTenor(
    Handle tenorRatePathsMap,
    int tenor,
    Handle ratePathCollection)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(tenorRatePathsMap, HandleType::TenorRatePathsMap);
    CHECK_HANDLE(ratePathCollection, HandleType::RatePathCollection);
    READ_LOCK_HANDLE(tenorRatePathsMap)
    READ_LOCK_HANDLE(ratePathCollection)

    const auto& tenorMap = handleObject<HandleType::TenorRatePathsMap>(tenorRatePathsMap);
    const auto& paths = handleObject<HandleType::RatePathCollection>(ratePathCollection);
    if (!tenorMap.empty()) {
        if (tenorMap.begin()->second.size() != paths.size()) {
            RETURN_API_ERROR(tenorRatePathsMap, ApiErrorNumPathsOutOfRange,
                fmt::format("Number of paths must be {}", tenorMap.begin()->second.size()));
        }
        if (tenorMap.begin()->second[0].size() != paths[0].size()) {
            RETURN_API_ERROR(tenorRatePathsMap, ApiErrorPathLengthMismatch,
                fmt::format("Path length must be {}", tenorMap.begin()->second[0].size()));
        }
    }
    //bind value (use tenor value as setter id)
    BIND_HANDLES(tenorRatePathsMap, ratePathCollection, tenor,
        [](Handle to, Handle from, int setterId)->HandleError {
            handleObject<HandleType::TenorRatePathsMap>(to)[setterId] =
                handleObject<HandleType::RatePathCollection>(from);
            return {};
        });
    RETURN_API_SUCCESS(tenorRatePathsMap);

    EXCEPTION_API_ERROR(tenorRatePathsMap);
}

/*-----------------------------------------------------------------------------------------
                              Curve Modification Spec
-------------------------------------------------------------------------------------------*/
int setCurveDateShift(
    Handle curveModSpec,
    ApiCurveDateShift shift)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(curveModSpec, HandleType::CurveModificationSpec);
    WRITE_LOCK_HANDLE(curveModSpec);
    auto& spec = handleObject<HandleType::CurveModificationSpec>(curveModSpec);
    spec.curveDateShiftOption_ = static_cast<CurveDateShiftOption>(shift);
    RETURN_API_SUCCESS(curveModSpec);

    EXCEPTION_API_ERROR(curveModSpec);
}

int setProxyCurveDefinition(
    Handle curveModSpec,
    const int* tenors,
    int numTenors)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(curveModSpec, HandleType::CurveModificationSpec);
    CHECK_ARRAY(curveModSpec, tenors, numTenors);
    WRITE_LOCK_HANDLE(curveModSpec);
    auto& spec = handleObject<HandleType::CurveModificationSpec>(curveModSpec);
    spec.proxyCurveDefinition_ = { tenors, tenors + numTenors };
    RETURN_API_SUCCESS(curveModSpec);

    EXCEPTION_API_ERROR(curveModSpec);
}

/*-----------------------------------------------------------------------------------------
                              Curve Interpolation Spec
-------------------------------------------------------------------------------------------*/

int setSofrCurveInterpolationMethod(
    Handle curveInterpSpec,
    ApiInterpolationMethod interp)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(curveInterpSpec, HandleType::CurveInterpolationSpec);
    WRITE_LOCK_HANDLE(curveInterpSpec);
    auto& spec = handleObject<HandleType::CurveInterpolationSpec>(curveInterpSpec);
    spec.sofrInterpMethod_ = static_cast<InterpolationMethod>(interp);
    RETURN_API_SUCCESS(curveInterpSpec);

    EXCEPTION_API_ERROR(curveInterpSpec);
}

int setUstCurveInterpolationMethod(
    Handle curveInterpSpec,
    ApiInterpolationMethod interp)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(curveInterpSpec, HandleType::CurveInterpolationSpec);
    WRITE_LOCK_HANDLE(curveInterpSpec);
    auto& spec = handleObject<HandleType::CurveInterpolationSpec>(curveInterpSpec);
    spec.ustInterpMethod_ = static_cast<InterpolationMethod>(interp);
    RETURN_API_SUCCESS(curveInterpSpec);

    EXCEPTION_API_ERROR(curveInterpSpec);
}


/*-----------------------------------------------------------------------------------------
                                      GREEK SPEC
-------------------------------------------------------------------------------------------*/
Handle createGreekSpec(
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT2;
    CHECK_CONTENT2(NullHandle, content, contentLen);
    GreekSpecReader::ReadType greekSpec = GreekSpecReader::readFromJson({ content, (size_t)contentLen });
    if (greekSpec.index() == 0) {
        std::string versions;
        for (size_t i = 0; i < GreekSpecReader::FormatVersions.size(); ++i) {
            if (i > 0) versions += ", ";
            versions += std::to_string(GreekSpecReader::FormatVersions[i]);
        }
        RETURN_ERROR(NullHandle, ApiErrorInvalidFormat,
            fmt::format("Invalid greek spec version. Supported versions are: [{}]", versions), NullHandle);
    }
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::GreekSpec,
        move(std::get<1>(greekSpec))));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int bindGreekSpec(Handle request, Handle greekSpec)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_HANDLE(greekSpec, HandleType::GreekSpec);
    READ_LOCK_HANDLE(request)
    READ_LOCK_HANDLE(greekSpec)

    switch (handleData(request).getType())
    {
    case HandleType::GenRatePathsForMortgageValuationRequest:
        BIND_HANDLES(request, greekSpec, HandleBinding::GreekSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::GenRatePathsForMortgageValuationRequest>(to).greekSpec_ =
                    handleObject<HandleType::GreekSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcValueForMortgageRequest:
        BIND_HANDLES(request, greekSpec, HandleBinding::GreekSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcValueForMortgageRequest>(to).greekSpec_ =
                    handleObject<HandleType::GreekSpec>(from);
                return {};
            });
        break;
    case HandleType::CalcGreeksFromPricesRequest:
        BIND_HANDLES(request, greekSpec, HandleBinding::GreekSpec,
            [](Handle to, Handle from, int setterId)->HandleError {
                handleObject<HandleType::CalcGreeksFromPricesRequest>(to).greekSpec_ =
                    handleObject<HandleType::GreekSpec>(from);
                return {};
            });
        break;
    default:
        RETURN_API_ERROR(request, ApiErrorInvalidRequestType, "Request does not support this operation");
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                WATERFALL ATTRIBUTION REQUEST
-------------------------------------------------------------------------------------------*/
int setWaterfallAttributionSteps(
    Handle request,
    const ApiWaterfallStep* waterfallSteps,
    int numWaterfallSteps)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES)
    CHECK_ARRAY(request, waterfallSteps, numWaterfallSteps);
    WRITE_LOCK_HANDLE(request);
    std::pmr::vector<WaterfallStep>* steps = nullptr;
    auto type = handleData(request).getType();
    switch (type)
    {
    case HandleType::AttribValueChangeByWaterfallRequest:
        steps = &handleObject<HandleType::AttribValueChangeByWaterfallRequest>(request).steps_;
        break;
    case HandleType::GeneratePathForWaterfallAttributionRequest:
        steps = &handleObject<HandleType::GeneratePathForWaterfallAttributionRequest>(request).steps_;
        break;
    default:
        THROW("Request type " + to_string(type) + " doesn't support Waterfall Steps assignment");
    }

    steps->clear();
    steps->reserve(numWaterfallSteps);
    for (int i = 0; i < numWaterfallSteps; ++i) {
        steps->push_back(static_cast<WaterfallStep>(waterfallSteps[i]));
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int setRiskFactorDerivativeCalculation(
    Handle request,
    ApiDerivativeCalculation derivType)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES)
    WRITE_LOCK_HANDLE(request);
    auto type = handleData(request).getType();
    switch (type)
    {
    case HandleType::AttribValueChangeByWaterfallRequest:
        handleObject<HandleType::AttribValueChangeByWaterfallRequest>(request).riskFactorDerivativeCalc_ =
            static_cast<DerivativeCalculation>(derivType);
        break;
    case HandleType::GeneratePathForWaterfallAttributionRequest:
        handleObject<HandleType::GeneratePathForWaterfallAttributionRequest>(request).riskFactorDerivativeCalc_ =
            static_cast<DerivativeCalculation>(derivType);
        break;
    default:
        THROW("Request type " + to_string(type) + " doesn't support RiskFactorDerivativeCalculation assignment");
    }

    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                MONTH END ROLL REQUEST
-------------------------------------------------------------------------------------------*/
int setMonthEndRollSteps(
    Handle request,
    const ApiMonthEndRollStep* rollSteps,
    int numRollSteps)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, HandleType::CalcValueForMonthEndRollRequest)
    CHECK_ARRAY(request, rollSteps, numRollSteps);
    WRITE_LOCK_HANDLE(request);
    auto& steps = handleObject<HandleType::CalcValueForMonthEndRollRequest>(request).steps_;
    steps.reserve(numRollSteps);
    for (int i = 0; i < numRollSteps; ++i) {
        steps.push_back(static_cast<MonthEndRollStep>(rollSteps[i]));
    }
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                  CALCUALTE GREEKS
-------------------------------------------------------------------------------------------*/
int setGreekScenarioPrices(
    Handle request,
    const char* content,
    int contentLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, HandleType::CalcGreeksFromPricesRequest)
    CHECK_CONTENT(request, content, contentLen);
    WRITE_LOCK_HANDLE(request);
    auto& greekRequest = handleObject<HandleType::CalcGreeksFromPricesRequest>(request);
    std::tie(greekRequest.instrumentIds_, greekRequest.scenarioPrices_) =
        GreekPricesReader::readFromCsv({ content, (size_t)contentLen });
    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

/*-----------------------------------------------------------------------------------------
                                    EXECUTION OPTIONS
-------------------------------------------------------------------------------------------*/
Handle createExecOptions()
{
    TRY

    CHECK_INIT2;
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::ExecOptions,
        HandleObjectType<HandleType::ExecOptions>{}));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int setInputValidationOnly(
    Handle execOptions,
    bool value)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(execOptions, HandleType::ExecOptions);
    WRITE_LOCK_HANDLE(execOptions);
    handleObject<HandleType::ExecOptions>(execOptions).execControl_.inputValidationOnly_ = value;
    RETURN_API_SUCCESS(execOptions);

    EXCEPTION_API_ERROR(execOptions);
}

int setExecutionMode(
    Handle execOptions,
    ApiExecMode mode)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(execOptions, HandleType::ExecOptions);
    WRITE_LOCK_HANDLE(execOptions);
    handleObject<HandleType::ExecOptions>(execOptions).execControl_.asyncMode_ = static_cast<app::Async>(mode);
    RETURN_API_SUCCESS(execOptions);

    EXCEPTION_API_ERROR(execOptions);
}

int setExecutorType(
    Handle execOptions,
    ApiExecutorType execType)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(execOptions, HandleType::ExecOptions);
    WRITE_LOCK_HANDLE(execOptions);
    handleObject<HandleType::ExecOptions>(execOptions).execControl_.executorType_ = static_cast<app::Executor>(execType);
    RETURN_API_SUCCESS(execOptions);

    EXCEPTION_API_ERROR(execOptions);
}

int setFloatingPointPrecision(
    Handle execOptions,
    int precision)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(execOptions, HandleType::ExecOptions);
    CHECK_RANGE(execOptions, precision, 0, 16)
    WRITE_LOCK_HANDLE(execOptions);
    handleObject<HandleType::ExecOptions>(execOptions).precision_ = static_cast<uint8_t>(precision);
    RETURN_API_SUCCESS(execOptions);

    EXCEPTION_API_ERROR(execOptions);
}

int enableResponseSlicing(Handle execOptions)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(execOptions, HandleType::ExecOptions);
    WRITE_LOCK_HANDLE(execOptions);
    handleObject<HandleType::ExecOptions>(execOptions).sliceResult_ = true;
    RETURN_API_SUCCESS(execOptions);

    EXCEPTION_API_ERROR(execOptions);
}

// {NOTE} setErrorBehavior kept behind #if 0 — see comment in WfmcmApi.h.
// Enabling it would change the error-handling contract for all existing API
// consumers.  The new model-suite path handles RunToCompletion internally.
#if 0
int setErrorBehavior(
    Handle execOptions,
    ApiErrorBehavior behavior)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(execOptions, HandleType::ExecOptions);
    WRITE_LOCK_HANDLE(execOptions);
    handleObject<HandleType::ExecOptions>(execOptions).execControl_.errorBehavior_ = static_cast<app::ErrorBehavior>(behavior);
    RETURN_API_SUCCESS(execOptions);

    EXCEPTION_API_ERROR(execOptions);
}
#endif

int includeStackTrace(
    Handle execOptions)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(execOptions, HandleType::ExecOptions);
    WRITE_LOCK_HANDLE(execOptions);
    handleObject<HandleType::ExecOptions>(execOptions).execControl_.stackTrace_ = true;
    RETURN_API_SUCCESS(execOptions);

    EXCEPTION_API_ERROR(execOptions);
}

int setUseNewBehavioralArchitecture(
    Handle execOptions,
    bool value)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(execOptions, HandleType::ExecOptions);
    WRITE_LOCK_HANDLE(execOptions);
    handleObject<HandleType::ExecOptions>(execOptions).useNewBehavioralArchitecture_ = value;
    RETURN_API_SUCCESS(execOptions);

    EXCEPTION_API_ERROR(execOptions);
}

int setSuiteRegistryConfigFile(
    Handle execOptions,
    const char* path,
    int pathLen)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(execOptions, HandleType::ExecOptions);
    if (path == nullptr) {
        RETURN_API_ERROR(execOptions, ApiErrorNullArgument, "Null input [path]");
    }
    WRITE_LOCK_HANDLE(execOptions);
    handleObject<HandleType::ExecOptions>(execOptions).suiteRegistryConfigFile_ = std::string(path, pathLen < 0 ? strlen(path) : pathLen);
    RETURN_API_SUCCESS(execOptions);

    EXCEPTION_API_ERROR(execOptions);
}

/*-----------------------------------------------------------------------------------------
                                      REQUEST EXECUTION
-------------------------------------------------------------------------------------------*/

namespace {

// Bootstrap handler for obtaining a ScenarioManager without a real request flow.
// Mirrors the pattern in PrepaymentDriver::calc_behavioral_model_new_architecture.
class ApiRuntimeBootstrapHandler final : public msg::BaseRequestHandler
{
public:
    explicit ApiRuntimeBootstrapHandler(msg::Request&& request)
        : BaseRequestHandler(std::move(request))
    {}

    msg::ScenarioManager& scenarioManagerRef() { return scenarioManager_; }

private:
    msg::Response::Selection handleRequest() override { return std::monostate{}; }
};

/**
 * @brief Execute a CalcBehavioralSpeedFromPrimaryRate request through the new
 *        model-suite architecture (WorkflowOrchestrator / ModelSuiteLayer).
 *
 * Builds a WorkflowRequest with embedded payloads from the already-parsed
 * request data and routes through WorkflowOrchestrator.
 *
 * @param req      The original request (will be moved from).
 * @param opts     Execution options containing suite registry path and exec control.
 * @return msg::Response with CalcBehavioralSpeedFromPrimaryRateResponse or error.
 */
msg::Response executeBehavioralSuiteFromRequest(
    msg::Request&& req,
    const staging::ExecutionOptions& opts)
{
    using namespace wfmcm::model_suite;

    auto header = req.header_;
    auto start = std::chrono::system_clock::now();

    try {
        // Validate suite registry config file
        if (opts.suiteRegistryConfigFile_.empty()) {
            msg::ErrorResponseBuilder b;
            b.withError("config_error",
                "suiteRegistryConfigFile must be set when useNewBehavioralArchitecture is enabled");
            auto resp = msg::makeResponse(b.build(), header);
            msg::finalizeHeader(&resp.header_, start);
            return resp;
        }
        if (!std::filesystem::exists(opts.suiteRegistryConfigFile_)) {
            msg::ErrorResponseBuilder b;
            b.withError("config_error",
                "Suite registry config file not found: " + opts.suiteRegistryConfigFile_);
            auto resp = msg::makeResponse(b.build(), header);
            msg::finalizeHeader(&resp.header_, start);
            return resp;
        }

        // Extract the behavioral request data
        auto& behavReq = req.calcBehavioralSpeedFromPrimaryRateRequest();

        // Bootstrap runtime: ScenarioManager, DataAccessLayer, ModelSuiteLayer
        auto bootstrapRequest = msg::makeRequest(
            app::RequestType::CalcBehavioralSpeedFromPrimaryRate,
            "api-new-behavioral-architecture");
        ApiRuntimeBootstrapHandler bootstrapHandler{std::move(bootstrapRequest)};

        auto dataAccessLayer = std::make_shared<wfmcm::data_access::DataAccessLayer>(true /*enableCache*/);

        auto modelSuiteLayer = std::make_shared<ModelSuiteLayer>(
            bootstrapHandler.scenarioManagerRef(),
            *dataAccessLayer,
            opts.suiteRegistryConfigFile_);

        // Build WorkflowRequest with embedded payloads from handle data
        WorkflowRequest workflowRequest;

        // Portfolio — embed already-parsed data
        workflowRequest.portfolio_.data_ = std::move(behavReq.portfolio_);
        workflowRequest.portfolio_.isEmbedded_ = true;

        // Rate paths — embed already-parsed data
        workflowRequest.primaryRatePaths_.data_ = std::move(behavReq.ratePaths_);
        workflowRequest.primaryRatePaths_.isEmbedded_ = true;
        workflowRequest.keyRatePaths_.data_ = std::move(behavReq.keyRatePaths_);
        workflowRequest.keyRatePaths_.isEmbedded_ = true;

        // Historical data — embed already-parsed data
        workflowRequest.historicalData_.data_ = std::move(behavReq.historicalData_);
        workflowRequest.historicalData_.isEmbedded_ = true;

        // Date spec
        workflowRequest.dateSpec_ = behavReq.dateSpec_;

        // Model options
        workflowRequest.prepayModelOptions_ = std::move(behavReq.prepayModelOptions_);
        workflowRequest.lossModelOptions_ = std::move(behavReq.lossModelOptions_);

        // Execution control
        workflowRequest.execControl_ = opts.execControl_;

        // Projection length from session specs
        for (const auto& [type, settings] : workflowRequest.prepayModelOptions_) {
            const auto it = settings.sessionSpec_.find(wfmcm::SessionParamName::ProjectionLength);
            if (it == settings.sessionSpec_.end()) continue;
            try { workflowRequest.projectionLength_ = std::stoul(it->second); } catch (...) {}
            if (workflowRequest.projectionLength_ > 0) break;
        }

        // Model map spec — extract from the portfolio's model map if it was set via mapBehavioralModel
        // Note: The model map is applied to instruments before they reach this point.
        // The orchestrator will re-apply if modelMapSpec_ is set.

        // Request behavioral suite
        workflowRequest.requestedSuites_ = {"BehavioralSuite"};
        workflowRequest.requestId_ = "api-behavioral";

        // Execute through model suite layer
        auto suiteResult = modelSuiteLayer->execute(workflowRequest);

        // Handle failure
        if (!suiteResult.success_ &&
            opts.execControl_.errorBehavior_ != app::ErrorBehavior::RunToCompletion) {
            msg::ErrorResponseBuilder errorBuilder;
            const auto* behavSuite = suiteResult.getSuiteResult("BehavioralSuite");
            if (behavSuite && behavSuite->hasExceptionDetails()) {
                errorBuilder.withError(behavSuite->exceptionDetails_, /*stackTrace=*/true);
            } else if (behavSuite && !behavSuite->errorMessage_.empty()) {
                errorBuilder.withError("suite_error", behavSuite->errorMessage_);
            } else {
                errorBuilder.withError("suite_error", "Behavioral model suite execution failed");
            }
            auto resp = msg::makeResponse(errorBuilder.build(), header);
            msg::finalizeHeader(&resp.header_, start);
            return resp;
        }

        // Extract behavioral result
        auto behavioralResult = suiteResult.getBehavioralResult("BehavioralSuite");
        if (!behavioralResult) {
            msg::ErrorResponseBuilder errorBuilder;
            errorBuilder.withError("suite_error", "No behavioral result found in suite output");
            auto resp = msg::makeResponse(errorBuilder.build(), header);
            msg::finalizeHeader(&resp.header_, start);
            return resp;
        }

        // Build per-instrument errors
        auto perInstrumentErrors = msg::extractPerInstrumentErrors(
            behavioralResult->calculationResults_);

        // Build response selection
        msg::CalcBehavioralSpeedFromPrimaryRateResponse responseSelection;
        for (size_t i = 0; i < behavioralResult->calculationResults_.size(); ++i) {
            auto& calcResult = behavioralResult->calculationResults_[i];
            if (calcResult.hasError()) continue;

            auto outputs = std::move(calcResult)
                .operator wfmcm::MortgageBehavioralCalculationResult::mbm_outputs();
            msg::matrix<msg::BehavioralModelOutput>::value_type outputRow;
            outputRow.reserve(outputs.size());
            for (auto& output : outputs) {
                outputRow.emplace_back(std::move(output));
            }
            responseSelection.behavioralSpeeds_.push_back(std::move(outputRow));
        }

        auto resp = msg::makeResponse(
            std::move(responseSelection),
            std::move(perInstrumentErrors),
            header);
        msg::finalizeHeader(&resp.header_, start);
        return resp;

    } catch (const std::exception& e) {
        msg::ErrorResponseBuilder b;
        b.withError("execution_error", std::string("New behavioral architecture execution failed: ") + e.what());
        auto resp = msg::makeResponse(b.build(), header);
        msg::finalizeHeader(&resp.header_, start);
        return resp;
    } catch (...) {
        msg::ErrorResponseBuilder b;
        b.withError("execution_error", "New behavioral architecture execution failed: unknown error");
        auto resp = msg::makeResponse(b.build(), header);
        msg::finalizeHeader(&resp.header_, start);
        return resp;
    }
}

} // anonymous namespace

int executeWithOptions(
    Handle request,
    const staging::ExecutionOptions& opts)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES)
    int rc = handleData(request).fill(); //fill takes write lock
    if (rc != ApiSuccess) {
        return rc;
    }

    WRITE_LOCK_HANDLE(request);
    //create slicer
    requestHandleData(request).slicer_ = make_unique<ResponseSlicer>(opts.sliceResult_);
    bool sendLocal = false; //TODO: set dynamically via request options
    msg::Request& r = requestHandleData(request).request_;
    r.execControl_ = opts.execControl_;

    // Route through new model-suite architecture when enabled for behavioral requests
    const bool useNewArch = opts.useNewBehavioralArchitecture_
        && r.isCalcBehavioralSpeedFromPrimaryRateRequest();

    if (useNewArch) {
        // Capture opts by value for the lambda (need suite registry path)
        requestHandleData(request).response_ =
            std::async([opts](msg::Request&& req) -> msg::Response
            {
                thread_local memory::heap_pooled_allocator<id::heap_pooled_mem_resource> alloc;
                wfmcm::scoped_function localLoggers([]() { logging::thread::local::shutdown(); });
                return executeBehavioralSuiteFromRequest(std::move(req), opts);
            }, move(r));
    }
    else {
        requestHandleData(request).response_ =
            std::async([sendLocal](msg::Request&& req)->msg::Response
            {
                //Use pooled allocator instead of monotonic since we want to free allocated
                //blocks immediately and keep memory consumption low.
                //A monotonic allocator would grow too much before the thread ends.
                thread_local memory::heap_pooled_allocator<id::heap_pooled_mem_resource> alloc;

                //make sure we clear locally instantiated loggers
                wfmcm::scoped_function localLoggers([]() { logging::thread::local::shutdown(); });

                msg::Response response;
                if (sendLocal) {
                    msg::LocalConnector conn;
                    msg::RequestManager manager(conn);
                    response = manager.sendRequest(move(req));
                }
                else {
                    response = msg::RequestProcessor::handle(move(req));
                }
                return response;
            }, move(r));
    }

    //reset to monostate
    r = {};

    RETURN_API_SUCCESS(request);

    EXCEPTION_API_ERROR(request);
}

int execute(Handle request)
{
    CHECK_INIT;
    return executeWithOptions(request, staging::ExecutionOptions{});
}

int execute2(
    Handle request,
    Handle execOptions)
{
    CHECK_INIT;
    //Validate the request handle BEFORE dereferencing it via BIND_HANDLES2:
    //previously an invalid request handle reached handleData() (reinterpret_cast
    //of an arbitrary value) before executeWithOptions could reject it.
    CHECK_HANDLE(request, REQUEST_HANDLES);
    CHECK_HANDLE(execOptions, HandleType::ExecOptions);
    {
        READ_LOCK_HANDLE(request)
        BIND_HANDLES2(request, execOptions, HandleBinding::ExecOptions,
            [](Handle to, Handle from, int setterId)->HandleError {
                return {};
            }, false);
    }
    //Copy the options under a read lock rather than passing an unsynchronized reference.
    staging::ExecutionOptions optsCopy;
    {
        READ_LOCK_HANDLE(execOptions)
        optsCopy = handleObject<HandleType::ExecOptions>(execOptions);
    }
    return executeWithOptions(request, optsCopy);
}

const char* getResultWithTimeoutImpl(
    Handle request,
    int timeoutSec,
    bool queryResultSlices)
{
    int rc = waitForResultWithTimeout(request, timeoutSec);
    if (rc != ApiSuccess) {
        return empty_value<string>.c_str();
    }

    TRY

    READ_LOCK_HANDLE(request);
    const auto& requestData = requestHandleData(request);

    if (queryResultSlices && !requestData.hasData()) {
        //request or errors have already been read
        RETURN_ERROR(request, ApiErrorNoMoreData, "No more results", empty_value<string>.c_str());
    }
    else if (!queryResultSlices && !requestData.hasError()) {
        //request or errors have already been read
        RETURN_ERROR(request, ApiErrorNoMoreData, "No more errors", empty_value<string>.c_str());
    }

    //read result slices or errors
    auto& slicer = *requestData.slicer_;
    //get the exec options and set floating point precision output
    Handle execOptions = getBoundHandle(request, HandleBinding::ExecOptions);
    json_options options;
    options.json_allocator_buffer_size(8192);
    if (isValidHandle(execOptions)) {
        options.floating_point_precision(handleObject<HandleType::ExecOptions>(execOptions).precision_);
    }
    json_parser parser(options);

    //check to see which result is ready if any
    if (requestHandleData(request).isResponseReady()) {
        UPGRADE_TO_WRITE_LOCK_HANDLE(request)
        if (requestHandleData(request).isResponseReady()) {
            //load into the slicer
            slicer.loadResponse(requestHandleData(request).response_.get());
        }
    } //on exit: release write + take read lock

    //if we have slices available take the next one
    optional<Response> response;
    if (queryResultSlices && slicer.hasSlices()) {
        response.emplace(slicer.nextSlice());
    }
    else if (!queryResultSlices && slicer.hasErrorSlices()) {
        response.emplace(slicer.nextErrorSlice());
    }
    if (response) {
        //get the return thread local buffer
        auto& str = store<string, id::api_response>();
        polyvar p;
        p << response.value();
        str = parser.serialize(p);
        RETURN_SUCCESS(request, str.c_str());
    }

    RETURN_ERROR(request, ApiErrorNoMoreData, "No more data", empty_value<string>.c_str());

    EXCEPTION_ERROR(request, empty_value<string>.c_str());
}

int waitForResult(Handle request)
{
    return waitForResultWithTimeout(request, -1);
}

int waitForResultWithTimeout(Handle request, int timeoutSec)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);
    if ((timeoutSec != -1) && (timeoutSec < 0)) {
        RETURN_API_ERROR(request, ApiErrorInvalidTimeout, "Timeout must be >= 0 or -1");
    }

    READ_LOCK_HANDLE(request);
    const auto& requestData = requestHandleData(request);
    if (requestData.isResponsePending()) {
        //block thread and wait
        if (timeoutSec == -1) {
            requestData.response_.wait();
        }
        else {
            auto status = requestData.response_.wait_for(chrono::seconds(timeoutSec));
            if (status == future_status::timeout) {
                RETURN_API_ERROR(request, ApiErrorTimeout, "Operation timed out");
            }
        }
    }
    //result is ready
    RETURN_API_SUCCESS(request);

    } catch (const std::future_error&) {
        //result has already been retrieved or processed
        RETURN_API_SUCCESS(request);
    EXCEPTION_API_ERROR(request);
}

const char* getResult(Handle request)
{
    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", empty_value<string>.c_str());
    }
    return getResultWithTimeoutImpl(request, -1, true);
}

const char* getResultWithTimeout(Handle request, int timeoutSec)
{
    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", empty_value<string>.c_str());
    }
    return getResultWithTimeoutImpl(request, timeoutSec, true);
}

const char* getResultError(Handle request)
{
    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", empty_value<string>.c_str());
    }
    return getResultWithTimeoutImpl(request, -1, false);
}

const char* getResultErrorWithTimeout(Handle request, int timeoutSec)
{
    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", empty_value<string>.c_str());
    }
    return getResultWithTimeoutImpl(request, timeoutSec, false);
}

int hasResult(Handle request)
{
    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", ApiResultStatus_UNKNOWN);
    }
    CHECK_HANDLE2(ApiResultStatus_UNKNOWN, request, REQUEST_HANDLES);
    READ_LOCK_HANDLE(request);
    if (requestHandleData(request).isResponsePending()) {
        return ApiResultStatus_Pending;
    }
    return requestHandleData(request).hasData() ? ApiResultStatus_Available : ApiResultStatus_NotAvailable;
}

int hasResultError(Handle request)
{
    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", ApiResultStatus_UNKNOWN);
    }
    CHECK_HANDLE2(ApiResultStatus_UNKNOWN, request, REQUEST_HANDLES);
    READ_LOCK_HANDLE(request);
    if (requestHandleData(request).isResponsePending()) {
        return ApiResultStatus_Pending;
    }
    return requestHandleData(request).hasError() ? ApiResultStatus_Available : ApiResultStatus_NotAvailable;
}

int cancelRequest(Handle request)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(request, REQUEST_HANDLES);

    //Create the cancellation request. ScopedHandle releases it on every return
    //path -- previously the handle leaked in the container for the process lifetime.
    app::ScopedHandle cancel(createRequestObject(ApiInternalRequest_Cancel));
    if (!cancel) {
        RETURN_API_ERROR(NullHandle, ApiErrorException, "Failed to create cancel request");
    }
    BIND_HANDLES2(cancel, request, HandleBinding::CancelRequest,
        [](Handle to, Handle from, int setterId)->HandleError {
            const auto& sentRequest = requestHandleData(from);
            if (sentRequest.isResponsePending()) {
                //request can be cancelled
                auto& cancelRequest = handleObject<HandleType::CancelRequest>(to);
                cancelRequest.groupId_ = sentRequest.id_.first;
                return {};
            }
            return HandleError(ApiErrorNotCancellable, "Request already terminated");
        }, false);
    //Execute using default options
    int rc = execute(cancel);
    //Block and wait
    if (rc != ApiSuccess) {
        return rc;
    }
    //Get the result
    string result = getResult(cancel);
    if (hasHandleError(cancel)) {
        return handleError().second.error_;
    }
    //Validate result
    app::messages::Response response;
    from_string(result, &response);
    if (response.isCancelResponse()) {
        RETURN_API_SUCCESS(cancel);
    }
    //we have an error
    RETURN_API_ERROR(cancel, ApiErrorRequestNotFound, "Request not found");

    EXCEPTION_API_ERROR(request);
}

namespace {

const char* extractJsonErrorString(pair<string, HandleError>& value)
{
    polyvar poly;
    poly << value.second;
    value.first = to_string(poly);
    return value.first.c_str();
}

} // namespace

const char* getError()
{
    return extractJsonErrorString(globalError());
}

bool hasError()
{
    return globalError().second.isError();
}

const char* getHandleError(Handle handle)
{
    if (isValidHandle(handle)) {
        //load error into thread local store
        handleError().second = handleData(handle).getError();
        return extractJsonErrorString(handleError());
    }
    if (handles) {
        setHandleError(NullHandle, { ApiErrorInvalidHandle, "Invalid handle" });
    }
    else {
        setHandleError(NullHandle, { ApiErrorLibSetup, "Library not initialized" });
    }
    return getError();
}

bool hasHandleError(Handle handle)
{
    return isValidHandle(handle) ? handleData(handle).getError().isError() : true;
}

/*-----------------------------------------------------------------------------------------
                                      RESOURCE MANAGEMENT
-------------------------------------------------------------------------------------------*/
int deleteHandle(Handle handle)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(handle, HandleType::Any);
    if (handles->deleteHandleData(handle) == 0) {
        RETURN_API_ERROR(handle, ApiErrorHandleNotFound, "Handle not found");
    }
    RETURN_API_SUCCESS(handle);

    EXCEPTION_API_ERROR(handle);
}

bool isValidHandle(Handle handle)
{
    return handles && (handle != NullHandle) && handles->handleExists(handle);
}

bool isValidDate(Date date)
{
    //Inclusive: error messages state dates "must start from 1900-01-01", but the
    //previous strict '<' rejected exactly 19000101.
    return !(date < MinDateInt);
}

Handle cloneHandle(Handle handle)
{
    CHECK_INIT2;
    CHECK_HANDLE2(NullHandle, handle, HandleType::Any);
    READ_LOCK_HANDLE(handle);
    return handles->cloneHandleData(handle);
}

Handle nullHandle()
{
    return NullHandle;
}

Date nullDate()
{
    return NullDate;
}

/*-----------------------------------------------------------------------------------------
                                     HANDLE ARRAY
-------------------------------------------------------------------------------------------*/
Handle createHandleArray(int size)
{
    TRY

    CHECK_INIT2;
    CHECK_SIZE2(NullHandle, size);
    return handles->addHandleData(make_shared<HandleData>(
        HandleType::HandleArray, HandleObjectType<HandleType::HandleArray>{(size_t)size, NullHandle}));

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

int insertHandleAt(Handle handleArray, int position, Handle handle)
{
    TRY

    CHECK_INIT;
    CHECK_HANDLE(handleArray, HandleType::HandleArray);
    WRITE_LOCK_HANDLE(handleArray);
    auto& arr = handleObject<HandleType::HandleArray>(handleArray);
    CHECK_RANGE(handleArray, position, 0, arr.size() - 1);
    arr[position] = handle;
    RETURN_API_SUCCESS(handleArray);

    EXCEPTION_API_ERROR(handleArray)
}

Handle getHandleAt(Handle handleArray, int position)
{
    TRY

    CHECK_INIT2;
    CHECK_HANDLE2(NullHandle, handleArray, HandleType::HandleArray);
    READ_LOCK_HANDLE(handleArray);
    auto& arr = handleObject<HandleType::HandleArray>(handleArray);
    CHECK_RANGE2(handleArray, position, 0, arr.size() - 1);
    RETURN_SUCCESS(handleArray, arr[position]);

    EXCEPTION_ERROR(handleArray, NullHandle)
}
