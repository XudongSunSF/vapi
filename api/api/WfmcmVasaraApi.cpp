#include <src/app-common/api/WfmcmVasaraApi.h>
#include <src/app-common/api/WfmcmApiInternal.h>
#include <src/app-common/api/ModelConfigHash.h>
#include <src/app-common/HashMix.h>
#include <src/app-common/messages/RequestProcessor.h>
#include <src/app-common/messages/requests/GenRatePathsForMortgageValuationRequest.h>
#include <src/app-common/messages/requests/CalcValueForMortgageFromRatesRequest.h>
#include <src/app-common/messages/handlers/BaseRequestHandler.h>
#include <src/app-common/messages/responses/CalcValueForMortgageResponse.h>
#include <wfmcm/MortgageValuator.h>
#include <src/core/behavioral/BehavioralModelMap.h>
#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/core/ContextComponentNameManager.h>
#include <src/io/TimeSeriesReader.h>
#include <wfmcm/SofrYieldTermStructureBuilder.h>
#include <wfmcm/YieldTermStructureBuilder.h>
#include <mortgage/utility/containers/context/context_builder.h>
#include <mortgage/utility/types/polyvar/json_parser.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

using namespace app;
namespace msg = app::messages;
namespace utility_id = wf::mortgage::utility::id;

using wf::mortgage::utility::empty_value;
using wf::mortgage::utility::polyvar;
using wf::mortgage::utility::store;

namespace {

using ModelConfigDataT = app::vasara::ModelConfigData;
using BehavioralModelMapSpec = app::vasara::BehavioralModelMapSpec;
template <typename T>
using PandoInternalData = app::vasara::PandoInternalData<T>;
using RequestContextDataT = app::vasara::RequestContextData;
using RequestContextBytesT = app::vasara::RequestContextBytes;
using RequestContextHandlesT = app::vasara::RequestContextHandles;
using ResultDataT = app::vasara::ResultData;

// ----- Tunable constants (kept file-local to avoid header churn) ------------
constexpr size_t kDebugPayloadPreviewMax = 240;
constexpr size_t kSerializeJsonBufferBytes = 8192;
constexpr size_t kPricingResultJsonBufferBytes = 1024;

// Snapshot of the request-context fields needed by the pricing path.
// Used so the per-context read lock is held only for the snapshot, not across
// the full bind/execute/serialize flow.
struct PricingRequestInputs {
    Handle historicalData{NullHandle};
    Handle dateSpec{NullHandle};
    Handle modelOptions{NullHandle};
    Handle marketData{NullHandle};
    std::shared_ptr<const ModelConfigDataT> modelConfig;
};

std::vector<char> copyBytes(const char* data, int len)
{
    if (!data || len <= 0) {
        return {};
    }
    return std::vector<char>(data, data + len);
}

HandleError fillModelOptionsHandle(Handle modelOptionsHandle)
{
    const int fillRc = handleData(modelOptionsHandle).fill();
    if (fillRc == ApiSuccess) {
        return {};
    }

    HandleError fillErr = handleData(modelOptionsHandle).getError();
    if (!fillErr.isError()) {
        fillErr = HandleError{
            static_cast<ApiError>(fillRc),
            fmt::format("modelOptions fill failed (rc={})", fillRc)};
    }
    return fillErr;
}

void copyPortfolioFromHandle(
    Handle portfolio,
    wfmcm::MortgageValuationPortfolio& outPortfolio)
{
    READ_LOCK_HANDLE(portfolio);
    const auto& srcPortfolio = handleObject<HandleType::Portfolio>(portfolio);

    // Do not copy-assign pmr containers directly from a temporary handle.
    // We need stable ownership after the handle is released.
    outPortfolio.isMsr = srcPortfolio.isMsr;
    outPortfolio.msrData.assign(srcPortfolio.msrData.begin(), srcPortfolio.msrData.end());
    outPortfolio.pools.assign(srcPortfolio.pools.begin(), srcPortfolio.pools.end());
    outPortfolio.pricingInput.assign(
        srcPortfolio.pricingInput.begin(), srcPortfolio.pricingInput.end());
    outPortfolio.profitabilityInput.assign(
        srcPortfolio.profitabilityInput.begin(), srcPortfolio.profitabilityInput.end());
}

// Returns a payload preview suitable for a debug log line.
// IMPORTANT: instrument CSVs carry PII / loan-level data. The preview is
// truncated AND length-only by default. The full snippet is included only
// when PANDO_VASARA_DEBUG=full is set; callers must never enable that
// in production.
enum class PayloadPreviewMode { LengthOnly, Truncated };

// Tri-state debug verbosity controlled by env var PANDO_VASARA_DEBUG:
//   unset / 0        -> off
//   any non-empty    -> on, payload previews are length-only (production-safe)
//   "full"           -> on, payload previews include truncated content
//                      (requires PANDO_VASARA_DEBUG_ALLOW_PII=1)
// SECURITY: "full" mode must NEVER be enabled in production environments
// because it writes instrument CSV bytes (loan-level data) to the process log.
enum class VasaraDebugLevel { Off, Safe, Full };

std::string previewPayloadForDebugImpl(
    const char* data,
    int len,
    PayloadPreviewMode mode,
    size_t maxLen = kDebugPayloadPreviewMax)
{
    if (!data || len <= 0) {
        return "<empty>";
    }
    if (mode == PayloadPreviewMode::LengthOnly) {
        return std::format("<{} bytes redacted>", len);
    }
    const size_t safeLen = static_cast<size_t>(len);
    const size_t take = std::min(maxLen, safeLen);
    std::string out(data, data + take);
    std::replace(out.begin(), out.end(), '\n', ' ');
    std::replace(out.begin(), out.end(), '\r', ' ');
    return out;
}

VasaraDebugLevel vasaraDebugLevelFromEnvValuesImpl(
    const char* rawDebug,
    const char* rawAllowPii)
{
    if (!rawDebug || rawDebug[0] == '\0' || rawDebug[0] == '0') {
        return VasaraDebugLevel::Off;
    }

    std::string v(rawDebug);
    std::transform(v.begin(), v.end(), v.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (v == "full") {
        if (rawAllowPii && rawAllowPii[0] == '1' && rawAllowPii[1] == '\0') {
            return VasaraDebugLevel::Full;
        }
        return VasaraDebugLevel::Safe;
    }

    return VasaraDebugLevel::Safe;
}

PayloadPreviewMode payloadPreviewModeFromDebugLevelImpl(
    VasaraDebugLevel level)
{
    return level == VasaraDebugLevel::Full
        ? PayloadPreviewMode::Truncated
        : PayloadPreviewMode::LengthOnly;
}


// NOTE: an earlier revision kept a file-local modelConfig -> requestContext
// registry here. It was write-only (registered on create, never consulted,
// never unregistered -- generic deleteHandle cannot see file-local state), so
// it grew without bound and accumulated stale Handle keys. It has been
// removed. If session->context dependency tracking is ever needed (e.g. to
// reject destroying a session with live contexts), it must be wired into an
// explicit destroy path, not bolted onto this translation unit.

VasaraDebugLevel vasaraDebugLevel()
{
    static const VasaraDebugLevel level = [] {
        const char* raw = std::getenv("PANDO_VASARA_DEBUG");
        const char* allowPii = std::getenv("PANDO_VASARA_DEBUG_ALLOW_PII");
        return vasaraDebugLevelFromEnvValuesImpl(raw, allowPii);
    }();
    return level;
}

bool isVasaraDebugEnabled()
{
    return vasaraDebugLevel() != VasaraDebugLevel::Off;
}

PayloadPreviewMode currentPayloadPreviewMode()
{
    return payloadPreviewModeFromDebugLevelImpl(vasaraDebugLevel());
}

std::mutex g_logMutex;

void logVasaraDebug(const std::string& msg)
{
    if (!isVasaraDebugEnabled()) {
        return;
    }
    // Serialize writes across worker threads so log lines from concurrent
    // calcValueForInstrument calls do not interleave at byte granularity.
    std::lock_guard lock(g_logMutex);
    std::clog << "[WfmcmVasaraApi] " << msg << '\n';
}

/**
 * Applies the behavioral model map to a portfolio directly from parsed spec
 * data.  Bypasses the Handle-based mapBehavioralModel() public API — the
 * spec is internal data, never exposed to Java, and has no reason to live in
 * HandleContainer.
 *
 * @return ApiSuccess on success; on failure, sets the error on the portfolio
 *         handle and returns the error code.
 */
HandleError applyBehavioralModelMapFromSpecImpl(
    Handle portfolio,
    const wfmcm::ModelParamSpec& specParams,
    Date factorDate)
{
    // Validate that ModelParams is present.
    auto it = specParams.find(wfmcm::ModelParamName::ModelParams);
    if (it == specParams.end() || it->second.empty()) {
        return {ApiErrorInvalidModelParameter,
            "Behavioral model map spec has no ModelParams entry"};
    }

    WRITE_LOCK_HANDLE(portfolio);
    wfmcm::BehavioralModelMapBuilder builder;
    auto behavModelMap = builder.withSpec(specParams).build();
    auto& pools = handleObject<HandleType::Portfolio>(portfolio).pools;
    for (size_t i = 0; i < pools.size(); ++i) {
        ensure_dates(pools,
            std::chrono::year_month_day(
                QuantLib::fromYYYYMMDD(factorDate.yyyymmdd_)));
        behavModelMap.assignSubModelType(pools[i]);
    }
    return {};
}

const char* serializeResponseToApiBuffer(msg::Response&& response)
{
    wfmcm::json_options options;
    options.json_allocator_buffer_size(kSerializeJsonBufferBytes);
    wfmcm::json_parser parser(options);
    auto& str = store<std::string, utility_id::api_response>();
    polyvar p;
    p << response;
    str = parser.serialize(p);
    return str.c_str();
}

Handle makeResultHandle(msg::Response&& response)
{
    auto hd = std::make_shared<app::HandleData>(
        HandleType::ResultData,
        ResultDataT{ std::move(response) });
    Handle h = handles->addHandleData(std::move(hd));
    return h;
}

const wfmcm::PricingInput* firstPricingInput(const msg::Response& response)
{
    if (response.isCalcValueForMortgageResponse()) {
        const auto& prices = response.calcValueForMortgageResponse().prices_;
        if (!prices.empty() && !prices[0].empty()) {
            return &prices[0][0];
        }
    }
    if (response.isCalcValueForMortgageFromRatesResponse()) {
        const auto& prices = response.calcValueForMortgageFromRatesResponse().prices_;
        if (!prices.empty()) {
            return &prices[0];
        }
    }
    return nullptr;
}

const char* emptyApiString()
{
    return empty_value<std::string>.c_str();
}

const char* storeApiString(std::string value)
{
    auto& str = store<std::string, utility_id::api_response>();
    str = std::move(value);
    return str.c_str();
}

// Dedicated per-thread buffer for error text. resultGetError must NOT alias
// api_response: that slot holds the payload returned by the const char* calc
// APIs, so a later resultGetError() on the same thread would overwrite the
// caller's payload pointer.
const char* storeResultErrorString(std::string value)
{
    static thread_local std::string str;
    str = std::move(value);
    return str.c_str();
}

template <typename ComponentGetter>
const char* getRatePathComponentJson(
    Handle ratePaths,
    ComponentGetter&& getComponent)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", emptyApiString());
    }
    CHECK_HANDLE2(emptyApiString(), ratePaths, HandleType::CalcRatePaths);
    READ_LOCK_HANDLE(ratePaths);
    const auto& paths = handleObject<HandleType::CalcRatePaths>(ratePaths);

