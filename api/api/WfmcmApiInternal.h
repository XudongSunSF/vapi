#ifndef WFMCM_API_INTERNAL_H
#define WFMCM_API_INTERNAL_H

#include <src/app-common/api/WfmcmApi.h>
#include <src/app-common/api/WfmcmApiEnums.h>
#include <src/app-common/api/ResultData.h>
#include <src/app-common/messages/requests/Request.h>
#include <src/app-common/messages/responses/Response.h>
#include <src/app-common/messages/slicers/ResponseSlicer.h>
#include <src/app-common/enums.h>
#include <src/app-common/run-config/RunConfigResolver.h>
#include <src/app-common/api/ModelConfigData.h>
#include <src/app-common/api/RequestContextData.h>
#include <wfmcm/enums.h>
#include <mortgage/utility/memory/thread_local.h>
#include <mortgage/utility/types/polyvar/polyvar_macros.h>
#include <mortgage/utility/concurrency/scoped_unlock.h>
#include <wfmcm/classIds.h>

#include <ql/time/date.hpp>
#include <boost/container_hash/hash.hpp>

#include <initializer_list>
#include <mutex>
#include <memory>
#include <shared_mutex>
#include <string>
#include <variant>
#include <tuple>
#include <any>
#include <unordered_map>
#include <format>
#include <optional>
#include <map>
#include <future>
#include <cstring>

using namespace app;

constexpr const Handle NullHandle{ 0 };
constexpr const Date NullDate{ 0 };
constexpr const char* MinDateStr = "1900-01-01";
constexpr const Date MinDateInt = { 19000101 };

namespace std
{
	template<> struct hash<Handle>
	{
		size_t operator()(const Handle& h) const {
			return hash<unsigned long long>()(h.internal_);
		};
	};
}

bool operator==(const Handle& lhs, const Handle& rhs);
bool operator<(const Handle& lhs, const Handle& rhs);
bool operator==(const Date& lhs, const Date& rhs);
bool operator<(const Date& lhs, const Date& rhs);

//-------------------------------------------------------------------------------------------
//                                   MACROS
//-------------------------------------------------------------------------------------------
#define READ_LOCK_HANDLE(h) auto rlock##h = handleReadLock(h);
#define READ_UNLOCK_HANDLE(h) auto rulock##h = handleReadUnlock(h);
#define WRITE_LOCK_HANDLE(h) auto wlock##h = handleWriteLock(h);
#define WRITE_UNLOCK_HANDLE(h) auto wulock##h = handleWriteUnlock(h);

//Preconditions: handle read lock is taken.
//Postconditions: handle read lock is taken.
//The sequence of locking events is as follows:
//	1. unlock read mutex
//  2. lock write mutex
//  3. unlock write mutex (scope exit)
//  4. re-lock read mutex (scope exit)
#define UPGRADE_TO_WRITE_LOCK_HANDLE(h) \
	READ_UNLOCK_HANDLE(h) \
	WRITE_LOCK_HANDLE(h)

/**
 * Return success, clearing errors on a handle, and returning the given status.
 *
 * @note Usually you want to use `RETURN_API_SUCCESS`.
 *
 * @param handle `Handle` to clear errors on
 * @param ret Value to return, e.g. a `Handle` or `ApiError`
 */
#define RETURN_SUCCESS(handle, ret) \
	do { \
		clearHandleError(handle); \
		return ret; \
	} \
	while (false)

/**
 * Return success, clearing errors on a handle.
 *
 * This returns `ApiSuccess` to the caller.
 *
 * @param handle `Handle` to clear errors on
 */
#define RETURN_API_SUCCESS(handle) RETURN_SUCCESS(handle, ApiSuccess)

/**
 * Return an error status, setting an error on a handle with a message.
 *
 * @note Usually you want to use `RETURN_API_ERROR`.
 *
 * @param handle `Handle` to set error on
 * @param err `ApiError` error status to set
 * @param text `std::string` or string literal error text to use
 * @param ret Value to return, e.g. a `Handle` or `ApiError`
 */
