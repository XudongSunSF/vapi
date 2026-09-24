#include <src/app-common/api/WfmcmApiInternal.h>
#include <src/app-common/messages/util.h> 
#include <src/app-common/ModelSpecifications.h>
#include <src/core/behavioral/Models.h>
#include <src/core/cashflow/Models.h>
#include <src/core/pss/DynamicPss.h>
#if _WIN32 
#include <src/core/pss/AdcoPss.h>
#endif
#include <src/core/basis/StatisticalBasis.h>
#include <src/core/basis/TbaMarketBasis.h>
#include <src/core/interest-rate/ConstantInterestRateModel.h>
#include <src/core/interest-rate/StaticInterestRateModel.h>
#include <src/core/interest-rate/MonteCarloInterestRateModel.h>
#include <src/core/behavioral/BehavioralModelMap.h>
#include <mortgage/utility/memory/thread_local.h>
#include <algorithm>
#include <sstream>

using namespace std;
using namespace wfmcm;
using namespace app;
using namespace wf::mortgage::utility;
namespace msg = app::messages;
using namespace std::chrono_literals;

bool operator==(const Handle& lhs, const Handle& rhs) 
{
	return lhs.internal_ == rhs.internal_;
}

bool operator<(const Handle& lhs, const Handle& rhs) 
{
	return lhs.internal_ < rhs.internal_;
}

bool operator==(const Date& lhs, const Date& rhs)
{
	return lhs.yyyymmdd_ == rhs.yyyymmdd_;
}

bool operator<(const Date& lhs, const Date& rhs)
{
	return lhs.yyyymmdd_ < rhs.yyyymmdd_;
}