    // Getter returns nullptr when the requested component is absent. Absence is a
    // legitimate state, not a failure, so we return an empty string WITHOUT setting
    // the per-thread error indicator. Callers distinguish "absent component" from
    // "failure" by checking the error indicator: it is set only on the failure
    // paths above (uninitialized library, null/wrong-type handle) and in
    // EXCEPTION_ERROR below. See the getter contract in WfmcmVasaraApi.h.
    const auto* component = getComponent(paths);
    if (!component) {
        return emptyApiString();
    }

    wfmcm::json_options options;
    options.json_allocator_buffer_size(kSerializeJsonBufferBytes);
    wfmcm::json_parser parser(options);
    polyvar p;
    p << *component;
    return storeApiString(parser.serialize(p));

    EXCEPTION_ERROR(ratePaths, emptyApiString());
}

HandleError applyPrimaryRatePathsFromCsv(
    Date asOf,
    const char* content,
    int contentLen,
    wfmcm::RatePathsSet<wfmcm::PrimaryRateType>& ratePaths,
    wfmcm::RatePathsSet<wfmcm::KeyRate>& keyRatePaths)
{
    if (!content || contentLen <= 0) {
        return {ApiErrorNullContent, "Rate paths content is empty"};
    }
    auto primaryRatesTs = wfmcm::io::TimeSeriesReader::readFromCsv({content, static_cast<size_t>(contentLen)});
    for (auto& item : primaryRatesTs) {
        const bool withinTsDateRange =
            QuantLib::monthsBetween(item.second.startDate(), QuantLib::fromYYYYMMDD(asOf.yyyymmdd_)) >= 0
            && QuantLib::monthsBetween(QuantLib::fromYYYYMMDD(asOf.yyyymmdd_), item.second.endDate()) >= 0;

        std::optional<wfmcm::PrimaryRateType> optPrimRateType;
        wf::mortgage::utility::strings::try_from_string(item.first, &optPrimRateType);
        if (optPrimRateType) {
            if (withinTsDateRange) {
                ratePaths[optPrimRateType.value()].emplace_back(
                    item.second.at(QuantLib::fromYYYYMMDD(asOf.yyyymmdd_)),
                    item.second.end());
            }
        }

        std::optional<wfmcm::KeyRate> optKeyRateType;
        wf::mortgage::utility::strings::try_from_string(item.first, &optKeyRateType);
        if (optKeyRateType) {
            if (withinTsDateRange) {
                keyRatePaths[optKeyRateType.value()].emplace_back(
                    item.second.at(QuantLib::fromYYYYMMDD(asOf.yyyymmdd_)),
                    item.second.end());
            }
        }
    }
    return {};
}

HandleError buildBehavioralRequestFromContext(
const RequestContextDataT& ctx,
const char* instrumentData,
int instrumentDataLen,
const char* ratePaths,
int ratePathsLen,
app::messages::CalcBehavioralSpeedFromPrimaryRateRequest& out)
{
    const auto* ctxHandles = std::get_if<RequestContextHandlesT>(&ctx.payload_);
    if (!ctxHandles) {
        return {ApiErrorOperationNotSupported,
            "Request context was not built from handles; use createRequestContextFromHandles"};
    }
    if (!instrumentData || instrumentDataLen <= 0) {
        return {ApiErrorNullContent, "Instrument portfolio CSV is empty"};
    }

    if (!isValidHandle(ctxHandles->historicalDataHandle_)
        || !isHandleTypeAnyOf(ctxHandles->historicalDataHandle_, {HandleType::HistoricalData})) {
        return {ApiErrorWrongHandleType, "historicalData handle is invalid"};
    }
    if (!isValidHandle(ctxHandles->dateSpecHandle_)
        || !isHandleTypeAnyOf(ctxHandles->dateSpecHandle_, {HandleType::DateSpec})) {
        return {ApiErrorWrongHandleType, "dateSpec handle is invalid"};
    }
    if (!isValidHandle(ctxHandles->modelOptionsHandle_)
        || !isHandleTypeAnyOf(ctxHandles->modelOptionsHandle_, {HandleType::ModelOptions})) {
        return {ApiErrorWrongHandleType, "modelOptions handle is invalid"};
    }

    // Fill modelOptions so all bound model specs (CFPM, PLDM, IR, Secondary, etc.)
    // added via addModelOptionsSpec() are materialised into the ModelOptions struct
    // before we read it below.  The normal execute() path does this automatically
    // inside HandleData::fill(); the vasara path must do it explicitly here
    // because createRequestContextFromHandles only stores the live handle reference
    // without ever calling fill().
    //
    // fill() acquires its own internal write lock on the handle, so we must NOT
    // hold any read lock on modelOptionsHandle_ at this point.
    const HandleError fillErr = fillModelOptionsHandle(ctxHandles->modelOptionsHandle_);
    if (fillErr.isError()) {
        return fillErr;
    }

    // READ_LOCK_HANDLE uses token-pasting on its argument, so it requires a
    // plain identifier. Copy member handles into locals before locking.
    const Handle historicalDataH = ctxHandles->historicalDataHandle_;
    const Handle dateSpecH = ctxHandles->dateSpecHandle_;
    const Handle modelOptionsH = ctxHandles->modelOptionsHandle_;
    READ_LOCK_HANDLE(historicalDataH);
    READ_LOCK_HANDLE(dateSpecH);
    READ_LOCK_HANDLE(modelOptionsH);

    const auto histErr = fillHistoricalData(
        ctxHandles->historicalDataHandle_,
        handleObject<HandleType::HistoricalData>(ctxHandles->historicalDataHandle_),
        &out.historicalData_);
    if (histErr.isError()) {
        return histErr;
    }

    const auto& dateSpec = handleObject<HandleType::DateSpec>(ctxHandles->dateSpecHandle_);
    if (dateSpec.getValuationDate().serialNumber() == 0) {
        return {ApiErrorInvalidDate, "Valuation date not set on date spec handle"};
    }
    out.dateSpec_ = dateSpec;

    const auto& modelOptions = handleObject<HandleType::ModelOptions>(ctxHandles->modelOptionsHandle_);
    out.prepayModelOptions_ = modelOptions.prepayModelOptions_;
    out.lossModelOptions_ = modelOptions.lossModelOptions_;

    // Use getPrimRateAsOfDate() — already falls back to getValuationDate() when unset
    // (see DateSpec in MessageComponents.h). Java sets this to the historical rate
    // reference month (e.g. 2020-03-30) so dpssHistory.csv rows are found correctly.
    // Using getValuationDate() (e.g. 2023-04-30) here misses all historical rows.
    const QuantLib::Date primRateAsOfQl = dateSpec.getPrimRateAsOfDate();
    const QuantLib::Date valuationDateQl = dateSpec.getValuationDate();
    const bool primRateAsOfImplicitlyDefaulted = (primRateAsOfQl == valuationDateQl);

    Date asOf{};
    asOf.yyyymmdd_ = static_cast<unsigned int>(QuantLib::toYYYYMMDD(primRateAsOfQl));
    const auto rateErr = applyPrimaryRatePathsFromCsv(
        asOf,
        ratePaths,
        ratePathsLen,
        out.ratePaths_,
        out.keyRatePaths_);
    if (rateErr.isError()) {
        return rateErr;
    }

    // Defensive guard: avoid downstream vector indexing assertions when the
    // selected as-of date does not map to any primary/key rate path rows.
    // Most common cause: caller forgot setPrimaryRateAsOfDate(...) and we
    // silently fell back to the valuation date, which typically post-dates
    // the historical rate CSV coverage window.
    if (out.ratePaths_.empty() && out.keyRatePaths_.empty()) {
        const unsigned int valuationYyyymmdd =
            static_cast<unsigned int>(QuantLib::toYYYYMMDD(valuationDateQl));
        std::string hint;
        if (primRateAsOfImplicitlyDefaulted) {
            hint = fmt::format(
                " Hint: primaryRateAsOfDate was not set explicitly and defaulted to the "
                "valuation date ({}); call setPrimaryRateAsOfDate(dateSpec, <historical "
                "rate reference date>) before createRequestContextFromHandles.",
                valuationYyyymmdd);
        } else {
            hint = fmt::format(
                " Hint: primaryRateAsOfDate ({}) is outside the rate paths CSV coverage "
                "window; verify it matches the historical rate reference date used to "
                "produce the CSV.",
                asOf.yyyymmdd_);
        }
        return {
            ApiErrorEmptyRates,
            fmt::format(
                "No primary/key rate paths available for as-of date {} "
                "(valuationDate={}).{}",
                asOf.yyyymmdd_, valuationYyyymmdd, hint)
        };
    }

    app::ScopedHandle portfolio(
        createInstrumentPortfolio(ApiPortfolioType_Mbs, instrumentData, instrumentDataLen));
    if (!portfolio) {
        return {ApiErrorOperationNotSupported, "Failed to create instrument portfolio"};
    }

    if (ctx.modelConfig_ && ctx.modelConfig_->hasModelMap()) {
        const QuantLib::Date qlFactor = dateSpec.getMarketCurveDate();
        const Date factorDate{
            static_cast<unsigned int>(QuantLib::toYYYYMMDD(qlFactor))};
        const auto mapErr = applyBehavioralModelMapFromSpecImpl(
            portfolio,
            ctx.modelConfig_->modelMapParams(),
            factorDate);
        if (mapErr.isError()) {
            return mapErr;
        }
    }

    copyPortfolioFromHandle(portfolio, out.portfolio_);

    (void)ctxHandles->marketDataHandle_;
    return {};
}