#define RETURN_ERROR(handle, err, text, ret) \
	do { \
		setHandleError(handle, {err, text}); \
		return ret; \
	} \
	while (false)

/**
 * Return an error status, setting an error on a handle with a message.
 *
 * @param handle `Handle` to set error on
 * @param err `ApiError` error status to set and return
 * @param text `std::string` or string literal error text to use
 */
#define RETURN_API_ERROR(handle, err, text) RETURN_ERROR(handle, err, text, err)

#define RETURN_ERROR_WITH_TROUBLESHOOT(handle, err, text, ctx, ret) \
	do { \
		setHandleError(handle, {err, text, ctx}); \
		return ret; \
	} \
	while (false)
#define RETURN_API_ERROR_WITH_TROUBLESHOOT(handle, err, text, ctx) RETURN_ERROR_WITH_TROUBLESHOOT(handle, err, text, ctx, err);

#define TRY try {

/**
 * Macro to handle exceptions and return an error status.
 *
 * This macro is used to wrap a block of code in a catch exceptions
 * that may occur during execution. It handles two types of exceptions:
 * - `LibException`: Captures library-specific exceptions, retrieves troubleshooting
 *   information, and returns an error with detailed context.
 * - `std::exception`: Captures standard exceptions and returns an error with the
 *   exception message.
 *
 * The error is written through `setHandleError`, which is self-synchronising
 * (`HandleData::setError` takes the handle's `m_` mutex), so no payload lock
 * (`objectMutex_`) is taken here.
 *
 * @param handle The `Handle` to set the error on.
 * @param ret The value to return in case of an exception, e.g., a `Handle` or `ApiError`.
 */
#define EXCEPTION_ERROR(handle, ret) } \
catch (const LibException& ex) { \
	std::string troubleshoot = getGlobalTroubleShootInfo(ex); \
	RETURN_ERROR_WITH_TROUBLESHOOT(handle, ApiErrorException, ex.what(), std::move(troubleshoot), ret); \
} \
catch (const std::exception& ex) { \
	RETURN_ERROR(handle, ApiErrorException, ex.what(), ret); \
}

#define EXCEPTION_API_ERROR(handle) EXCEPTION_ERROR(handle, ApiErrorException)

// {FIXME} remove semicolon to force users to use semicolon
#define CHECK_VALID_HANDLE(h, ...) \
if (!isValidHandle(h)) \
	RETURN_API_ERROR(h, ApiErrorInvalidHandle, "Invalid handle");

// {FIXME} remove semicolon to force users to use semicolon
#define CHECK_VALID_HANDLE2(ret, h, ...) \
if (!isValidHandle(h)) \
	RETURN_ERROR(h, ApiErrorInvalidHandle, "Invalid handle", ret);

