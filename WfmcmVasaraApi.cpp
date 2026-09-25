#include <src/app-common/api/WfmcmVasaraApi.h>
#include <src/app-common/api/WfmcmApiInternal.h>
#include <src/app-common/messages/RequestProcessor.h>
#include <src/io/TimeSeriesReader.h>
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

// Returns a payload preview suitable for a debug log line.
// IMPORTANT: instrument CSVs carry PII / loan-level data. The preview is
// truncated AND length-only by default. The full snippet is included only
// when PANDO_VASARA_DEBUG=full is set; callers must never enable that
// in production.
enum class PayloadPreviewMode { LengthOnly, Truncated };

std::string previewPayload(
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


// NOTE: an earlier revision kept a file-local modelConfig -> requestContext
// registry here. It was write-only (registered on create, never consulted,
// never unregistered -- generic deleteHandle cannot see file-local state), so
// it grew without bound and accumulated stale Handle keys. It has been
// removed. If session->context dependency tracking is ever needed (e.g. to
// reject destroying a session with live contexts), it must be wired into an
// explicit destroy path, not bolted onto this translation unit.

// Tri-state debug verbosity controlled by env var PANDO_VASARA_DEBUG:
//   unset / 0        -> off
//   any non-empty    -> on, payload previews are length-only (production-safe)
//   "full"           -> on, payload previews include truncated content
//                      (requires PANDO_VASARA_DEBUG_ALLOW_PII=1)
// SECURITY: "full" mode must NEVER be enabled in production environments
// because it writes instrument CSV bytes (loan-level data) to the process log.
enum class VasaraDebugLevel { Off, Safe, Full };

VasaraDebugLevel vasaraDebugLevel()
{
    static const VasaraDebugLevel level = [] {
        const char* raw = std::getenv("PANDO_VASARA_DEBUG");
        if (!raw || raw[0] == '\0' || raw[0] == '0') {
            return VasaraDebugLevel::Off;
        }
        std::string v(raw);
        std::transform(v.begin(), v.end(), v.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (v == "full") {
            const char* allowPii = std::getenv("PANDO_VASARA_DEBUG_ALLOW_PII");
            if (allowPii && allowPii[0] == '1' && allowPii[1] == '\0') {
                return VasaraDebugLevel::Full;
            }
            return VasaraDebugLevel::Safe;
        }
        return VasaraDebugLevel::Safe;
    }();
    return level;
}

bool isVasaraDebugEnabled()
{
    return vasaraDebugLevel() != VasaraDebugLevel::Off;
}

PayloadPreviewMode currentPayloadPreviewMode()
{
    return vasaraDebugLevel() == VasaraDebugLevel::Full
        ? PayloadPreviewMode::Truncated
        : PayloadPreviewMode::LengthOnly;
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
 * Shared RAII owner for the internal behavioral-model-map spec handle.
 *
 * The spec handle is created natively inside createModelConfig and is never
 * returned to the Java side, so it is NOT in Java's handle-teardown stack and
 * nothing external will ever delete it. Ownership therefore lives with the
 * ModelConfigData (and every request-context copy of it) via a shared_ptr
 * whose deleter removes the handle when the last owner goes away -- i.e. when
 * the session handle is deleted after all dependent contexts (Java's LIFO
 * teardown guarantees contexts go first). The deleter tolerates teardown
 * ordering: if the HandleContainer is already gone, the spec's HandleData was
 * destroyed with it and there is nothing left to delete.
 *
 * @param h The spec handle to own. May be NullHandle for a session with no
 *          behavioral map.
 */
std::shared_ptr<const Handle> makeOwnedSpecHandle(Handle h)
{
    if (h == NullHandle) {
        return {};
    }
    return std::shared_ptr<const Handle>(
        new Handle(h),
        [](const Handle* p) {
            if (p) {
                if (isValidHandle(*p)) {
                    deleteHandle(*p);
                }
                delete p;
            }
        });
}

/**
 *  Builds the behavioral model map spec from CSV bytes.
 * - Empty CSV is a legitimate "no behavioral map" session: returns NullHandle
 *   with *err left as success.
 * - Any FAILURE (spec creation or parameter set) is reported through *err so
 *   createModelConfig can fail loudly. Silently proceeding without the map
 *   would skip mapBehavioralModel in the calc paths and produce wrong
 *   valuations with no error indication -- the worst failure mode available.
 *  @param csv The CSV bytes to parse.
 *  @param err Out param for any failure. Cleared on success.
 */
Handle createBehavioralModelMapSpecFromCsv(const std::vector<char>& csv, HandleError* err)
{
    *err = HandleError{};
    if (csv.empty()) {
        return NullHandle;
    }
    // Managed manually rather than via ScopedHandle: ScopedHandle has no
    // release() -- its move-assignment deletes the held handle -- so it cannot
    // transfer ownership out on the success path. setModelParameter has its
    // own TRY/catch and does not throw, so the manual cleanup below is safe.
    Handle spec = createModelSpec(ApiModel_BehavioralModelMap);
    if (spec == NullHandle) {
        *err = HandleError{ApiErrorException,
            "createModelSpec(ApiModel_BehavioralModelMap) failed"};
        return NullHandle;
    }
    const int rc = setModelParameter(
        spec,
        ApiModelParameter_ModelParams,
        csv.data(),
        static_cast<int>(csv.size()));
    if (rc != ApiSuccess) {
        *err = handleData(spec).getError();
        if (!err->isError()) {
            *err = HandleError{static_cast<ApiError>(rc),
                "setModelParameter(ModelParams) failed for behavioral model map"};
        }
        deleteHandle(spec);
        return NullHandle;
    }
    return spec;
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
    {
        const int fillRc = handleData(ctxHandles->modelOptionsHandle_).fill();
        if (fillRc != ApiSuccess) {
            HandleError fillErr = handleData(ctxHandles->modelOptionsHandle_).getError();
            if (!fillErr.isError()) {
                fillErr = HandleError{static_cast<ApiError>(fillRc),
                    fmt::format("modelOptions fill failed (rc={})", fillRc)};
            }
            return fillErr;
        }
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

    if (ctx.modelConfig_ && ctx.modelConfig_->modelMapSpecHandle() != NullHandle) {
        const QuantLib::Date qlFactor = dateSpec.getMarketCurveDate();
        const Date factorDate{
            static_cast<unsigned int>(QuantLib::toYYYYMMDD(qlFactor))};
        const int mapRc = mapBehavioralModel(
            portfolio,
            ctx.modelConfig_->modelMapSpecHandle(),
            factorDate);
        if (mapRc != ApiSuccess) {
            // mapBehavioralModel sets its error on either the portfolio or the
            // modelSpec handle depending on which check failed. If the
            // portfolio carries no error, fall back to a synthesized one --
            // returning a success-state HandleError here would make the caller
            // proceed as if mapping succeeded (and previously it then used the
            // already-deleted portfolio handle: use-after-free).
            HandleError mapErr = handleData(portfolio).getError();
            if (!mapErr.isError()) {
                mapErr = HandleError{static_cast<ApiError>(mapRc),
                    "mapBehavioralModel failed"};
            }
            return mapErr;
        }
    }

    {
        READ_LOCK_HANDLE(portfolio);
        const auto& srcPortfolio = handleObject<HandleType::Portfolio>(portfolio);

        // Do not copy-assign pmr containers directly from a temporary handle.
        // We need stable ownership after the handle is deleted below.
        out.portfolio_.isMsr = srcPortfolio.isMsr;
        out.portfolio_.msrData.assign(srcPortfolio.msrData.begin(), srcPortfolio.msrData.end());
        out.portfolio_.pools.assign(srcPortfolio.pools.begin(), srcPortfolio.pools.end());
        out.portfolio_.pricingInput.assign(
            srcPortfolio.pricingInput.begin(), srcPortfolio.pricingInput.end());
        out.portfolio_.profitabilityInput.assign(
            srcPortfolio.profitabilityInput.begin(), srcPortfolio.profitabilityInput.end());
    } // portfolio handle is released by ScopedHandle at scope exit

    (void)ctxHandles->marketDataHandle_;
    return {};
}

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
            previewPayload(instrumentData, instrumentDataLen, currentPayloadPreviewMode())));
        logVasaraDebug("calcValueForInstrument check: validating historicalData");
    }
    if (!isValidHandle(in.historicalData)
        || !isHandleTypeAnyOf(in.historicalData, {HandleType::HistoricalData})) {
        if (debug) {
            logVasaraDebug(std::format(
                "calcValueForInstrument rejected: historicalData handle invalid h={} isValid={} hasType={}",
                in.historicalData.internal_,
                isValidHandle(in.historicalData),
                isHandleTypeAnyOf(in.historicalData, {HandleType::HistoricalData})));
        }
        return {ApiErrorWrongHandleType, "historicalData handle is invalid"};
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument check: validating dateSpec");
    }
    if (!isValidHandle(in.dateSpec)
        || !isHandleTypeAnyOf(in.dateSpec, {HandleType::DateSpec})) {
        if (debug) {
            logVasaraDebug(std::format(
                "calcValueForInstrument rejected: dateSpec handle invalid h={} isValid={} hasType={}",
                in.dateSpec.internal_,
                isValidHandle(in.dateSpec),
                isHandleTypeAnyOf(in.dateSpec, {HandleType::DateSpec})));
        }
        return {ApiErrorWrongHandleType, "dateSpec handle is invalid"};
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument check: validating modelOptions");
    }
    if (!isValidHandle(in.modelOptions)
        || !isHandleTypeAnyOf(in.modelOptions, {HandleType::ModelOptions})) {
        if (debug) {
            logVasaraDebug(std::format(
                "calcValueForInstrument rejected: modelOptions handle invalid h={} isValid={} hasType={}",
                in.modelOptions.internal_,
                isValidHandle(in.modelOptions),
                isHandleTypeAnyOf(in.modelOptions, {HandleType::ModelOptions})));
        }
        return {ApiErrorWrongHandleType, "modelOptions handle is invalid"};
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument check: validating marketData");
    }
    if (!isValidHandle(in.marketData)
        || !isHandleTypeAnyOf(in.marketData, {HandleType::MarketData})) {
        if (debug) {
            logVasaraDebug(std::format(
                "calcValueForInstrument rejected: marketData handle invalid h={} isValid={} hasType={}",
                in.marketData.internal_,
                isValidHandle(in.marketData),
                isHandleTypeAnyOf(in.marketData, {HandleType::MarketData})));
        }
        return {ApiErrorWrongHandleType, "marketData handle is invalid"};
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument check: all handles valid");
    }

    app::ScopedHandle request(createRequest(ApiRequest_CalcValueForMortgage));
    if (!request) {
        if (debug) {
            logVasaraDebug(
                "calcValueForInstrument failed: createRequest(ApiRequest_CalcValueForMortgage) returned null");
        }
        return {ApiErrorOperationNotSupported, "Failed to create CalcValueForMortgage request"};
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: request created");
        logVasaraDebug("calcValueForInstrument stage: createPricingSpec(ApiPricingOutputType_Price)");
    }

    app::ScopedHandle pricingSpec(createPricingSpec(ApiPricingOutputType_Price));
    if (!pricingSpec) {
        if (debug) {
            logVasaraDebug(
                "calcValueForInstrument failed: createPricingSpec(ApiPricingOutputType_Price) returned null");
        }
        return {ApiErrorOperationNotSupported, "Failed to create pricing spec"};
    }

    auto handleErrFromHandle = [](Handle h, const char* fallback) -> HandleError {
        HandleError err = handleData(h).getError();
        if (err.isError()) {
            return err;
        }
        return {ApiErrorException, fallback};
    };

    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: setBaseOASFlag(false) on pricingSpec");
    }
    if (setBaseOASFlag(pricingSpec, false) != ApiSuccess) {
        return handleErrFromHandle(pricingSpec, "setBaseOASFlag(pricingSpec) failed");
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: setCleanPriceFlag(true) on pricingSpec");
    }
    if (setCleanPriceFlag(pricingSpec, true) != ApiSuccess) {
        return handleErrFromHandle(pricingSpec, "setCleanPriceFlag(pricingSpec) failed");
    }

    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: bindPricingSpec");
    }
    if (bindPricingSpec(request, pricingSpec) != ApiSuccess) {
        return handleErrFromHandle(request, "bindPricingSpec failed");
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: bindHistoricalData");
    }
    if (bindHistoricalData(request, in.historicalData) != ApiSuccess) {
        return handleErrFromHandle(request, "bindHistoricalData failed");
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: bindDateSpec");
    }
    if (bindDateSpec(request, in.dateSpec) != ApiSuccess) {
        return handleErrFromHandle(request, "bindDateSpec failed");
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: bindModelOptions");
    }
    if (bindModelOptions(request, in.modelOptions) != ApiSuccess) {
        return handleErrFromHandle(request, "bindModelOptions failed");
    }
    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: bindMarketData");
    }
    if (bindMarketData(request, in.marketData) != ApiSuccess) {
        return handleErrFromHandle(request, "bindMarketData failed");
    }

    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: createInstrumentPortfolio(ApiPortfolioType_Mbs)");
    }
    app::ScopedHandle portfolio(createInstrumentPortfolio(
        ApiPortfolioType_Mbs,
        instrumentData,
        instrumentDataLen));
    if (!portfolio) {
        if (debug) {
            logVasaraDebug("calcValueForInstrument failed: createInstrumentPortfolio returned null");
        }
        return {ApiErrorOperationNotSupported, "Failed to create instrument portfolio"};
    }

    if (in.modelConfig && in.modelConfig->modelMapSpecHandle() != NullHandle) {
        if (debug) {
            logVasaraDebug(std::format(
                "calcValueForInstrument applying behavioral model map handle={}",
                in.modelConfig->modelMapSpecHandle().internal_));
        }
        const Handle dateSpecH = in.dateSpec;
        READ_LOCK_HANDLE(dateSpecH);
        const auto& dateSpec = handleObject<HandleType::DateSpec>(dateSpecH);
        const QuantLib::Date qlFactor = dateSpec.getMarketCurveDate();
        const Date factorDate{ static_cast<unsigned int>(QuantLib::toYYYYMMDD(qlFactor)) };
        if (mapBehavioralModel(portfolio, in.modelConfig->modelMapSpecHandle(), factorDate) != ApiSuccess) {
            return handleErrFromHandle(portfolio, "mapBehavioralModel failed");
        }
    }

    if (debug) {
        logVasaraDebug("calcValueForInstrument stage: bindInstrumentPortfolio");
    }
    if (bindInstrumentPortfolio(request, portfolio) != ApiSuccess) {
        return handleErrFromHandle(request, "bindInstrumentPortfolio failed");
    }

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

    HandleError specErr;
    const Handle spec =
        createBehavioralModelMapSpecFromCsv(config.modelMapCsv_, &specErr);
    if (specErr.isError()) {
        RETURN_ERROR(NullHandle, specErr.error_, specErr.reason_, NullHandle);
    }
    // From here the shared owner guarantees the spec handle is released even
    // if make_shared / addHandleData throws below.
    config.modelMapSpec_ = makeOwnedSpecHandle(spec);

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
    // logVasaraDebug before the macro's WRITE_LOCK/return sequence.
    } catch (const LibException& ex) {
        logVasaraDebug(std::format("calcValueForInstrument LibException: {}", ex.what()));
        WRITE_LOCK_HANDLE(requestContext)
        std::string troubleshoot = getGlobalTroubleShootInfo(ex);
        RETURN_ERROR_WITH_TROUBLESHOOT(requestContext, ApiErrorException, ex.what(), std::move(troubleshoot), empty_value<std::string>.c_str());
    } catch (const std::exception& ex) {
        logVasaraDebug(std::format("calcValueForInstrument std::exception: {}", ex.what()));
        WRITE_LOCK_HANDLE(requestContext)
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
    return storeApiString(response.errorResponse().errors_.front().message_);

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