// Builds a GenRatePathsForMortgageValuationRequest from a request context that
// was created via createRequestContextFromHandles. Mirrors
// buildBehavioralRequestFromContext: validates the bound handles, materialises
// the model-options specs (fill()) before reading them, and snapshots the
// market/historical/date-spec/model-options inputs the rate-gen engine needs.
// Every base-scenario path type is requested (swap/SOFR, UST, primary,
// secondary, discount); no greek, user, or horizon scenarios are generated
// (empty greek spec and no scenarios_/scenarioGroupMap_).
HandleError buildRatePathsRequestFromContext(
    const RequestContextDataT& ctx,
    app::messages::GenRatePathsForMortgageValuationRequest& out)
{
    const auto* ctxHandles = std::get_if<RequestContextHandlesT>(&ctx.payload_);
    if (!ctxHandles) {
        return {ApiErrorOperationNotSupported,
            "Request context was not built from handles; use createRequestContextFromHandles"};
    }

    if (!isValidHandle(ctxHandles->historicalDataHandle_)
        || !isHandleTypeAnyOf(ctxHandles->historicalDataHandle_, {HandleType::HistoricalData})) {
        return {ApiErrorWrongHandleType, "historicalData handle is invalid"};
    }
    if (!isValidHandle(ctxHandles->dateSpecHandle_)
        || !isHandleTypeAnyOf(ctxHandles->dateSpecHandle_, {HandleType::DateSpec})) {
        return {ApiErrorWrongHandleType, "dateSpec handle is invalid"};
    }
    if (!isValidHandle(ctxHandles->modelOptionsHandle_)
        || !isHandleTypeAnyOf(ctxHandles->modelOptionsHandle_, {HandleType::ModelOptions})) {
        return {ApiErrorWrongHandleType, "modelOptions handle is invalid"};
    }
    if (!isValidHandle(ctxHandles->marketDataHandle_)
        || !isHandleTypeAnyOf(ctxHandles->marketDataHandle_, {HandleType::MarketData})) {
        return {ApiErrorWrongHandleType, "marketData handle is invalid"};
    }

    // Materialise bound model specs before reading ModelOptions (see
    // buildBehavioralRequestFromContext for the rationale). fill() acquires its
    // own internal write lock, so it must run before we take the read lock on
    // modelOptionsHandle_ below.
    const HandleError fillErr = fillModelOptionsHandle(ctxHandles->modelOptionsHandle_);
    if (fillErr.isError()) {
        return fillErr;
    }

    // READ_LOCK_HANDLE token-pastes its argument, so it needs plain identifiers.
    const Handle historicalDataH = ctxHandles->historicalDataHandle_;
    const Handle dateSpecH = ctxHandles->dateSpecHandle_;
    const Handle modelOptionsH = ctxHandles->modelOptionsHandle_;
    const Handle marketDataH = ctxHandles->marketDataHandle_;
    READ_LOCK_HANDLE(historicalDataH);
    READ_LOCK_HANDLE(dateSpecH);
    READ_LOCK_HANDLE(modelOptionsH);
    READ_LOCK_HANDLE(marketDataH);

    const auto histErr = fillHistoricalData(
        historicalDataH,
        handleObject<HandleType::HistoricalData>(historicalDataH),
        &out.historicalData_);
    if (histErr.isError()) {
        return histErr;
    }

    const auto mktErr = fillMarketData(
        marketDataH,
        handleObject<HandleType::MarketData>(marketDataH),
        &out.marketData_);
    if (mktErr.isError()) {
        return mktErr;
    }

    const auto& dateSpec = handleObject<HandleType::DateSpec>(dateSpecH);
    if (dateSpec.getValuationDate().serialNumber() == 0) {
        return {ApiErrorInvalidDate, "Valuation date not set on date spec handle"};
    }
    out.dateSpec_ = dateSpec;

    out.modelOptions_ = handleObject<HandleType::ModelOptions>(modelOptionsH);

    // Base scenario only: no greek scenarios. Request every base-scenario path
    // type the engine can produce (swap/SOFR, UST, primary, secondary, discount)
    // so genRatePaths can return the full CalcRatePaths result.
    out.greekSpec_ = {};
    out.requestedOutput_ = {
        PathOutputType::BaseSwapRates,
        PathOutputType::BaseUstSwapRates,
        PathOutputType::BasePrimaryRates,
        PathOutputType::BaseSecondaryRates,
        PathOutputType::BaseDiscountRates};

    // Primary rate projection is strictly demand-driven: the engine only projects
    // PrimaryRateType values present in requiredPrimaryRateTypes_. Leaving it empty
    // yields an empty paths.primaryRates_ scenario map, which then crashes any
    // downstream behavioral model (e.g. Cfpm) that consumes the rate manager via
    // calcValueForMortgageFromRates.
    //
    // The correct set for a model-independent, reusable cache is the intersection
    // of what the configured primary rate model supports and what the bound
    // HistoricalData actually carries. Requesting a spec-supported-but-historically-
    // absent type (e.g. fhmrate_icon in a DPSS spec whose CSV omits that column)
    // makes the DPSS *session* builder throw at doCalcSpread() because it cannot
    // compute a spread without the historical row.
    //
    // Secondary rates are intentionally left alone: the secondary basis model
    // computes its natural output set (FN30/FN15/GN30/GN15 etc.) as a byproduct of
    // projecting the mortgage basis over the swap curve, regardless of the
    // requiredSecondaryRateTypes_ demand list. Populating that list would only add
    // validation constraints, not projected content.
    if (out.modelOptions_.primaryRateModelOptions_.empty()) {
        return {ApiErrorInvalidModelParameter,
            "modelOptions has no primary rate model spec; cannot determine supported "
            "primary rate types for genRatePaths"};
    }
    // The rate-gen path projects against a single primary rate model. More than
    // one entry has no defined selection semantics here, so picking one (e.g. the
    // first map entry) would silently drop the others and could change the
    // required-primary-type set as configurations evolve. Reject it explicitly
    // instead of resolving it arbitrarily.
    if (out.modelOptions_.primaryRateModelOptions_.size() > 1) {
        return {ApiErrorInvalidModelParameter,
            "modelOptions has more than one primary rate model spec; genRatePaths "
            "supports exactly one. Configure a single primary rate model."};
    }
    try {
        const auto& primOpts = *out.modelOptions_.primaryRateModelOptions_.begin();
        auto primaryModel = wfmcm::PrimaryMortgageRateModelBuilder(primOpts.first)
            .withSpec(primOpts.second.modelSpec_)
            .build();
        auto supportedPrimaryTypes = primaryModel.supportedRateTypes();

        // Enumerate primary rate types present in the historical primary rate map
        // (keyed by rate-type string; same try_from_string conversion used by
        // applyPrimaryRatePathsFromCsv above).
        std::set<wfmcm::PrimaryRateType> historicalPrimaryTypes;
        for (const auto& [rateTypeStr, series] : out.historicalData_.mbsPrimaryRates_) {
            std::optional<wfmcm::PrimaryRateType> optRateType;
            wf::mortgage::utility::strings::try_from_string(rateTypeStr, &optRateType);
            if (optRateType) {
                historicalPrimaryTypes.insert(*optRateType);
            }
        }

        out.requiredPrimaryRateTypes_.reserve(supportedPrimaryTypes.size());
        for (auto primaryRateType : supportedPrimaryTypes) {
            if (historicalPrimaryTypes.count(primaryRateType) != 0) {
                out.requiredPrimaryRateTypes_.push_back(primaryRateType);
            }
        }
        if (out.requiredPrimaryRateTypes_.empty()) {
            return {ApiErrorInvalidModelParameter,
                "No primary rate types are both supported by the configured primary "
                "model and present in historicalData.mbsPrimaryRates; nothing to "
                "project. Verify the historical primary rate CSV covers the primary "
                "rate model spec's supported rate types."};
        }
    } catch (const LibException& ex) {
        return {ApiErrorInvalidModelParameter,
            fmt::format(
                "Failed to enumerate supported primary rate types from modelOptions: {}",
                ex.what())};
    }

    return {};
}