namespace app {

CLASS_OPERATOR_DEFINE(HandleError)

std::unique_ptr<HandleContainer> handles;
std::unique_ptr<RunConfigResolver> resolver;

HandleData::HandleData(HandleData&& other)
{
	*this = std::move(other);
}

HandleData::HandleData(const HandleData& other)
	: std::enable_shared_from_this<HandleData>()
{
	*this = other;
}

HandleData& HandleData::operator=(HandleData&& other)
{
	scoped_lock<recursive_mutex> lock(m_);
	scoped_lock<recursive_mutex> lock2(other.m_);
	unique_lock<shared_mutex> objLock(objectMutex_); //write
	unique_lock<shared_mutex> objLock2(other.objectMutex_); //write
	type_ = other.type_;
	object_ = move(other.object_);
	bindings_ = move(other.bindings_);
	error_ = move(other.error_);
	return *this;
}

HandleData& HandleData::operator=(const HandleData& other)
{
	scoped_lock<recursive_mutex> lock(m_);
	scoped_lock<recursive_mutex> lock2(other.m_);
	unique_lock<shared_mutex> objLock(objectMutex_); //write
	shared_lock<shared_mutex> objLock2(other.objectMutex_); //read
	type_ = other.type_;
	object_ = other.object_;
	bindings_ = other.bindings_;
	error_ = other.error_;
	return *this;
}

shared_mutex& HandleData::getLock() const
{
	return objectMutex_;
}

HandleType HandleData::getType() const
{
	return type_;
}

void HandleData::setError(HandleError e)
{
	scoped_lock<recursive_mutex> lock(m_);
	error_ = move(e);
}

HandleError HandleData::getError()
{
	scoped_lock<recursive_mutex> lock(m_);
	// Return a copy, NOT move(error_): this is an observer. Moving stole the
	// reason_/troubleshoot_ strings out of the stored error (the enum code was
	// left intact because scalar-move is a copy). Since both hasHandleError()
	// and getHandleError() call this, the standard check-then-get pattern
	// (`if (hasHandleError(h)) getHandleError(h)`) emptied the message during
	// the check, so getHandleError() reported the code with a blank message.
	// The error is reset explicitly via clearHandleError()/setError({}) at each
	// operation's start, so no consume-on-read is needed here.
	return error_;
}

void HandleData::bindHandle(Handle h, int setterId, SetterFunc&& func, bool doFill) 
{
	scoped_lock<recursive_mutex> lock(m_);
	bindings_[setterId] = Binding { handleData(h).shared_from_this(), std::move(func), doFill };
}

Handle HandleData::getBoundHandle(int setterId)
{
	scoped_lock<recursive_mutex> lock(m_);
	auto it = bindings_.find(setterId);
	return it == bindings_.end() ? NullHandle : asHandle(it->second.handleDataPtr_.get());
}

int HandleData::fill() 
{
	unique_lock<shared_mutex> objLock(objectMutex_); //we overwrite the entire object data
	scoped_lock<recursive_mutex> lock(m_); //lock internal

	//Preliminary prep
	error_ = preFill();
	if (error_.isError()) {
		return error_.error_;
	}

	//Fill
	for (const auto& [id, binding] : bindings_) {
		if (binding.doFill_) {
			int rc = binding.handleDataPtr_->fill();
			if (rc != ApiSuccess) {
				return rc;
			}
		}
		//read lock bound handle data since we're copying it
		shared_lock<shared_mutex> readLock(binding.handleDataPtr_->getLock());
		error_ = binding.setter_(asHandle(this), asHandle(binding.handleDataPtr_.get()), id);
		if (error_.isError()) {
			return error_.error_;
		}
	}

	//Final prep
	error_ = postFill();
	return error_.error_;
}

HandleError RequestHandleData::postFill()
{
	Handle h = asHandle(this);
	switch (type_) {
	case HandleType::CancelRequest:
		request_ = msg::makeRequest(getObject<HandleObjectType<HandleType::CancelRequest>>(), "api");
		break;
	case HandleType::GenRatePathsForMortgageValuationRequest:
		request_ = msg::makeRequest(getObject<HandleObjectType<HandleType::GenRatePathsForMortgageValuationRequest>>(), "api");
		break;
	case HandleType::CalcGreeksFromPricesRequest:
		request_ = msg::makeRequest(getObject<HandleObjectType<HandleType::CalcGreeksFromPricesRequest>>(), "api");
		break;
	case HandleType::GenPrimaryMortgageRatePathsRequest:
		request_ = msg::makeRequest(getObject<HandleObjectType<HandleType::GenPrimaryMortgageRatePathsRequest>>(), "api");
		break;
	case HandleType::GenSecondaryMortgageRatePathsRequest:
		request_ = msg::makeRequest(getObject<HandleObjectType<HandleType::GenSecondaryMortgageRatePathsRequest>>(), "api");
		break;
	case HandleType::CalcValueForMonthEndRollRequest:
		request_ = msg::makeRequest(getObject<HandleObjectType<HandleType::CalcValueForMonthEndRollRequest>>(), "api");
		break;
	case HandleType::GeneratePathForWaterfallAttributionRequest:
		request_ = msg::makeRequest(move(getObject<HandleObjectType<HandleType::GeneratePathForWaterfallAttributionRequest>>()), "api");
		break;
	case HandleType::AttribValueChangeByWaterfallRequest:
		request_ = msg::makeRequest(move(getObject<HandleObjectType<HandleType::AttribValueChangeByWaterfallRequest>>()), "api");
		break;
	case HandleType::CalcValueForMortgageRequest:
		request_ = msg::makeRequest(move(getObject<HandleObjectType<HandleType::CalcValueForMortgageRequest>>()), "api");
		break;
	case HandleType::CalcBehavioralSpeedFromPrimaryRateRequest:
		request_ = msg::makeRequest(move(getObject<HandleObjectType<HandleType::CalcBehavioralSpeedFromPrimaryRateRequest>>()), "api");
		break;
	case HandleType::CalcValueForMortgageFromRatesRequest:
		request_ = msg::makeRequest(getObject<HandleObjectType<HandleType::CalcValueForMortgageFromRatesRequest>>(), "api");
		break;
	case HandleType::CalcProfitabilityFromBehavioralSpeedsRequest:
		request_ = msg::makeRequest(getObject<HandleObjectType<HandleType::CalcProfitabilityFromBehavioralSpeedsRequest>>(), "api");
		break;
	default:
		return { ApiErrorOperationNotSupported, "Request data cannot be filled" };
	}
	id_ = request_.id();

	return {};
}

shared_lock<shared_mutex> handleReadLock(Handle h)
{
	if (!isValidHandle(h)) return {};
	return shared_lock<std::shared_mutex>{ handleData(h).getLock() };
}

shared_unlock<shared_mutex> handleReadUnlock(Handle h)
{
	if (!isValidHandle(h)) return {};
	return shared_unlock<std::shared_mutex>{ handleData(h).getLock() };
}

unique_lock<shared_mutex> handleWriteLock(Handle h)
{
	if (!isValidHandle(h)) return {};
	return unique_lock<shared_mutex>{ handleData(h).getLock() };
}

unique_unlock<shared_mutex> handleWriteUnlock(Handle h)
{
	if (!isValidHandle(h)) return {};
	return unique_unlock<shared_mutex>{ handleData(h).getLock() };
}

HandleData& handleData(Handle h) 
{ 
	return *reinterpret_cast<HandleData*>(h.internal_); 
}

RequestHandleData& requestHandleData(Handle h)
{
	return static_cast<RequestHandleData&>(handleData(h));
}

Handle asHandle(const HandleData* handleData)
{
	return { reinterpret_cast<unsigned long long>(handleData) };
}

Handle getBoundHandle(Handle h, HandleBinding b)
{
	return handleData(h).getBoundHandle(static_cast<int>(b));
}

pair<string, HandleError>& globalError()
{
	return store<pair<string, HandleError>, id::api_global_error>();
}
	
pair<string, HandleError>& handleError()
{
	return store<pair<string, HandleError>, id::api_handle_error>();
}

void setHandleError(Handle h, HandleError error)
{
	if (isValidHandle(h)) {
		handleData(h).setError(error);
	}
	globalError().second = move(error);
}

namespace{

void getGlobalTroubleShootInfoImpl(const LibException& ex, ostringstream& oss, bool& hasContext)
{
	// collect the context to osss recursively
	try {
		std::rethrow_if_nested(ex);
	} catch (const LibException& e) {
		getGlobalTroubleShootInfoImpl(e, oss, hasContext);
	}
	if (auto tt = ex.troubleshootTrace(); !tt.empty()) {
		auto fmt = troubleshoot_fmt::function_message;
		if (hasContext)
			oss << "; ";
		while (tt.size() > 1) {
			oss << tt.top().to_string(fmt) << "; ";
			tt.pop();
		}
		oss << tt.top().to_string(fmt);
		tt.pop();
		hasContext = true;
	}
}

} // namespace

std::string getGlobalTroubleShootInfo(const LibException& ex)
{
	// API calls only log the last exception
	// The global troubleshoot_info will be moved by the first thrown exception
	// In case of nested exceptions are rethrown, the global context is empty
	// Need to get to the root of the exception to e
	ostringstream oss;
	bool hasContext = false;
	getGlobalTroubleShootInfoImpl(ex, oss, hasContext);
	return oss.str();
}

void clearHandleError(Handle h)
{
	setHandleError(h, {});
}

bool isHandleTypeAnyOf(Handle h, initializer_list<HandleType> types)
{
	return any_of(types.begin(), types.end(), [h](const HandleType& type)->bool
		{ return type == HandleType::Any ? true : handleData(h).getType() == type; });
}

pmr::vector<string> to_string(initializer_list<HandleType> types)
{
	std::pmr::vector<string> names; 
	names.reserve(types.size());
	for (auto t : types) {
		names.push_back(to_string(t));
	}
	return names;
}

Handle HandleContainer::addHandleData(std::shared_ptr<HandleData> data) 
{
	unique_lock<shared_mutex> lock(m_);
	Handle h{ reinterpret_cast<unsigned long long>(data.get()) };
	handles_.insert(std::make_pair(h, move(data)));
	return h;
}

size_t HandleContainer::deleteHandleData(Handle h) 
{
	// Move the shared_ptr out of the map while the lock is held, then release
	// the lock BEFORE the shared_ptr destructor runs.  This is a defensive
	// pattern: HandleData types may hold bindings (shared_ptr<HandleData>)
	// whose destructors could trigger further container operations.  Releasing
	// the lock first avoids any potential re-entrancy on the non-recursive m_.
	std::shared_ptr<HandleData> detached;
	size_t num = 0;
	{
		unique_lock<shared_mutex> lock(m_);
		auto it = handles_.find(h);
		if (it != handles_.end()) {
			detached = std::move(it->second);
			handles_.erase(it);
			num = 1;
		}
	}
	// detached (if any) is destroyed here, outside the lock.
	h.internal_ = 0; //invalidate
	return num;
}

Handle HandleContainer::cloneHandleData(Handle h) 
{
	//copy handle data
	auto p = std::make_shared<HandleData>(handleData(h));
	//clear existing error if any
	p->setError({});
	return addHandleData(move(p));
}

bool HandleContainer::handleExists(Handle h) 
{
	shared_lock<shared_mutex> lock(m_);
	return handles_.find(h) != handles_.end();
}

int getModelOptionsBinding(ApiModel model)
{
	switch (model) {
		case ApiModel_BehavioralAdco:
		case ApiModel_BehavioralJftm:
		case ApiModel_BehavioralJatm:
		case ApiModel_BehavioralCrt:
		case ApiModel_BehavioralGfpm:
		case ApiModel_BehavioralCfpm:
		case ApiModel_BehavioralCapm:
		//case ApiModel_BehavioralAdcoTuning:
			return static_cast<int>(HandleBinding::PrepayModel) * ApiModel::ApiModel_UNKNOWN + model;
		case ApiModel_BehavioralPldm:
			return static_cast<int>(HandleBinding::LossModel) * ApiModel::ApiModel_UNKNOWN + model;
		case ApiModel_PrimaryRateAdco:
		case ApiModel_PrimaryRateDpss:
			return static_cast<int>(HandleBinding::PrimaryRateModel) * ApiModel::ApiModel_UNKNOWN + model;
		case ApiModel_SecondaryRateStatistical:
		case ApiModel_SecondaryRateTbaMarket:
			return static_cast<int>(HandleBinding::SecondaryRateModel) * ApiModel::ApiModel_UNKNOWN + model;
		case ApiModel_InterestRateConstant:
		case ApiModel_InterestRateStatic:
		case ApiModel_InterestRateMonteCarlo:
			return static_cast<int>(HandleBinding::InterestRateModel) * ApiModel::ApiModel_UNKNOWN + model;
		case ApiModel_CashflowModelMbsFixed:
		case ApiModel_CashflowModelMbsFloat:
		case ApiModel_CashflowModelMsr:
        case ApiModel_CashflowModelWholeLoanFixed:
			return static_cast<int>(HandleBinding::CashflowModel) * ApiModel::ApiModel_UNKNOWN + model;
		case ApiModel_DiscountingPoly:
		case ApiModel_DiscountingBusch:
		case ApiModel_DiscountingXva:
			return static_cast<int>(HandleBinding::DiscountingModel) * ApiModel::ApiModel_UNKNOWN + model;

		default:
			// if modelType is ApiModel_CurveBuilder then this function is not supposed to be called
			assert(false);
			return static_cast<int>(HandleBinding::Unknown) * ApiModel::ApiModel_UNKNOWN;
	}
}

HandleBinding getBoundHandle(int setterId) 
{
	return static_cast<HandleBinding>(setterId / ApiModel::ApiModel_UNKNOWN);
}

HandleError fillMarketData(
	Handle h, 
	const staging::MarketData& from, 
	msg::MortgageValuationMarketData* market)
{
	assert(market);
	if (!from.rawCurveInput_) {
		return { ApiErrorMissingRawCurveInput, "Missing raw curve input" };
	}
	market->rawSofrCurveInput_ = from.rawCurveInput_.value();
	if (!from.secondaryRates_) {
		return { ApiErrorMissingSecondaryRates, "Missing secondary mortgage market rates" };
	}
	market->secondaryRates_ = from.secondaryRates_.value();
	if (!from.sofrVolInput_) {
		return { ApiErrorMissingVolatilityStructureInput, "Missing volatility structure input" };
	}
	market->sofrVolInput_ = from.sofrVolInput_.value();
	//optional 
	market->rawUstCurveInput_ = from.ustCurveInput_;
	market->primaryRates_ = from.primaryRates_;
	return {};
}

HandleError fillMarketData(
	Handle h,
	const staging::MarketData& from,
	msg::PrimaryMortgagePathMarketData* market)
{
	assert(market);
	market->primaryRates_ = from.primaryRates_;
	return {};
}

HandleError fillMarketData(
	Handle h,
	const staging::MarketData& from,
	msg::SecondaryMortgagePathMarketData* market)
{
	assert(market);
	if (!from.rawCurveInput_ || (from.rawCurveInput_.value().index() != 1)) {
		return { ApiErrorMissingRawCurveInput, "Missing SOFR curve input" };
	}
	market->sofrCurveInput_ = get<1>(from.rawCurveInput_.value());
	if (!from.secondaryRates_) {
		return { ApiErrorMissingSecondaryRates, "Missing secondary mortgage market rates" };
	}
	market->secondaryRates_ = from.secondaryRates_.value();
	if (!from.sofrVolInput_) {
		return { ApiErrorMissingVolatilityStructureInput, "Missing volatility structure input" };
	}
	market->sofrVolInput_ = from.sofrVolInput_.value();
	return {};
}

HandleError fillHistoricalData(
	Handle h, 
	const staging::HistoricalData& from, 
	msg::MortgageValuationHistoricalData* hist)
{
	assert(hist);
	if (!from.hpi_) {
		return { ApiErrorMissingHpiData, "Missing HPI data" };
	}
	hist->hpi_ = from.hpi_.value();
	if (!from.unemployment_) {
		return { ApiErrorMissingUnemploymentData, "Missing unemployment data" };
	}
	hist->unemployment_ = from.unemployment_.value();
	if (!from.economicScenario_ && !from.mbsPrimaryRates_) {
		return { ApiErrorMissingPrimaryRates, "Missing economic scenario or mbs primary rates" };
	}
	if (from.economicScenario_) {
		hist->economicScenario_ = from.economicScenario_.value();
	}
	if (from.mbsPrimaryRates_) {
		hist->mbsPrimaryRates_ = from.mbsPrimaryRates_.value();
	}
	if (from.historyDailySofr_) {
		hist->historyDailySofr_ = from.historyDailySofr_.value();
	}
	else {
		hist->historyDailySofr_ = wfmcm::lookup<double, wfmcm::ExtendedVectorKey<QuantLib::Date>>();
	}
	if (from.gfee_) {
		hist->gfee_ = from.gfee_.value();
	}
	return {};
}

HandleError fillHistoricalData(
	Handle h,
	const staging::HistoricalData& from,
	msg::PrimaryMortgagePathHistoricalData* hist)
{
	assert(hist);
	if (!from.economicScenario_ && !from.mbsPrimaryRates_) {
		return { ApiErrorMissingPrimaryRates, "Missing economic scenario and/or mbs primary rates" };
	}
	if (from.economicScenario_) {
		hist->economicScenario_ = from.economicScenario_.value();
	}
	if (from.mbsPrimaryRates_) {
		hist->mbsPrimaryRates_ = from.mbsPrimaryRates_.value();
	}
	return {};
}


HandleError fillHistoricalData(
	Handle h,
	const staging::HistoricalData& from,
	app::messages::SecondaryMortgagePathHistoricalData* hist)
{
	assert(hist);
	if (!from.hpi_) {
		return { ApiErrorMissingHpiData, "Missing hpi data" };
	}
	hist->hpi_ = from.hpi_.value();
	return {};
}

template <typename Model>
set<string> specParamsOf(bool isModelSpec)
{
	set<string> params;
	if (isModelSpec) {
		ApiModelParameter apiParam;
		for (const auto& elem : Model::SupportedSpecParams()) {
			if (static_cast<int>(elem) < ApiModelParameter_UNKNOWN) {
				params.insert(to_string(convert(elem, &apiParam)));
			}
		}
	}
	else {
		ApiSessionParameter apiParam;
		for (const auto& elem : Model::SessionType::SupportedSpecParams()) {
			if (static_cast<int>(elem) < ApiSessionParameter_UNKNOWN) {
				params.insert(to_string(convert(elem, &apiParam)));
			}
		}
	}
	return params;
}

std::set<std::string> 
supportedSpecParams(ApiModel model, bool isModelSpec)
{
	switch (model) {
		case ApiModel_CashflowModelMsr:
			return specParamsOf<MsrCashflowModel>(isModelSpec);
		case ApiModel_CashflowModelMbsFixed:
			return specParamsOf<MbsFixedCashflowModel>(isModelSpec);
		case ApiModel_CashflowModelMbsFloat:
			return specParamsOf<MbsFloatCashflowModel>(isModelSpec);
		case ApiModel_CashflowModelWholeLoanFixed:
            return specParamsOf<WholeLoanFixedCashflowModel>(isModelSpec);
		case ApiModel_BehavioralJftm:
			return specParamsOf<JumboFixedTransitionModel>(isModelSpec);
		case ApiModel_BehavioralJatm:
			return specParamsOf<JumboArmTransitionModel>(isModelSpec);
		case ApiModel_BehavioralCrt:
			return specParamsOf<CrtModel>(isModelSpec);
		case ApiModel_BehavioralPldm:
			return specParamsOf<Pldm>(isModelSpec);
		case ApiModel_BehavioralCfpm:
			return specParamsOf<Cfpm>(isModelSpec);
		case ApiModel_BehavioralCapm:
			return specParamsOf<Capm>(isModelSpec);
		case ApiModel_BehavioralGfpm:
		    return specParamsOf<Gfpm>(isModelSpec);
		case ApiModel_PrimaryRateDpss:
			return specParamsOf<DynamicPss>(isModelSpec);
#if _WIN32 
		case ApiModel_PrimaryRateAdco:
			return specParamsOf<AdcoPss>(isModelSpec);
		case ApiModel_BehavioralAdco:
			return specParamsOf<Adco>(isModelSpec);
#endif
		case ApiModel_SecondaryRateStatistical:
			return specParamsOf<StatisticalBasis>(isModelSpec);
		case ApiModel_SecondaryRateTbaMarket:
			return specParamsOf<TbaMarketBasis>(isModelSpec);
		/*case ApiModel_BehavioralAdcoTuning:
			return specParamsOf<AdcoTuningModel>(isModelSpec);*/
		case ApiModel_InterestRateConstant:
			return specParamsOf<ConstantInterestRateModel>(isModelSpec);
		case ApiModel_InterestRateStatic:
			return specParamsOf<StaticInterestRateModel>(isModelSpec);
		case ApiModel_InterestRateMonteCarlo:
			return specParamsOf<MonteCarloInterestRateModel>(isModelSpec);
		case ApiModel_BehavioralModelMap:
			return specParamsOf<BehavioralModelMap>(isModelSpec);
		default:
			assert(false);
			throw std::runtime_error(std::format("Invalid model type: {}", (int)model));
	}
}

std::ostream& 
streamSpecParams(std::ostream& os, ApiModel model, bool isModelSpec)
{
	//NOTE: previously this function duplicated the switch in supportedSpecParams()
	//but was missing the Cfpm, Gfpm, Capm and BehavioralModelMap cases, so it threw
	//for models the library actually supports. Delegate to the single, complete
	//implementation instead of maintaining two switches.
	polyvar p("parameters");
	p << supportedSpecParams(model, isModelSpec);
	os << p;
	return os;
}

const char* to_string(ApiModel p)
{
	switch (p) {
		case ApiModel_BehavioralPldm: return "BehavioralPldm";
		case ApiModel_BehavioralAdco: return "BehavioralAdco";
		//case ApiModel_BehavioralAdcoTuning: return "BehavioralAdcoTuning";
		case ApiModel_BehavioralJftm: return "BehavioralJftm";
		case ApiModel_BehavioralJatm: return "BehavioralJatm";
		case ApiModel_BehavioralCrt: return "BehavioralCrt";
		case ApiModel_BehavioralCfpm: return "BehavioralCfpm";
		case ApiModel_BehavioralGfpm: return "BehavioralGfpm";
		case ApiModel_BehavioralCapm: return "BehavioralCapm";
		case ApiModel_BehavioralModelMap: return "BehavioralModelMap";
		case ApiModel_PrimaryRateDpss: return "PrimaryRateDpss";
		case ApiModel_PrimaryRateAdco: return "PrimaryRateAdco";
		case ApiModel_SecondaryRateTbaMarket: return "SecondaryRateTbaMarket";
		case ApiModel_SecondaryRateStatistical: return "SecondaryRateStatistical";
		case ApiModel_InterestRateConstant: return "InterestRateConstant";
		case ApiModel_InterestRateStatic: return "InterestRateStatic";
		case ApiModel_InterestRateMonteCarlo: return "InterestRateMonteCarlo";
		case ApiModel_CurveBuilder: return "CurveBuilder";
		case ApiModel_CurveInterpolation: return "CurveInterpolation";
		case ApiModel_CashflowModelMsr: return "CashflowModelMsr";
		case ApiModel_CashflowModelMbsFixed: return "CashflowModelMbsFixed";
		case ApiModel_CashflowModelMbsFloat: return "CashflowModelMbsFloat";
        case ApiModel_CashflowModelWholeLoanFixed: return "CashflowModelWholeLoanFixed";
		case ApiModel_DiscountingPoly: return "DiscountingPoly";
		case ApiModel_DiscountingBusch: return "DiscountingBusch";
		case ApiModel_DiscountingXva: return "DiscountingXva";
		case ApiModel_Sensitivities: return "Sensitivities";
		case ApiModel_BehavioralAdcoTuning: return "BehavioralAdcoTuning";
		case ApiModel_Profitability: return "Profitability";
		default:
			throw std::runtime_error("Invalid model type: " + std::to_string((int)p));
	}
	return nullptr;
}

const char* to_string(ApiModelParameter p)
{
	switch (p) {
		case ApiModelParameter_ModelParams: return "ModelParams";
		case ApiModelParameter_BusinessDays: return "BusinessDays";
		case ApiModelParameter_AverageBusinessDays: return "AverageBusinessDays";
		case ApiModelParameter_CohortDefaultValues: return "CohortDefaultValues";
		case ApiModelParameter_LlpaData: return "LlpaData";
		case ApiModelParameter_LlpaMetadata: return "LlpaMetadata";
		case ApiModelParameter_ConformingLimits: return "ConformingLimits";
		case ApiModelParameter_OwacHistory: return "OwacHistory";
		case ApiModelParameter_ServicerSpeed: return "ServicerSpeed";
		case ApiModelParameter_MonteCarloCorrelation: return "MonteCarloCorrelation";
		// case ApiModelParameter_MonteCarloSettings: return "MonteCarloSettings";
		case ApiModelParameter_AdcoDataPath: return "AdcoDataPath";
		case ApiModelParameter_AdcoDllPath: return "AdcoDllPath";
		case ApiModelParameter_AdcoModelVersion: return "AdcoModelVersion";
		case ApiModelParameter_UNKNOWN: return "Unknown";
		default:
			throw std::runtime_error("Invalid model parameter: " + std::to_string((int)p));
	}
	return nullptr;
}

const char* to_string(ApiSessionParameter p)
{
	switch (p)
	{
		case ApiSessionParameter_PrepayMultiplier: return "PrepayMultiplier";
		case ApiSessionParameter_LossMultiplier: return "LossMultiplier";
		case ApiSessionParameter_RefinanceMultiplier: return "RefinanceMultiplier";
		case ApiSessionParameter_TurnoverMultiplier: return "TurnoverMultiplier";
		case ApiSessionParameter_Multipliers: return "Multipliers";
		case ApiSessionParameter_Dials: return "Dials";
		case ApiSessionParameter_HistoricalIndex: return "HistoricalIndex";
		case ApiSessionParameter_PrimaryRateSource: return "PrimaryRateSource";
		case ApiSessionParameter_OutputDir: return "OutputDir";
		case ApiSessionParameter_AggregatedDebugOutput: return "AggregatedDebugOutput";
		case ApiSessionParameter_DetailedDebugOutput: return "DetailedDebugOutput";
		case ApiSessionParameter_InstrumentFileOutputMode: return "InstrumentFileOutputMode";
		case ApiSessionParameter_ModelOutputAppend: return "ModelOutputAppend";
		case ApiSessionParameter_BoundBasisOption: return "BoundBasisOption";
		case ApiSessionParameter_ProjectionLength: return "ProjectionLength";
		case ApiSessionParameter_CalibrationBasket: return "CalibrationBasket";
		case ApiSessionParameter_CalibratedVolParams: return "CalibratedVolParams";
		case ApiSessionParameter_MonteCarloPaths: return "MonteCarloPaths";
		case ApiSessionParameter_YieldCurveLog: return "YieldCurveLog";
		case ApiSessionParameter_VolatilitySurfaceLog: return "VolatilitySurfaceLog";
		case ApiSessionParameter_SommCalibrationLog: return "SommCalibrationLog";
		case ApiSessionParameter_SommSimulationLog: return "SommSimulationLog";
		case ApiSessionParameter_DiagLogForBaseScnOnly: return "DiagLogForBaseScnOnly";
		case ApiSessionParameter_RateHistory: return "RateHistory";
		case ApiSessionParameter_EconScenario: return "EconScenario";
		case ApiSessionParameter_ProfitabilityEquityType: return "ProfitabilityEquityType";
		case ApiSessionParameter_ProfitabilityFalloutAdjust: return "ProfitabilityFalloutAdjust";
		case ApiSessionParameter_ProfitabilityDataType: return "ProfitabilityDataType";
		case ApiSessionParameter_ProfitabilityFullCost: return "ProfitabilityFullCost";
		case ApiSessionParameter_ProfitabilityRunWithABRD: return "ProfitabilityRunWithABRD";
		case ApiSessionParameter_ProfitabilityAssumptionsFolder: return "ProfitabilityAssumptionsFolder";
		case ApiSessionParameter_ProfitabilityInputCashflowFile: return "ProfitabilityInputCashflowFile";
		case ApiSessionParameter_ProfitabilityPricingDate: return "ProfitabilityPricingDate";
		case ApiSessionParameter_ProfitabilityMonthlyCashflow: return "ProfitabilityMonthlyCashflow";
		case ApiSessionParameter_UNKNOWN: return "Unknown";
		default:
			throw std::runtime_error("Invalid session parameter: " + std::to_string((int)p));
	}
}

ApiModelParameter convert(ModelParamName from, ApiModelParameter* to)
{
	assert(to);
	if (static_cast<int>(from) > ApiModelParameter_UNKNOWN) {
		*to = static_cast<ApiModelParameter>(static_cast<int>(from) - ApiModelParameter_UNKNOWN - 1);
	}
	else {
		*to = static_cast<ApiModelParameter>(from);
	}
	return *to;
}

ApiSessionParameter convert(SessionParamName from, ApiSessionParameter* to)
{
	assert(to);
	if (static_cast<int>(from) > ApiSessionParameter_UNKNOWN) {
		*to = static_cast<ApiSessionParameter>(static_cast<int>(from) - ApiSessionParameter_UNKNOWN - 1);
	}
	else {
		*to = static_cast<ApiSessionParameter>(from);
	}
	return *to;
}

Handle handleAtIndex(Handle maybeHandleArray, size_t pos)
{
	return (handleData(maybeHandleArray).getType() == HandleType::HandleArray) ?  
		handleObject<HandleType::HandleArray>(maybeHandleArray).at(pos) : maybeHandleArray;
}

ApiModelTypes convert(ApiModel modelType)
{
	switch (modelType) {
	case ApiModel::ApiModel_BehavioralPldm:
	case ApiModel::ApiModel_BehavioralJftm:
	case ApiModel::ApiModel_BehavioralJatm:
	case ApiModel::ApiModel_BehavioralCrt:
	case ApiModel::ApiModel_BehavioralCfpm:
	case ApiModel::ApiModel_BehavioralGfpm:
	case ApiModel::ApiModel_BehavioralCapm:
	case ApiModel::ApiModel_BehavioralAdcoTuning:
		return static_cast<MortgageBehavioralModelType>(modelType);
	// {FIXME}: Prepay Adco model is not supported yet, we need to combine the resolve behavioral model with the api function
	case ApiModel::ApiModel_BehavioralAdco:
		throw(std::runtime_error("Adco model is not supported in API"));
	case ApiModel::ApiModel_PrimaryRateDpss:
	case ApiModel::ApiModel_PrimaryRateAdco:
		return static_cast<PrimaryMortgageRateModelType>(modelType);
	case ApiModel::ApiModel_SecondaryRateTbaMarket:
	case ApiModel::ApiModel_SecondaryRateStatistical:
		return static_cast<SecondaryMortgageRateModelType>(modelType);
	case ApiModel::ApiModel_InterestRateConstant:
	case ApiModel::ApiModel_InterestRateStatic:
	case ApiModel::ApiModel_InterestRateMonteCarlo:
		return static_cast<InterestRateModelType>(modelType);
	case ApiModel::ApiModel_DiscountingPoly:
	case ApiModel::ApiModel_DiscountingBusch:
	case ApiModel::ApiModel_DiscountingXva:
		return static_cast<DiscountingModelType>(modelType);
	case ApiModel::ApiModel_CashflowModelMsr:
	case ApiModel::ApiModel_CashflowModelMbsFixed:
	case ApiModel::ApiModel_CashflowModelMbsFloat:
	case ApiModel::ApiModel_CashflowModelWholeLoanFixed:
		return static_cast<CashflowModelType>(modelType);
	case ApiModel::ApiModel_BehavioralModelMap:
		return static_cast<MortgageBehavioralModelMapType>(modelType);
	default:
		throw(std::runtime_error(std::format("Cannot convert {} to general model type in the library.", to_string(modelType))));
	}
}

const char* getGlobalErrorStr()
{
	const char* err = getError();
	stringstream ss;
	ss << "{" << err << "}";
	auto& errMsg = globalError().first;
	errMsg = ss.str();
	return errMsg.c_str();
}

} //namespace app

namespace wf::mortgage::utility::types
{
	using namespace app;

	CLASS_MEMBERS_BEGIN(HandleError)
		{ "error", &HandleError::error_ },
		{ "reason", &HandleError::reason_ },
		{ "troubleshoot_trace", &HandleError::troubleshoot_ }
	CLASS_MEMBERS_END
	CLASS_WITHOUT_KEYS(HandleError)
}