// {FIXME} encase in do { ... } while (false)
#define CHECK_HANDLE_TYPE(h, ...) \
if (!isHandleTypeAnyOf(h, {__VA_ARGS__})) { \
	auto handlesStr = to_string({__VA_ARGS__}); \
	RETURN_API_ERROR(h, ApiErrorWrongHandleType, \
		fmt::format("[{}] Invalid handle type. Expected: [{}] Received: {}.", \
		#h, fmt::join(handlesStr, ", "), to_string(handleData(h).getType()))); \
}

// {FIXME} encase in do { ... } while (false)
#define CHECK_HANDLE_TYPE2(ret, h, ...) \
if (!isHandleTypeAnyOf(h, {__VA_ARGS__})) { \
	auto handlesStr = to_string({__VA_ARGS__}); \
	RETURN_ERROR(h, ApiErrorWrongHandleType, \
		fmt::format("[{}] Invalid handle type. Expected: [{}] Received: {}.", \
		#h, fmt::join(handlesStr, ", "), to_string(handleData(h).getType())), \
		ret); \
}

// {TODO} consider if we need do { ... } while (false) for CHECK_HANDLE[2]

//Returns an ApiError
#define CHECK_HANDLE(h, ...) \
CHECK_VALID_HANDLE(h, __VA_ARGS__) \
CHECK_HANDLE_TYPE(h, __VA_ARGS__)

//Returns an specified return type
#define CHECK_HANDLE2(ret, h, ...) \
CHECK_VALID_HANDLE2(ret, h, __VA_ARGS__) \
CHECK_HANDLE_TYPE2(ret, h, __VA_ARGS__)

// {FIXME} encase in do { ... } while (false)
//Returns an ApiError
#define CHECK_CONTENT(h, content, len) \
if ((content == nullptr) || (*content == 0)) { \
	RETURN_API_ERROR(h, ApiErrorNullContent, "Content is null"); \
} \
if ((len != -1) && (len <= 0)) { \
	RETURN_API_ERROR(h, ApiErrorInvalidLength, "Content length must be > 0 or -1"); \
} \
else if (len == -1) { \
	len = (int)std::strlen(content); \
}

// {FIXME} encase in do { ... } while (false)
//Returns a null handle
#define CHECK_CONTENT2(h, content, len) \
if ((content == nullptr) || (*content == 0)) { \
	RETURN_ERROR(h, ApiErrorNullContent, "Content is null", NullHandle); \
} \
if ((len != -1) && (len <= 0)) { \
	RETURN_ERROR(h, ApiErrorInvalidLength, "Content length must be > 0 or -1", NullHandle); \
} \
else if (len == -1) { \
	len = (int)std::strlen(content); \
}

// {FIXME} encase in do { ... } while (false)
//Returns an ApiError
#define CHECK_ARRAY(h, arr, len) \
if (arr == nullptr) { \
	RETURN_API_ERROR(h, ApiErrorNullArgument, fmt::format("Null input [{}]",#arr)); \
} \
if (len <= 0) { \
	RETURN_API_ERROR(h, ApiErrorInvalidLength, fmt::format("Size must be > 0 [{}]", #arr)); \
}

// {FIXME} encase in do { ... } while (false)
//Returns a null handle
#define CHECK_ARRAY2(h, arr, len) \
if (arr == nullptr) { \
	RETURN_ERROR(h, ApiErrorNullArgument, fmt::format("Null input [{}]",#arr), NullHandle); \
} \
if (len <= 0) { \
	RETURN_ERROR(h, ApiErrorInvalidLength, fmt::format("Size must be > 0 [{}]", #arr), NullHandle); \
}

// {FIXME} remove semicolon to force users to use semicolon
//Returns an ApiError
#define CHECK_SIZE(h, len) \
if (len <= 0) \
	RETURN_API_ERROR(h, ApiErrorInvalidLength, fmt::format("Size must be > 0 [{}]", #len));

// {FIXME} remove semicolon to force users to use semicolon
//Returns a null handle
#define CHECK_SIZE2(h, len) \
if (len <= 0) \
	RETURN_ERROR(h, ApiErrorInvalidLength, fmt::format("Size must be > 0 [{}]", #len), NullHandle);

// {FIXME} remove semicolon to force users to use semicolon
//Returns an ApiError
#define CHECK_RANGE(h, len, min, max) \
if ((len < min) || (len > max)) \
	RETURN_API_ERROR(h, ApiErrorOutOfRange, fmt::format("Value of '{}' must be in the range [{},{}]", #len, min, max));

// {FIXME} subsequent CHECK_* macros should either use do-while or no semicolon

//Returns a null handle
#define CHECK_RANGE2(h, len, min, max) \
if ((len < min) || (len > max)) { \
	RETURN_ERROR(h, ApiErrorOutOfRange, fmt::format("Value of '{}' must be in the range [{},{}]", #len, min, max), NullHandle); \
}

#define CHECK_DATE(h, date, name) \
if (!isValidDate(date)) { \
	RETURN_API_ERROR(h, ApiErrorInvalidDate, fmt::format("{} date must start from {}", #name, MinDateStr)); \
}

#define CHECK_DATE2(h, date, name) \
if (!isValidDate(date)) { \
	RETURN_ERROR(h, ApiErrorInvalidDate, fmt::format("{} date must start from {}", #name, MinDateStr), NullHandle); \
}

/**
 * Check that the library is initialized and return error if not.
 *
 * This is used with API functions that return an integral status.
 */
#define CHECK_INIT \
	if (!handles) \
		RETURN_API_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized")

/**
 * Check that the library is initialized and return the null handle if not.
 *
 * This is used with API functions that return a `Handle`.
 */
#define CHECK_INIT2 \
	if (!handles) \
		RETURN_ERROR(NullHandle, ApiErrorLibSetup, "Library not initialized", NullHandle)

#define BIND_HANDLES(to, from, id, func) handleData(to).bindHandle(from, static_cast<int>(id), func, true);

#define BIND_HANDLES2(to, from, id, func, doFill) handleData(to).bindHandle(from, static_cast<int>(id), func, doFill);

#define REQUEST_HANDLES \
	HandleType::AttribValueChangeByWaterfallRequest, \
	HandleType::GenRatePathsForMortgageValuationRequest, \
	HandleType::GenPrimaryMortgageRatePathsRequest, \
	HandleType::GenSecondaryMortgageRatePathsRequest, \
	HandleType::CalcValueForMortgageRequest, \
	HandleType::CalcValueForMortgageWithSofrRatesRequest, \
	HandleType::CalcGreeksFromPricesRequest, \
    HandleType::CalcValueForMonthEndRollRequest, \
	HandleType::GeneratePathForWaterfallAttributionRequest, \
	HandleType::CalcBehavioralSpeedFromPrimaryRateRequest, \
	HandleType::CalcValueForMortgageFromRatesRequest, \
	HandleType::CalcProfitabilityFromBehavioralSpeedsRequest, \
	HandleType::CancelRequest

//-------------------------------------------------------------------------------------------
//                                    INTERNAL API
//-------------------------------------------------------------------------------------------
namespace app {

//TODO: convert this to std::error_code
struct HandleError
{
	HandleError() = default;
	explicit HandleError(ApiError e)
		: error_(e)
		, reason_(app::to_string(e))
	{}
	HandleError(ApiError e, std::string s, std::string tt="")
		: error_(e)
		, reason_(std::move(s))
		, troubleshoot_(std::move(tt))
	{}
	bool isError() const { return error_ != ApiSuccess; }
	ApiError error_{ ApiSuccess };
	std::string reason_;
	std::string troubleshoot_;
};

CLASS_OPERATOR_DECLARE(HandleError)

//-------------------------------------------------------------------------------------------
//                                    HANDLE DATA
//-------------------------------------------------------------------------------------------
//NOTE: Access to this object must be synchronized
struct HandleData : public std::enable_shared_from_this<HandleData>
{
	//Set data from->to
	using SetterFunc = std::function<HandleError(Handle to, Handle from, int setterId)>;

	template <typename T>
	HandleData(HandleType type, T&& t)
		: type_(type)
		, object_(std::move(t))
	{}

	HandleData(HandleData&& other);
	HandleData(const HandleData& other);
	HandleData& operator=(HandleData&& other);
	HandleData& operator=(const HandleData& other);
	virtual ~HandleData() {}

	std::shared_mutex& getLock() const;

	HandleType getType() const;

	//use under read/write lock `getLock()`
	template <typename T>
	T& getObject() {
		return std::any_cast<T&>(object_);
	}

	//use under read/write lock `getLock()`
	template <typename T>
	const T& getObject() const {
		return std::any_cast<const T&>(object_);
	}

	void setError(HandleError e);

	HandleError getError();

	void bindHandle(Handle h, int setterId, SetterFunc&& func, bool doFill = true);

	Handle getBoundHandle(int setterId);

	//Update held object with the latest bindings data.
	int fill();

protected:
	virtual HandleError preFill() { return {}; }
	virtual HandleError postFill() { return {}; }

	struct Binding
	{
		std::shared_ptr<HandleData> handleDataPtr_;
		SetterFunc setter_;
		bool doFill_{ true }; //calls fill() recursively for each binding
	};

	//Members
	HandleType type_{ HandleType::Unknown };
	mutable std::recursive_mutex m_; //for error and bindings
	HandleError error_;
	std::map<int, Binding> bindings_;
	mutable std::shared_mutex objectMutex_; //for held object
	std::any object_;
};

struct RequestHandleData : public HandleData
{
	using HandleData::HandleData;
	HandleError postFill() override;

	bool isValid() const
	{
		return response_.valid();
	}

	bool isResponseReady() const
	{
		return isValid() &&
			(response_.wait_for(std::chrono::seconds::zero()) == std::future_status::ready);
	}

	bool isResponsePending() const
	{
		return isValid() &&
			(response_.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout);
	}

	bool hasData() const
	{
		return isResponseReady() || slicer_->hasSlices();
	}

	bool hasError() const
	{
		return isResponseReady() || slicer_->hasErrorSlices();
	}

	//members
	app::messages::Request::Identifier id_;
	app::messages::Request request_;
	std::future<app::messages::Response> response_;
	std::unique_ptr<app::messages::ResponseSlicer> slicer_;
};

struct HandleContainer
{
	Handle addHandleData(std::shared_ptr<HandleData> data);
	size_t deleteHandleData(Handle h);
	Handle cloneHandleData(Handle h);
	bool handleExists(Handle h);
private:
	std::shared_mutex m_;
	std::unordered_map<Handle, std::shared_ptr<HandleData>> handles_;
};

extern std::unique_ptr<app::HandleContainer> handles;
extern std::unique_ptr<app::RunConfigResolver> resolver;

struct ScopedHandle
{
	ScopedHandle() : h_(NullHandle) {}
	ScopedHandle(Handle h) : h_(h) {}
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle(ScopedHandle&& other) noexcept
		: h_(other.h_)
	{
		other.h_ = NullHandle;
	}
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	ScopedHandle& operator=(ScopedHandle&& other) noexcept
	{
		if (isValidHandle(h_)) {
			deleteHandle(h_);
		}
		h_ = other.h_;
		other.h_ = NullHandle;
		return *this;
	}
	ScopedHandle& operator=(Handle h)
	{
		if (isValidHandle(h_)) {
			deleteHandle(h_);
		}
		h_ = h;
		return *this;
	}
	~ScopedHandle() noexcept
	{
		if (isValidHandle(h_)) {
			deleteHandle(h_);
		}
	}
	explicit operator bool() const
	{
		return isValidHandle(h_);
	}
	operator Handle() const { return h_; }
private:
	Handle h_;
};

namespace staging
{
	struct HistoricalData
	{
		std::optional<StateTsLookup> hpi_;
		std::optional<StateTsLookup> unemployment_;
		std::optional<HistRateTsMap<std::string>> economicScenario_;
		std::optional<HistRateTsMap<std::string>> mbsPrimaryRates_;
		std::optional<HistRateTsMap<wfmcm::MortgageRateType>> secondaryRates_;
		std::optional<wfmcm::lookup<double, wfmcm::ExtendedVectorKey<QuantLib::Date>>> historyDailySofr_;
		std::optional<wfmcm::lookup<double, wfmcm::ExtendedVectorKey<QuantLib::Date>, wfmcm::VectorKey<wfmcm::GFeeType>>> gfee_;
	};

	struct MarketData
	{
		QuantLib::Date marketDate_;
		std::optional<wfmcm::VolatilityStructureInput> sofrVolInput_;
		std::optional<wfmcm::RawCurveInput> rawCurveInput_;
		std::optional<wfmcm::RawCurveInput> ustCurveInput_;
		std::optional<wfmcm::HistoricalIndexProvider<wfmcm::KeyRate>> secondaryRates_;
		std::optional<wfmcm::HistoricalIndexProvider<wfmcm::PrimaryRateType>> primaryRates_;
	};

	struct ParamSpecDetails
	{
		ApiModel modelType_;
		app::messages::ModelSettings settings_;
	};

	//FIXME: Convert messages::Scenarios into this class and read directly from json scenario file.
	//The scenario file needs to be converted to use map directly.
	struct Scenarios
	{
		std::map<std::string, app::messages::ValuationScenarioGroup> scenarioGroups_;
		std::pmr::vector<app::messages::ValuationScenario> scenarios_;
		wfmcm::DerivativeCalculation scenarioRiskFactorDerivativeCalc_;
	};

	struct ExecutionOptions
	{
		app::messages::ExecutionControl execControl_;
		uint8_t precision_{ 16 };
		bool sliceResult_{ false };
		bool useNewBehavioralArchitecture_{ false };
		std::string suiteRegistryConfigFile_;
	};
}

//-------------------------------------------------------------------------------------------
//                                    HANDLE TYPES
//-------------------------------------------------------------------------------------------
using HandleTypeSelection = std::tuple<
	std::monostate,
	//Requests                                                  HandleType
	app::messages::GenRatePathsForMortgageValuationRequest,		//ApiRequest_GenRatePathsForMortgageValuation
	app::messages::CalcValueForMortgageRequest,					//ApiRequest_CalcValueForMortgage
	app::messages::CalcValueForMortgageWithSofrRatesRequest,	//ApiRequest_CalcValueForMortgageWithSofrRates
	app::messages::CalcGreeksFromPricesRequest,					//ApiRequest_CalcGreeksFromPrices
	app::messages::AttribValueChangeByWaterfallRequest,			//ApiRequest_AttribValueChangeByWaterfall
	app::messages::GenPrimaryMortgageRatePathsRequest,			//ApiRequest_GenPrimaryMortgageRatePaths
	app::messages::GenSecondaryMortgageRatePathsRequest,		//ApiRequest_GenSecondaryMortgageRatePaths
	app::messages::CalcValueForMonthEndRollRequest,				//ApiRequest_CalcValueForMonthEndRoll
	app::messages::GenRatePathsForWaterfallAttributionRequest,	//ApiRequest_GenRatePathsForWaterfallAttribution
	app::messages::CalcBehavioralSpeedFromPrimaryRateRequest,	//ApiRequest_CalcBehavioralSpeedFromPrimaryRate
	app::messages::CalcValueForMortgageFromRatesRequest,		// ApiRequest_CalcValueForMortgageFromRates
	app::messages::CalcProfitabilityFromBehavioralSpeedsRequest, //ApiRequest_CalcProfitabilityFromBehavioralSpeeds
	app::messages::CancelRequest,								//ApiInternalRequest_Cancel
	//Other types
	std::pmr::vector<Handle>,									//HandleArray
	app::messages::DateSpec,									//DateSpec
	staging::MarketData,										//MarketData
	staging::HistoricalData,									//HistoricalData
	wfmcm::matrix<double>,										//FloatingPointMatrix
	wfmcm::matrix<double>,										//RatePathCollection
	app::messages::GreekSpec,									//GreekSpec
	wfmcm::MortgageValuationPortfolio,							//Portfolio
	staging::ParamSpecDetails,									//ModelSpec
	staging::ParamSpecDetails,									//SessionSpec
	app::messages::ModelOptions,								//ModelOptions
	wfmcm::RatePathsSet<wfmcm::Tenor>,							//TenorRatePathsMap
	wfmcm::RatePathsSet<wfmcm::KeyRate>,						//SofrRatePaths
	staging::Scenarios,											//UserScenarios
	wfmcm::VolatilityBasket,									//VolatilityBasket
	app::messages::CurveModificationSpec,						//CurveModificationSpec
	app::messages::CurveInterpolationSpec,						//CurveInterpolationSpec
	staging::ExecutionOptions,									//ExecOptions
	app::messages::PricingSpec,									//PricingSpec
	std::map<wfmcm::PrimaryRateType, std::tuple<wfmcm::InitialBasisInputType, double>>, //PrimaryRateInputs
	app::RunConfigResolver,
	app::vasara::ModelConfigData,							//ModelConfigData
	app::vasara::RequestContextData,						//RequestContextData
	app::vasara::ResultData,								//ResultData
	app::messages::CalcRatePaths				//CalcRatePaths
>;

//-------------------------------------------------------------------------------------------
//                                    HANDLE HELPERS
//-------------------------------------------------------------------------------------------
std::pair<std::string, HandleError>& handleError();

std::pair<std::string, HandleError>& globalError();

void setHandleError(Handle h, HandleError error);

std::string getGlobalTroubleShootInfo(const LibException& ex);

void clearHandleError(Handle h);

//Use to lock a handle
std::shared_lock<std::shared_mutex> handleReadLock(Handle h);

wfmutil::shared_unlock<std::shared_mutex> handleReadUnlock(Handle h);

std::unique_lock<std::shared_mutex> handleWriteLock(Handle h);

wfmutil::unique_unlock<std::shared_mutex> handleWriteUnlock(Handle h);

HandleData& handleData(Handle h);

RequestHandleData& requestHandleData(Handle h);

Handle asHandle(const HandleData* handleData);

Handle getBoundHandle(Handle h, HandleBinding b);

bool isHandleTypeAnyOf(Handle h, std::initializer_list<HandleType> types);

std::pmr::vector<std::string> to_string(std::initializer_list<HandleType> types);

template <HandleType H>
using HandleObjectType = std::tuple_element_t<static_cast<size_t>(H), HandleTypeSelection>;

template <HandleType H>
HandleObjectType<H>& handleObject(Handle h)
{
	return handleData(h).getObject<HandleObjectType<H>>();
}

//-------------------------------------------------------------------------------------------
//                                    MISC HELPERS
//-------------------------------------------------------------------------------------------
int getModelOptionsBinding(ApiModel model);
HandleBinding getBoundHandle(int setterId);
HandleError fillMarketData(
	Handle h,
	const staging::MarketData& from,
	app::messages::MortgageValuationMarketData* market);
HandleError fillMarketData(
	Handle h,
	const staging::MarketData& from,
	app::messages::PrimaryMortgagePathMarketData* market);
HandleError fillMarketData(
	Handle h,
	const staging::MarketData& from,
	app::messages::SecondaryMortgagePathMarketData* market);
HandleError fillHistoricalData(Handle h, const staging::HistoricalData& from, app::messages::MortgageValuationHistoricalData* hist);
HandleError fillHistoricalData(Handle h, const staging::HistoricalData& from, app::messages::PrimaryMortgagePathHistoricalData* hist);
HandleError fillHistoricalData(Handle h, const staging::HistoricalData& from, app::messages::SecondaryMortgagePathHistoricalData* hist);
std::set<std::string> supportedSpecParams(ApiModel model, bool isModelSpec);
const char* to_string(ApiModel p);
const char* to_string(ApiModelParameter p);
const char* to_string(ApiSessionParameter p);
ApiModelParameter convert(wfmcm::ModelParamName from, ApiModelParameter* to);
ApiSessionParameter convert(wfmcm::SessionParamName from, ApiSessionParameter* to);

//Get first handle from a handle array or from a single handle
Handle handleAtIndex(Handle maybeHandleArray, size_t pos);

using ApiModelTypes = std::variant<wfmcm::InterestRateModelType, wfmcm::SecondaryMortgageRateModelType,
	wfmcm::PrimaryMortgageRateModelType, wfmcm::MortgageBehavioralModelType,
	wfmcm::DiscountingModelType, wfmcm::CashflowModelType, wfmcm::MortgageBehavioralModelMapType>;
// Convert ApiModel to ApiModelTypes, which is the variant of all model types
ApiModelTypes convert(ApiModel modelType);

/**
 * Wraps the output of getError() in additional braces `{}` to ensure proper parsing
 * by the C-API call path logger.
 *
 * This function is intended for use exclusively within the CHECK_API_ERROR macro.
 *
 * @note The getError() function returns a JSON-formatted string containing escape characters
 * that are not correctly handled by the logger. By enclosing the string in `{}`, we ensure
 * it is parsed correctly without introducing any extra braces in the final log output.
 */
const char* getGlobalErrorStr();

}

#endif