// Fills a CalcValueForMortgageFromRatesRequest from a request context and an
// instrument CSV. Snapshots the market/historical/date-spec/model-options inputs
// (like buildRatePathsRequestFromContext) and builds the instrument portfolio with
// the optional behavioral-model map (like buildPricingRequestFromContext). The
// full precomputed rate-path set is injected by the caller as overrideRatePaths_;
// the handler derives the base-context model-wiring seed from that set, so there
// is no separate seed field to populate here. pricingOutputType selects
// Price / OAS / ConstantYield on the pricing spec; cleanPrice selects
// clean (ex-accrued) vs dirty (with accrued) price handling.
HandleError buildCalcValueForMortgageFromRatesRequestFromContext(
    const RequestContextDataT& ctx,
    const char* instrumentData,
    int instrumentDataLen,
    wfmcm::PricingOutputType pricingOutputType,
    bool cleanPrice,
    app::messages::CalcValueForMortgageFromRatesRequest& out)
{
    const auto* ctxHandles = std::get_if<RequestContextHandlesT>(&ctx.payload_);
    if (!ctxHandles) {
        return {ApiErrorOperationNotSupported,
            "Request context was not built from handles; use createRequestContextFromHandles"};
    }
    if (!instrumentData || instrumentDataLen <= 0) {
        return {ApiErrorNullContent, "Instrument portfolio CSV is empty"};
    }

    if (!isValidHandle(ctxHandles->historicalDataHandle_)
        || !isHandleTypeAnyOf(ctxHandles->historicalDataHandle_, {HandleType::HistoricalData})) {
        return {ApiErrorWrongHandleType, "historicalData handle is invalid"};
    }
    if (!isValidHandle(ctxHandles->dateSpecHandle_)
        || !isHandleTypeAnyOf(ctxHandles->dateSpecHandle_, {HandleType::DateSpec})) {
        return {ApiErrorWrongHandleType, "dateSpec handle is invalid"};
    }
    if (!isValidHandle(ctxHandles->modelOptionsHandle_)
        || !isHandleTypeAnyOf(ctxHandles->modelOptionsHandle_, {HandleType::ModelOptions})) {
        return {ApiErrorWrongHandleType, "modelOptions handle is invalid"};
    }
    if (!isValidHandle(ctxHandles->marketDataHandle_)
        || !isHandleTypeAnyOf(ctxHandles->marketDataHandle_, {HandleType::MarketData})) {
        return {ApiErrorWrongHandleType, "marketData handle is invalid"};
    }

    // Materialise bound model specs before reading ModelOptions (fill() takes its
    // own write lock, so it must run before we take the read lock below).
    const HandleError fillErr = fillModelOptionsHandle(ctxHandles->modelOptionsHandle_);
    if (fillErr.isError()) {
        return fillErr;
    }

    const Handle historicalDataH = ctxHandles->historicalDataHandle_;
    const Handle dateSpecH = ctxHandles->dateSpecHandle_;
    const Handle modelOptionsH = ctxHandles->modelOptionsHandle_;
    const Handle marketDataH = ctxHandles->marketDataHandle_;
    READ_LOCK_HANDLE(historicalDataH);
    READ_LOCK_HANDLE(dateSpecH);
    READ_LOCK_HANDLE(modelOptionsH);
    READ_LOCK_HANDLE(marketDataH);

    const auto histErr = fillHistoricalData(
        historicalDataH,
        handleObject<HandleType::HistoricalData>(historicalDataH),
        &out.historicalData_);
    if (histErr.isError()) {
        return histErr;
    }

    const auto mktErr = fillMarketData(
        marketDataH,
        handleObject<HandleType::MarketData>(marketDataH),
        &out.marketData_);
    if (mktErr.isError()) {
        return mktErr;
    }

    const auto& dateSpec = handleObject<HandleType::DateSpec>(dateSpecH);
    if (dateSpec.getValuationDate().serialNumber() == 0) {
        return {ApiErrorInvalidDate, "Valuation date not set on date spec handle"};
    }
    out.dateSpec_ = dateSpec;

    out.modelOptions_ = handleObject<HandleType::ModelOptions>(modelOptionsH);

    // Pricing spec: caller-selected output type (Price / OAS / ConstantYield),
    // caller-selected clean/dirty price handling, own OAS (base scenario). For
    // Price output cleanPrice picks which figure is produced; for OAS /
    // ConstantYield it picks how the instrument CSV target price is interpreted.
    app::messages::PricingSpec pricingSpec(pricingOutputType);
    pricingSpec.setBaseOASFlag(false);
    pricingSpec.setCleanPriceFlag(cleanPrice);
    out.pricingSpec_ = pricingSpec;

    // Build the instrument portfolio, apply the behavioral model map when the
    // request context carries one, then copy the pools into the request. The
    // portfolio handle is released when this scope exits.
    app::ScopedHandle portfolio(createInstrumentPortfolio(
        ApiPortfolioType_Mbs, instrumentData, instrumentDataLen));
    if (!portfolio) {
        return {ApiErrorOperationNotSupported, "Failed to create instrument portfolio"};
    }
    if (ctx.modelConfig_ && ctx.modelConfig_->hasModelMap()) {
        const QuantLib::Date qlFactor = dateSpec.getMarketCurveDate();
        const Date factorDate{static_cast<unsigned int>(QuantLib::toYYYYMMDD(qlFactor))};
        const auto mapErr = applyBehavioralModelMapFromSpecImpl(
            portfolio, ctx.modelConfig_->modelMapParams(), factorDate);
        if (mapErr.isError()) {
            return mapErr;
        }
    }
    copyPortfolioFromHandle(portfolio, out.portfolio_);

    // Derive an identity for the immutable base-context prefix so it can be built
    // once and reused across requests. Folds the model config, the bound historical-
    // data and model-options handles, and the instrument bytes (which determine the
    // portfolio pools and thus the required index set). The override rate paths are
    // deliberately excluded so the same instrument repriced under different paths
    // reuses the prefix. Correctness comes from the key alone, so no opt-in is needed:
    // any input change yields a different id and a fresh prefix.
    if (ctx.modelConfig_) {
        std::size_t id = app::vasara::hashModelConfig(*ctx.modelConfig_);
        id = mixHash(id, ctxHandles->historicalDataHandle_.internal_);
        id = mixHash(id, ctxHandles->modelOptionsHandle_.internal_);
        // Guard the empty case: forming a string_view from a null pointer is not
        // well-defined even with size 0. (The caller already rejects empty input;
        // this keeps the hash self-contained.)
        const std::size_t instrumentBytesHash =
            (instrumentData && instrumentDataLen > 0)
                ? std::hash<std::string_view>{}(
                    std::string_view(instrumentData, static_cast<std::size_t>(instrumentDataLen)))
                : 0;
        id = mixHash(id, instrumentBytesHash);
        id = mixHash(id, out.indexMapper_.getHash());
        out.baseContextPrefixCacheId_ = id;

        // Extend the prefix identity into a full base-context identity by folding
        // the market-data and date-spec handle ids and the pricing intent (output
        // type + clean/dirty), which together determine the per-request base
        // market/calculation contexts (curves, vols, dates, discounting session).
        // Simulation months and the greek spec are invariant on this path, so they
        // are not folded. Any input change yields a different id and a fresh base
        // context; correctness comes from the key alone.
        std::size_t fullId = mixHash(id,
            ctxHandles->marketDataHandle_.internal_);
        fullId = mixHash(fullId, ctxHandles->dateSpecHandle_.internal_);
        fullId = mixHash(fullId, static_cast<std::size_t>(pricingOutputType));
        fullId = mixHash(fullId, cleanPrice ? 1u : 0u);
        out.baseContextCacheId_ = fullId;
    }

    return {};
}

// ---- Helpers for buildPricingRequestFromContext ----------------------------

// Retrieves the error from a handle, falling back to a synthesized error
// when the handle carries no error state.
HandleError handleErrFromHandleOrFallbackImpl(Handle h, const char* fallback)
{
    HandleError err = handleData(h).getError();
    return err.isError() ? err : HandleError{ApiErrorException, fallback};
}

HandleError createAndBindPricingSpec(Handle request, bool debug);
HandleError bindPricingInputHandles(
    Handle request,
    const PricingRequestInputs& in,
    bool debug);
HandleError createAndBindPortfolio(
    Handle request,
    const PricingRequestInputs& in,
    const char* instrumentData,
    int instrumentDataLen,
    bool debug);
HandleError buildPricingRequestFromContext(
    const PricingRequestInputs& in,
    const char* instrumentData,
    int instrumentDataLen,
    app::ScopedHandle& outRequest);

// Validates that all required input handles are live and of the expected type.
HandleError validatePricingInputHandles(const PricingRequestInputs& in, bool debug)
{
    struct Check { Handle h; HandleType type; const char* name; };
    const Check checks[] = {
        {in.historicalData, HandleType::HistoricalData, "historicalData"},
        {in.dateSpec,       HandleType::DateSpec,       "dateSpec"},
        {in.modelOptions,   HandleType::ModelOptions,   "modelOptions"},
        {in.marketData,     HandleType::MarketData,     "marketData"},
    };
    for (const auto& [h, type, name] : checks) {
        if (debug) {
            logVasaraDebug(std::format("calcValueForInstrument check: validating {}", name));
        }
        if (!isValidHandle(h) || !isHandleTypeAnyOf(h, {type})) {
            if (debug) {
                logVasaraDebug(std::format(
                    "calcValueForInstrument rejected: {} handle invalid h={} isValid={} hasType={}",
                    name, h.internal_, isValidHandle(h), isHandleTypeAnyOf(h, {type})));
            }
            return {ApiErrorWrongHandleType, std::format("{} handle is invalid", name)};
        }
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument check: all handles valid");
    }
    return {};
}

// Creates a pricing spec (BaseOAS=false, CleanPrice=true) and binds it to
// the request.
HandleError createAndBindPricingSpec(Handle request, bool debug)
{
    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: createPricingSpec(ApiPricingOutputType_Price)");
    }
    app::ScopedHandle pricingSpec(createPricingSpec(ApiPricingOutputType_Price));
    if (!pricingSpec) {
        if (debug) {
            logVasaraDebug("calcValueForInstrument failed: createPricingSpec returned null");
        }
        return {ApiErrorOperationNotSupported, "Failed to create pricing spec"};
    }
    if (setBaseOASFlag(pricingSpec, false) != ApiSuccess) {
        return handleErrFromHandleOrFallbackImpl(
            pricingSpec,
            "setBaseOASFlag(pricingSpec) failed");
    }
    if (setCleanPriceFlag(pricingSpec, true) != ApiSuccess) {
        return handleErrFromHandleOrFallbackImpl(
            pricingSpec,
            "setCleanPriceFlag(pricingSpec) failed");
    }
    if (bindPricingSpec(request, pricingSpec) != ApiSuccess) {
        return handleErrFromHandleOrFallbackImpl(request, "bindPricingSpec failed");
    }
    return {};
}

// Binds historicalData, dateSpec, modelOptions, and marketData to the request.
HandleError bindPricingInputHandles(
    Handle request, const PricingRequestInputs& in, bool debug)
{
    struct Binding { Handle h; int(*fn)(Handle, Handle); const char* name; };
    const Binding bindings[] = {
        {in.historicalData, bindHistoricalData, "bindHistoricalData"},
        {in.dateSpec,       bindDateSpec,       "bindDateSpec"},
        {in.modelOptions,   bindModelOptions,   "bindModelOptions"},
        {in.marketData,     bindMarketData,     "bindMarketData"},
    };
    for (const auto& [h, fn, name] : bindings) {
        if (debug) {
            logVasaraDebug(std::format("calcValueForInstrument stage: {}", name));
        }
        if (fn(request, h) != ApiSuccess) {
            return handleErrFromHandleOrFallbackImpl(request, name);
        }
    }
    return {};
}

// Creates the instrument portfolio, applies the behavioral model map when
// present, and binds the portfolio to the request.
HandleError createAndBindPortfolio(
    Handle request,
    const PricingRequestInputs& in,
    const char* instrumentData,
    int instrumentDataLen,
    bool debug)
{
    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: createInstrumentPortfolio(ApiPortfolioType_Mbs)");
    }
    app::ScopedHandle portfolio(createInstrumentPortfolio(
        ApiPortfolioType_Mbs, instrumentData, instrumentDataLen));
    if (!portfolio) {
        if (debug) {
            logVasaraDebug("calcValueForInstrument failed: createInstrumentPortfolio returned null");
        }
        return {ApiErrorOperationNotSupported, "Failed to create instrument portfolio"};
    }

    if (in.modelConfig && in.modelConfig->hasModelMap()) {
        if (debug) {
            logVasaraDebug("calcValueForInstrument applying behavioral model map");
        }
        const Handle dateSpecH = in.dateSpec;
        READ_LOCK_HANDLE(dateSpecH);
        const auto& dateSpec = handleObject<HandleType::DateSpec>(dateSpecH);
        const QuantLib::Date qlFactor = dateSpec.getMarketCurveDate();
        const Date factorDate{static_cast<unsigned int>(QuantLib::toYYYYMMDD(qlFactor))};
        const auto mapErr = applyBehavioralModelMapFromSpecImpl(
            portfolio, in.modelConfig->modelMapParams(), factorDate);
        if (mapErr.isError()) {
            return mapErr;
        }
    }

    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: bindInstrumentPortfolio");
    }
    if (bindInstrumentPortfolio(request, portfolio) != ApiSuccess) {
        return handleErrFromHandleOrFallbackImpl(
            request,
            "bindInstrumentPortfolio failed");
    }
    return {};
}

// ---- End helpers ----------------------------------------------------------

// Shared pricing request builder for calcValueForInstrument /
// calcValueForInstrumentObj. Validates handles, configures the pricing spec
// (BaseOAS=false, CleanPrice=true), builds the instrument portfolio (with
// optional behavioral-model mapping), and binds everything to `outRequest`.
// On success `outRequest` owns the fully bound request handle and the caller
// is responsible for `execute(outRequest)` and reading the result.
HandleError buildPricingRequestFromContext(
    const PricingRequestInputs& in,
    const char* instrumentData,
    int instrumentDataLen,
    app::ScopedHandle& outRequest)
{
    const bool debug = isVasaraDebugEnabled();
    if (debug) {
        logVasaraDebug(std::format(
            "calcValueForInstrument start: instrumentLen={} historical={} dateSpec={} modelOptions={} marketData={}",
            instrumentDataLen,
            in.historicalData.internal_,
            in.dateSpec.internal_,
            in.modelOptions.internal_,
            in.marketData.internal_));
        logVasaraDebug(std::format(
            "calcValueForInstrument instrument csv preview={}",
            previewPayloadForDebugImpl(
                instrumentData,
                instrumentDataLen,
                currentPayloadPreviewMode())));
    }

    const auto valErr = validatePricingInputHandles(in, debug);
    if (valErr.isError()) return valErr;

    app::ScopedHandle request(createRequest(ApiRequest_CalcValueForMortgage));
    if (!request) {
        if (debug) {
            logVasaraDebug("calcValueForInstrument failed: createRequest returned null");
        }
        return {ApiErrorOperationNotSupported, "Failed to create CalcValueForMortgage request"};
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: request created");
    }

    const auto specErr = createAndBindPricingSpec(request, debug);
    if (specErr.isError()) return specErr;

    const auto bindErr = bindPricingInputHandles(request, in, debug);
    if (bindErr.isError()) return bindErr;

    const auto portErr = createAndBindPortfolio(
        request, in, instrumentData, instrumentDataLen, debug);
    if (portErr.isError()) return portErr;

    outRequest = std::move(request);
    return {};
}

} // namespace

Handle createModelConfig(
    const char* modelParams,
    int modelParamsLen,
    const char* modelMapCsv,
    int modelMapCsvLen)
{
    TRY

    CHECK_INIT2;

    ModelConfigDataT config{
        copyBytes(modelParams, modelParamsLen),
        copyBytes(modelMapCsv, modelMapCsvLen)};

    // Parse the behavioral model map CSV into a spec stored directly in the
    // config — no Handle registration, no custom deleter.
    if (!config.modelMapCsv_.empty()) {
        wfmcm::ModelParamSpec params;
        params[wfmcm::ModelParamName::ModelParams] =
            std::string(config.modelMapCsv_.begin(), config.modelMapCsv_.end());
        config.modelMapSpec_ = PandoInternalData<BehavioralModelMapSpec>(
            BehavioralModelMapSpec{std::move(params)});
    }

    auto hd = std::make_shared<app::HandleData>(
        HandleType::ModelConfigData,
        std::move(config));

    Handle h = handles->addHandleData(std::move(hd));
    RETURN_SUCCESS(h, h);

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

Handle createRequestContext(
    Handle modelConfig,
    const char* historicalData,
    int historicalDataLen,
    const char* marketData,
    int marketDataLen,
    Date valuationDate)
{
    TRY

    CHECK_INIT2;
    CHECK_HANDLE2(NullHandle, modelConfig, HandleType::ModelConfigData);

    // Bytes variant is reserved for future cross-JVM / Grid distribution.
    // DTO parsing is not yet wired, so non-empty payloads cannot be honored.
    // Reject them up-front so the failure surfaces here rather than as a
    // silent wrong-result downstream in calc*ForInstrument.
    if ((historicalData != nullptr && historicalDataLen > 0)
        || (marketData != nullptr && marketDataLen > 0)) {
        RETURN_ERROR(
            modelConfig,
            ApiErrorOperationNotSupported,
            "createRequestContext (bytes variant) does not yet support "
            "non-empty payloads; use createRequestContextFromHandles",
            NullHandle);
    }

    READ_LOCK_HANDLE(modelConfig);
    auto& modelObj = handleObject<HandleType::ModelConfigData>(modelConfig);
    auto modelCopy = std::make_shared<const ModelConfigDataT>(modelObj);

    RequestContextDataT ctx{
        modelConfig,
        modelCopy,
        RequestContextBytesT{
            copyBytes(historicalData, historicalDataLen),
            copyBytes(marketData, marketDataLen),
            valuationDate}};

    auto hd = std::make_shared<app::HandleData>(
        HandleType::RequestContextData,
        std::move(ctx));

    Handle h = handles->addHandleData(std::move(hd));
    RETURN_SUCCESS(h, h);

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

Handle createRequestContextFromHandles(
    Handle modelConfig,
    Handle historicalData,
    Handle dateSpec,
    Handle modelOptions,
    Handle marketData)
{
    TRY

    CHECK_INIT2;
    CHECK_HANDLE2(NullHandle, modelConfig, HandleType::ModelConfigData);
    CHECK_HANDLE2(NullHandle, historicalData, HandleType::HistoricalData);
    CHECK_HANDLE2(NullHandle, dateSpec, HandleType::DateSpec);
    CHECK_HANDLE2(NullHandle, modelOptions, HandleType::ModelOptions);

    if (marketData != NullHandle) {
        CHECK_HANDLE2(NullHandle, marketData, HandleType::MarketData);
    }

    READ_LOCK_HANDLE(modelConfig);
    auto& modelObj = handleObject<HandleType::ModelConfigData>(modelConfig);
    auto modelCopy = std::make_shared<const ModelConfigDataT>(modelObj);

    RequestContextDataT ctx{
        modelConfig,
        modelCopy,
        RequestContextHandlesT{
            historicalData,
            dateSpec,
            modelOptions,
            marketData}};

    auto hd = std::make_shared<app::HandleData>(
        HandleType::RequestContextData,
        std::move(ctx));

    Handle h = handles->addHandleData(std::move(hd));
    RETURN_SUCCESS(h, h);

    EXCEPTION_ERROR(NullHandle, NullHandle);
}

const char* calcBehavioralSpeedForInstrument(
    Handle requestContext,
    const char* instrumentData,
    int instrumentDataLen,
    const char* ratePaths,
    int ratePathsLen)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized",
            empty_value<std::string>.c_str());
    }
    // Return the empty string (not NULL) so all error paths of the
    // const char* APIs have one convention; NULL becomes a Java null
    // String and NPEs callers that only check isEmpty().
    CHECK_HANDLE2(emptyApiString(), requestContext, HandleType::RequestContextData);

    READ_LOCK_HANDLE(requestContext);
    const auto& ctx = handleObject<HandleType::RequestContextData>(requestContext);

    app::messages::CalcBehavioralSpeedFromPrimaryRateRequest behavReq;
    const auto buildErr = buildBehavioralRequestFromContext(
        ctx,
        instrumentData,
        instrumentDataLen,
        ratePaths,
        ratePathsLen,
        behavReq);
    if (buildErr.isError()) {
        RETURN_ERROR(
            requestContext,
            buildErr.error_,
            buildErr.reason_.c_str(),
            empty_value<std::string>.c_str());
    }

    auto msgReq = msg::makeRequest(std::move(behavReq), "vasara-api");
    msgReq.execControl_ = app::messages::ExecutionControl{};
    msgReq.execControl_->asyncMode_ = app::Async::Off;

    msg::Response response = msg::RequestProcessor::handle(std::move(msgReq));
    const char* out = serializeResponseToApiBuffer(std::move(response));
    RETURN_SUCCESS(requestContext, out);

    EXCEPTION_ERROR(requestContext, empty_value<std::string>.c_str());
}

Handle calcBehavioralSpeedForInstrumentObj(
    Handle requestContext,
    const char* instrumentData,
    int instrumentDataLen,
    const char* ratePaths,
    int ratePathsLen)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", NullHandle);
    }
    CHECK_HANDLE2(NullHandle, requestContext, HandleType::RequestContextData);

    READ_LOCK_HANDLE(requestContext);
    const auto& ctx = handleObject<HandleType::RequestContextData>(requestContext);

    app::messages::CalcBehavioralSpeedFromPrimaryRateRequest behavReq;
    const auto buildErr = buildBehavioralRequestFromContext(
        ctx,
        instrumentData,
        instrumentDataLen,
        ratePaths,
        ratePathsLen,
        behavReq);
    if (buildErr.isError()) {
        RETURN_ERROR(requestContext, buildErr.error_, buildErr.reason_.c_str(), NullHandle);
    }

    auto msgReq = msg::makeRequest(std::move(behavReq), "vasara-api");
    msgReq.execControl_ = app::messages::ExecutionControl{};
    msgReq.execControl_->asyncMode_ = app::Async::Off;

    msg::Response response = msg::RequestProcessor::handle(std::move(msgReq));
    const Handle result = makeResultHandle(std::move(response));
    RETURN_SUCCESS(result, result);

    EXCEPTION_ERROR(requestContext, NullHandle);
}

const char* calcValueForInstrument(
    Handle requestContext,
    const char* instrumentData,
    int instrumentDataLen)
{
    TRY

    if (!handles) {
        logVasaraDebug("calcValueForInstrument rejected: library not initialized");
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized",
            empty_value<std::string>.c_str());
    }
    // Return the empty string (not NULL) so all error paths of the
    // const char* APIs have one convention; NULL becomes a Java null
    // String and NPEs callers that only check isEmpty().
    CHECK_HANDLE2(emptyApiString(), requestContext, HandleType::RequestContextData);

    if (!instrumentData || instrumentDataLen <= 0) {
        logVasaraDebug(std::format(
            "calcValueForInstrument rejected: empty instrument csv len={}",
            instrumentDataLen));
        RETURN_ERROR(
            requestContext,
            ApiErrorNullContent,
            "Instrument portfolio CSV is empty",
            empty_value<std::string>.c_str());
    }

    PricingRequestInputs inputs;
    {
        READ_LOCK_HANDLE(requestContext);
        const auto& ctx = handleObject<HandleType::RequestContextData>(requestContext);

        const auto* ctxHandles = std::get_if<RequestContextHandlesT>(&ctx.payload_);
        if (!ctxHandles) {
            logVasaraDebug("calcValueForInstrument rejected: request context storage is not handles");
            RETURN_ERROR(
                requestContext,
                ApiErrorOperationNotSupported,
                "Request context was not built from handles; use createRequestContextFromHandles",
                empty_value<std::string>.c_str());
        }

        inputs.historicalData = ctxHandles->historicalDataHandle_;
        inputs.dateSpec = ctxHandles->dateSpecHandle_;
        inputs.modelOptions = ctxHandles->modelOptionsHandle_;
        inputs.marketData = ctxHandles->marketDataHandle_;
        inputs.modelConfig = ctx.modelConfig_;
    }

    app::ScopedHandle request;
    const auto buildErr = buildPricingRequestFromContext(
        inputs, instrumentData, instrumentDataLen, request);
    if (buildErr.isError()) {
        RETURN_ERROR(
            requestContext,
            buildErr.error_,
            buildErr.reason_.c_str(),
            empty_value<std::string>.c_str());
    }

    logVasaraDebug("calcValueForInstrument stage: execute");
    if (execute(request) != ApiSuccess) {
        const HandleError err = handleData(request).getError();
        logVasaraDebug(std::format(
            "calcValueForInstrument execute failed: apiError={} reason={}",
            static_cast<int>(err.isError() ? err.error_ : ApiErrorException),
            err.isError() ? err.reason_ : std::string("execute failed")));
        RETURN_ERROR(
            requestContext,
            err.isError() ? err.error_ : ApiErrorException,
            err.isError() ? err.reason_.c_str() : "execute failed",
            empty_value<std::string>.c_str());
    }

    // getResult waits for the async worker and consumes the response into the
    // request's slicer. Do NOT consult hasResultError() first: it reports
    // "response ready", not "response is an error" (see
    // RequestHandleData::hasError), so branching on it races the worker and
    // misclassifies fast successful calcs as errors.
    logVasaraDebug("calcValueForInstrument stage: getResult");
    const char* result = getResult(request);
    if (!result || result[0] == '\0') {
        // No data slices: the response was an error response (or genuinely
        // empty). Drain the error slices for the message.
        const char* resultError = getResultError(request);
        const bool haveDetail = resultError && resultError[0] != '\0';
        logVasaraDebug(std::format(
            "calcValueForInstrument result error payload: {}",
            haveDetail ? resultError : "<empty>"));
        RETURN_ERROR(
            requestContext,
            ApiErrorException,
            haveDetail ? resultError : "Pricing request returned empty response",
            empty_value<std::string>.c_str());
    }

    // `result` already points into the thread-local api_response store -- the
    // documented lifetime of this API's return value. (An earlier revision
    // copied it onto itself via `store<...>() = result;`, a self-aliasing
    // std::string assignment from its own c_str().)
    if (isVasaraDebugEnabled()) {
        const std::string_view resultView(result);
        if (currentPayloadPreviewMode() == PayloadPreviewMode::Truncated) {
            logVasaraDebug(std::format(
                "calcValueForInstrument success: response bytes={} preview={}",
                resultView.size(),
                resultView.substr(0, std::min<size_t>(resultView.size(),
                    kDebugPayloadPreviewMax))));
        } else {
            logVasaraDebug(std::format(
                "calcValueForInstrument success: response bytes={}",
                resultView.size()));
        }
    }
    RETURN_SUCCESS(requestContext, result);

    // Catches are hand-rolled (not EXCEPTION_ERROR) so each arm can log via
    // logVasaraDebug before returning the error.
    } catch (const LibException& ex) {
        logVasaraDebug(std::format("calcValueForInstrument LibException: {}", ex.what()));
        std::string troubleshoot = getGlobalTroubleShootInfo(ex);
        RETURN_ERROR_WITH_TROUBLESHOOT(requestContext, ApiErrorException, ex.what(), std::move(troubleshoot), empty_value<std::string>.c_str());
    } catch (const std::exception& ex) {
        logVasaraDebug(std::format("calcValueForInstrument std::exception: {}", ex.what()));
        RETURN_ERROR(requestContext, ApiErrorException, ex.what(), empty_value<std::string>.c_str());
    }
}

Handle calcValueForInstrumentObj(
    Handle requestContext,
    const char* instrumentData,
    int instrumentDataLen)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", NullHandle);
    }
    CHECK_HANDLE2(NullHandle, requestContext, HandleType::RequestContextData);

    if (!instrumentData || instrumentDataLen <= 0) {
        RETURN_ERROR(requestContext, ApiErrorNullContent, "Instrument portfolio CSV is empty", NullHandle);
    }

    PricingRequestInputs inputs;
    {
        READ_LOCK_HANDLE(requestContext);
        const auto& ctx = handleObject<HandleType::RequestContextData>(requestContext);

        const auto* ctxHandles = std::get_if<RequestContextHandlesT>(&ctx.payload_);
        if (!ctxHandles) {
            RETURN_ERROR(
                requestContext,
                ApiErrorOperationNotSupported,
                "Request context was not built from handles; use createRequestContextFromHandles",
                NullHandle);
        }

        inputs.historicalData = ctxHandles->historicalDataHandle_;
        inputs.dateSpec = ctxHandles->dateSpecHandle_;
        inputs.modelOptions = ctxHandles->modelOptionsHandle_;
        inputs.marketData = ctxHandles->marketDataHandle_;
        inputs.modelConfig = ctx.modelConfig_;
    }

    app::ScopedHandle request;
    const auto buildErr = buildPricingRequestFromContext(
        inputs, instrumentData, instrumentDataLen, request);
    if (buildErr.isError()) {
        RETURN_ERROR(requestContext, buildErr.error_, buildErr.reason_.c_str(), NullHandle);
    }

    if (execute(request) != ApiSuccess) {
        const HandleError err = handleData(request).getError();
        RETURN_ERROR(requestContext, err.isError() ? err.error_ : ApiErrorException, err.isError() ? err.reason_.c_str() : "execute failed", NullHandle);
    }

    // Wait for the async worker, then take the response directly from the
    // future. Do NOT consult hasResultError() first: it reports "response
    // ready", not "response is an error", and its getResultError() branch
    // consumed the future into the slicer, after which response_.get() below
    // threw std::future_error (broken pipeline whenever the worker finished
    // before the check). Error responses are wrapped like successes; callers
    // interrogate them via resultIsError / resultGetError.
    if (waitForResult(request) != ApiSuccess) {
        const HandleError err = handleData(request).getError();
        RETURN_ERROR(requestContext, err.isError() ? err.error_ : ApiErrorException, err.isError() ? err.reason_.c_str() : "waitForResult failed", NullHandle);
    }

    WRITE_LOCK_HANDLE(request)
    auto& reqData = requestHandleData(request);
    if (!reqData.isValid()) {
        RETURN_ERROR(requestContext, ApiErrorException,
            "Pricing response already consumed", NullHandle);
    }
    auto response = reqData.response_.get();
    const Handle result = makeResultHandle(std::move(response));
    RETURN_SUCCESS(result, result);

    EXCEPTION_ERROR(requestContext, NullHandle);
}

Handle genRatePaths(Handle requestContext)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", NullHandle);
    }
    CHECK_HANDLE2(NullHandle, requestContext, HandleType::RequestContextData);

    // Snapshot the rate-gen inputs from the request context under its read lock.
    // buildRatePathsRequestFromContext fills the request struct (which owns its
    // data) so the per-context lock is not held across engine execution below.
    app::messages::GenRatePathsForMortgageValuationRequest req;
    {
        READ_LOCK_HANDLE(requestContext);
        const auto& ctx = handleObject<HandleType::RequestContextData>(requestContext);
        const auto buildErr = buildRatePathsRequestFromContext(ctx, req);
        if (buildErr.isError()) {
            RETURN_ERROR(requestContext, buildErr.error_, buildErr.reason_.c_str(), NullHandle);
        }
    }

    // Object-pattern dispatch: RequestProcessor::handle constructs the
    // GenRatePathsForMortgageValuationRequestHandler and runs it synchronously
    // (no async queue), returning the response in-process.
    auto msgReq = msg::makeRequest(std::move(req), "vasara-api");
    msgReq.execControl_ = app::messages::ExecutionControl{};
    msgReq.execControl_->asyncMode_ = app::Async::Off;

    msg::Response response = msg::RequestProcessor::handle(std::move(msgReq));
    if (response.isErrorResponse()) {
        const auto& errs = response.errorResponse().errors_;
        RETURN_ERROR(
            requestContext,
            ApiErrorException,
            errs.empty() ? "genRatePaths failed" : errs.front().message_.c_str(),
            NullHandle);
    }

    auto& ratePathsResp = response.genRatePathsForMortgageValuationResponse();

    // Return the full base-scenario rate-path set (swap/SOFR, UST, primary,
    // secondary, discount, incremental primary history) as a CalcRatePaths
    // handle. Ownership passes to the caller (the Java side holds the handle).
    const Handle result = handles->addHandleData(
        std::make_shared<app::HandleData>(
            HandleType::CalcRatePaths, std::move(ratePathsResp.ratePaths_)));
    RETURN_SUCCESS(result, result);

    EXCEPTION_ERROR(requestContext, NullHandle);
}

Handle calcValueForMortgageFromRates(
    Handle requestContext,
    Handle ratePaths,
    const char* instrumentData,
    int instrumentDataLen,
    ApiPricingOutputType pricingOutputType,
    bool cleanPrice)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", NullHandle);
    }
    CHECK_HANDLE2(NullHandle, requestContext, HandleType::RequestContextData);
    CHECK_HANDLE2(NullHandle, ratePaths, HandleType::CalcRatePaths);

    if (!instrumentData || instrumentDataLen <= 0) {
        RETURN_ERROR(requestContext, ApiErrorNullContent,
            "Instrument portfolio CSV is empty", NullHandle);
    }

    // Decode the pricing output type. SWIG marshals the enum as a plain int, so a
    // value outside the supported set can still arrive. Switch on the explicitly
    // supported enumerators and map each to its wfmcm counterpart; reject anything
    // else (ApiPricingOutputType_UNKNOWN and any out-of-range value). Switching on
    // the enumerators keeps this correct regardless of the enum's ordering/values.
    wfmcm::PricingOutputType outputType;
    switch (pricingOutputType) {
    case ApiPricingOutputType_Price:
        outputType = wfmcm::PricingOutputType::Price;
        break;
    case ApiPricingOutputType_OAS:
        outputType = wfmcm::PricingOutputType::OAS;
        break;
    case ApiPricingOutputType_ConstantYield:
        outputType = wfmcm::PricingOutputType::ConstantYield;
        break;
    default:
        RETURN_ERROR(requestContext, ApiErrorOperationNotSupported,
            "pricingOutputType must be Price, OAS, or ConstantYield",
            NullHandle);
    }

    // Snapshot the raw pricing inputs + portfolio from the request context under its
    // read lock into the request struct (which owns its data), so no per-context
    // lock is held across engine execution. cleanPrice selects clean vs dirty
    // price handling in the pricing computation. The resulting price is read back
    // via resultGetCleanPrice(), which returns the clean price when set and falls
    // back to dirty otherwise; the full clean/dirty breakdown is available via
    // resultGetPricingResultJson().
    app::messages::CalcValueForMortgageFromRatesRequest req;
    {
        READ_LOCK_HANDLE(requestContext);
        const auto& ctx = handleObject<HandleType::RequestContextData>(requestContext);
        const auto buildErr = buildCalcValueForMortgageFromRatesRequestFromContext(
            ctx, instrumentData, instrumentDataLen, outputType, cleanPrice, req);
        if (buildErr.isError()) {
            RETURN_ERROR(requestContext, buildErr.error_, buildErr.reason_.c_str(), NullHandle);
        }
    }

    // Snapshot the full precomputed rate paths. These are injected directly by the
    // handler (no projection); the handler derives the base swap/SOFR/UST KeyRate
    // seed for the base-context model wiring from this override set internally.
    {
        READ_LOCK_HANDLE(ratePaths);
        const auto& paths = handleObject<HandleType::CalcRatePaths>(ratePaths);
        req.overrideRatePaths_ = paths;
    }

    // Object-pattern dispatch: RequestProcessor::handle constructs the
    // CalcValueForMortgageFromRatesRequestHandler and runs it synchronously (no
    // async queue), returning the response in-process (mirrors genRatePaths).
    auto msgReq = msg::makeRequest(std::move(req), "vasara-api");
    msgReq.execControl_ = app::messages::ExecutionControl{};
    msgReq.execControl_->asyncMode_ = app::Async::Off;

    msg::Response response = msg::RequestProcessor::handle(std::move(msgReq));
    if (response.isErrorResponse()) {
        const auto& errs = response.errorResponse().errors_;
        RETURN_ERROR(
            requestContext,
            ApiErrorException,
            errs.empty() ? "calcValueForMortgageFromRates failed" : errs.front().message_.c_str(),
            NullHandle);
    }

    const Handle result = makeResultHandle(std::move(response));
    RETURN_SUCCESS(result, result);

    EXCEPTION_ERROR(requestContext, NullHandle);
}

int resultIsError(Handle resultData)
{
    TRY

    if (!handles) {
        //Do not use CHECK_INIT here: it returns the ApiErrorLibSetup enum value,
        //which callers of this predicate would misread as "result is an error".
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", 0);
    }
    CHECK_HANDLE2(0, resultData, HandleType::ResultData);
    READ_LOCK_HANDLE(resultData);
    return handleObject<HandleType::ResultData>(resultData).response_.isErrorResponse() ? 1 : 0;

    EXCEPTION_ERROR(resultData, 0);
}

const char* resultGetError(Handle resultData)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized",
            emptyApiString());
    }
    CHECK_HANDLE2(emptyApiString(), resultData, HandleType::ResultData);
    READ_LOCK_HANDLE(resultData);
    const auto& response = handleObject<HandleType::ResultData>(resultData).response_;
    if (!response.isErrorResponse() || response.errorResponse().errors_.empty()) {
        return emptyApiString();
    }
    return storeResultErrorString(response.errorResponse().errors_.front().message_);

    EXCEPTION_ERROR(resultData, emptyApiString());
}

double resultGetCleanPrice(Handle resultData)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", 0.0);
    }
    CHECK_HANDLE2(0.0, resultData, HandleType::ResultData);
    READ_LOCK_HANDLE(resultData);
    const auto& response = handleObject<HandleType::ResultData>(resultData).response_;
    const auto* pricingInput = firstPricingInput(response);
    if (!pricingInput) {
        return 0.0;
    }
    // NOTE: When clean price is not present we fall back to dirty price for
    // backward compatibility with the existing Java/Python callers. Callers
    // that need to distinguish the two should consume the structured payload
    // via resultGetPricingResultJson and inspect isCleanPriceSet.
    return pricingInput->isCleanPriceSet() ? pricingInput->cleanPrice() : pricingInput->dirtyPrice();

    EXCEPTION_ERROR(resultData, 0.0);
}

double resultGetHolding(Handle resultData)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", 0.0);
    }
    CHECK_HANDLE2(0.0, resultData, HandleType::ResultData);
    READ_LOCK_HANDLE(resultData);
    const auto& response = handleObject<HandleType::ResultData>(resultData).response_;
    const auto* pricingInput = firstPricingInput(response);
    return pricingInput ? pricingInput->holding() : 0.0;

    EXCEPTION_ERROR(resultData, 0.0);
}

double resultGetOas(Handle resultData)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", 0.0);
    }
    CHECK_HANDLE2(0.0, resultData, HandleType::ResultData);
    READ_LOCK_HANDLE(resultData);
    const auto& response = handleObject<HandleType::ResultData>(resultData).response_;
    const auto* pricingInput = firstPricingInput(response);
    return pricingInput ? pricingInput->oas() : 0.0;

    EXCEPTION_ERROR(resultData, 0.0);
}

const char* resultGetInstrumentId(Handle resultData)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized",
            emptyApiString());
    }
    CHECK_HANDLE2(emptyApiString(), resultData, HandleType::ResultData);
    READ_LOCK_HANDLE(resultData);
    const auto& response = handleObject<HandleType::ResultData>(resultData).response_;
    const auto* pricingInput = firstPricingInput(response);
    if (!pricingInput) {
        return emptyApiString();
    }
    return storeApiString(pricingInput->id());

    EXCEPTION_ERROR(resultData, emptyApiString());
}

int resultGetSettleDate(Handle resultData)
{
    TRY

    if (!handles) {
        //CHECK_INIT would return ApiErrorLibSetup, which is a plausible YYYYMMDD-domain
        //int for callers expecting "0 when unset".
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", 0);
    }
    CHECK_HANDLE2(0, resultData, HandleType::ResultData);
    READ_LOCK_HANDLE(resultData);
    const auto& response = handleObject<HandleType::ResultData>(resultData).response_;
    const auto* pricingInput = firstPricingInput(response);
    if (!pricingInput || !pricingInput->isSettleDateSet()) {
        return 0;
    }
    return static_cast<int>(QuantLib::toYYYYMMDD(pricingInput->settleDate()));

    EXCEPTION_ERROR(resultData, 0);
}

const char* resultGetPricingResultJson(Handle resultData)
{
    TRY

    if (!handles) {
        RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", emptyApiString());
    }
    CHECK_HANDLE2(emptyApiString(), resultData, HandleType::ResultData);
    READ_LOCK_HANDLE(resultData);
    const auto& response = handleObject<HandleType::ResultData>(resultData).response_;
    const auto* pricingInput = firstPricingInput(response);
    if (!pricingInput) {
        return emptyApiString();
    }

    wfmcm::json_options options;
    options.json_allocator_buffer_size(kPricingResultJsonBufferBytes);
    wfmcm::json_parser parser(options);
    polyvar p;
    p << *pricingInput;
    return storeApiString(parser.serialize(p));

    EXCEPTION_ERROR(resultData, emptyApiString());
}

const char* getSwapRates(Handle ratePaths)
{
    return getRatePathComponentJson(ratePaths, [](const auto& paths) {
        return paths.swapRates_ ? &*paths.swapRates_ : nullptr;
    });
}

const char* getPrimaryRates(Handle ratePaths)
{
    return getRatePathComponentJson(ratePaths, [](const auto& paths) {
        return paths.primaryRates_ ? &*paths.primaryRates_ : nullptr;
    });
}

const char* getSecondaryRates(Handle ratePaths)
{
    return getRatePathComponentJson(ratePaths, [](const auto& paths) {
        return paths.secondaryRates_ ? &*paths.secondaryRates_ : nullptr;
    });
}

const char* getDiscountRates(Handle ratePaths)
{
    return getRatePathComponentJson(ratePaths, [](const auto& paths) {
        return paths.discountRates_ ? &*paths.discountRates_ : nullptr;
    });
}

const char* getIncrementalPrimaryRateHistory(Handle ratePaths)
{
    return getRatePathComponentJson(ratePaths, [](const auto& paths) {
        return paths.incrementalPrimaryRateHistory_
            ? &*paths.incrementalPrimaryRateHistory_
            : nullptr;
    });
}