/**
 * @file CfpmInstrumentSession.h
 * @author Wells Fargo MMDC
 * @brief Unified CFPM instrument session struct and builder.
 * @copyright 2024 Wells Fargo MMDC
 *
 * The CPU and GPU backends previously each owned an `CfpmInstrumentEx` and a
 * builder whose `doBuild()` was ~90% identical. The instrument data is shared;
 * only a handful of blocks differ (asset-multiplier setup, diagnostic header,
 * FICO/adjusted-loan-size series, and the CPU-only rate-independent pre-calc).
 * Those are selected with `if constexpr` on the backend traits.
 */

#ifndef WFMCM_CFPM_INSTRUMENT_SESSION_H
#define WFMCM_CFPM_INSTRUMENT_SESSION_H

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <memory_resource>
#include <numeric>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <src/core/behavioral/IInstrumentEx.h>
#include <src/core/utilities/model_util.h>

#include <mortgage/utility/containers/context/context.h>
#include <mortgage/utility/time/time_series.h>
#include <mortgage/utility/types/optional.h>
#include <mortgage/utility/strings/to_string.h>

#include <src/app-common/InstrumentLogger.h>
#include <src/app-common/util.h>
#include <src/core/behavioral/cfpm/CfpmSession.h>
#include <src/core/behavioral/cfpm/detail/CfpmDetail.h>
#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/core/ContextComponentNameManager.h>
#include <src/core/MortgageCashflows.h>

namespace wfmcm {

//----------------------------------------------------------------------------
// InstrumentEx (shared by CPU and GPU)
//----------------------------------------------------------------------------

struct Cfpm;
struct CfpmGpuImpl;
struct CfpmSession;
template <class Model, class Traits> struct CfpmInstrumentExBuilder;

/**
 * CFPM `InstrumentEx` subclass.
 *
 * Instrument dependent, Rate independent, Model dependent.
 *
 * The same struct is used by both backends; the extra GPU accessors simply
 * expose data that already exists (they are harmless on the CPU path).
 */
struct CfpmInstrumentEx : public InstrumentEx {
    //
    // {FIXME}
    //
    // standardize values in % or decimals, and functions should follow the
    // same way mortgage items (sic, missing other explanation?)
    //

    int origTerm() const noexcept { return mortgage_->OrigTerm; }
    int wala() const noexcept { return mortgage_->Wala; }
    auto wac() const noexcept { return mortgage_->Wac; }
    const auto& state() const noexcept { return mortgage_->State; }
    auto issuer() const noexcept { return mortgage_->Issuer; }

    // calculated items
    auto amortLength() const noexcept { return amortLength_; }

    double fico() const
    {
        return calcExCore_.valid() ? calcExCore_.value().waoCreditScore() : waoCreditScore_;
    }

    double oals() const noexcept { return waoLoanSize_; }
    double als() const noexcept { return wacLoanSize_; }
    double oltv() const noexcept { return waoLoanToValue_ * 100.; }
    double piw() const noexcept { return propertyInspectionWaiver_; }
    double wacDiscFactor() const noexcept { return wacDiscFactor_; }
    double chancelTPO() const noexcept { return channel_tpo_; }
    const auto& occupancy() const noexcept { return occupancy_; }
    const auto& purpose() const noexcept { return purpose_; }
    const auto& property() const noexcept { return property_; }
    const auto& channel() const noexcept { return channel_; }
    const auto& rfSpeed() const noexcept { return rfSpeed_; }
    const auto& htSpeed() const noexcept { return htSpeed_; }
    // model mapping
    auto primaryRate() const noexcept { return primaryRate_; }
    auto modelType() const noexcept { return MortgageBehavioralModelType::Cfpm; }
    auto subModelType() const noexcept { return subModelType_; }

    // all series members should have the same size: [0, projLength-1]
    // NB: amortDatesSeries() dates are contiguous (same time unit). this is an
    // implementation detail some of the CfpmDetail.cpp functions rely on!
    const auto& amortDatesSeries() const noexcept { return amortDatesSeries_; }
    const auto& ficoSeries() const noexcept { return ficoSeries_; }
    const auto& loanSizeSeries() const noexcept { return loanSizeSeries_; }
    const auto& adjLoanSizeSeries() const noexcept { return adjLoanSizeSeries_; }
    const auto& ltvSeries() const noexcept { return ltvSeries_; }
    const auto& wamSeries() const noexcept { return wamSeries_; }
    const auto& walaSeries() const noexcept { return walaSeries_; }
    const auto& cumHpaSeries() const noexcept { return cumHpaSeries_; }

    // {TODO} document more; what are the units?

    // projection items [wala, wala + projLength - 1]
    // note: operator[] of std::vector is not strictly noexcept
    const auto& projDate(size_t idx) const
    {
        return amortDatesSeries_[idx + mortgage_->Wala];
    }

    auto season(size_t idx) const
    {
        return amortDatesSeries_[idx + mortgage_->Wala].month();
    }

    double projFico(size_t idx) const
    {
        return ficoSeries_[idx + mortgage_->Wala];
    }

    // unit: thousand
    double projLoanSize(size_t idx) const
    {
        return loanSizeSeries_[idx + mortgage_->Wala] / 1000.;
    }

    // unit: thousand
    double projAdjLoanSize(size_t idx) const
    {
        return adjLoanSizeSeries_[idx + mortgage_->Wala] / 1000.;
    }

    double projFactorRatio(size_t idx) const { return factorRatioSeries_[idx + mortgage_->Wala]; }
    double projLtv(size_t idx) const { return ltvSeries_[idx + mortgage_->Wala]; }
    double projWam(size_t idx) const { return wamSeries_[idx + mortgage_->Wala]; }
    double projWala(size_t idx) const { return walaSeries_[idx + mortgage_->Wala]; }
    double projRfTunedWala(size_t idx) const { return walaRfTunedSeries_[idx + mortgage_->Wala]; }
    double projHtTunedWala(size_t idx) const { return walaHtTunedSeries_[idx + mortgage_->Wala]; }
    double projCoTunedWala(size_t idx) const { return walaCoTunedSeries_[idx + mortgage_->Wala]; }
    double projCumHpa(size_t idx) const { return cumHpaSeries_[idx + mortgage_->Wala] * 100.; }
    double projHpa1y(size_t idx) const { return projHpa1ySeries_[idx] * 100.; }
    double projHpa2y(size_t idx) const { return projHpa2ySeries_[idx] * 100.; }
    double projHpa3y(size_t idx) const { return projHpa3ySeries_[idx] * 100.; }
    double projHpa5y(size_t idx) const { return projHpa5ySeries_[idx] * 100.; }

    // elbow shift
    const auto& elbowRateIndependent() const noexcept { return elbowRateIndependent_; }
    // model component
    const auto& burnoutCltvMult() const noexcept { return burnoutCltvMult_; }
    const auto& rfRateIndependent() const noexcept { return rfRateIndependent_; }
    const auto& htRateIndependent() const noexcept { return htRateIndependent_; }
    const auto& coRateIndependent() const noexcept { return coRateIndependent_; }
    const auto& ctRateIndependent() const noexcept { return ctRateIndependent_; }

    // calculated items
    int wam() const noexcept { return wam_; }
    auto origDate() const noexcept { return origDate_; }
    double sato() const noexcept { return sato_; }

    auto projectionLength() const noexcept { return projectionLength_; }
    auto factorDate() const noexcept { return factorDate_; }

    // GPU data accessors (always available; unused by the CPU backend)
    const auto& totalSmmMult() const noexcept { return multiplier_.TotalSmmData; }
    const auto& refinanceSmmMult() const noexcept { return multiplier_.RefinanceSmmData; }
    const auto& turnoverSmmMult() const noexcept { return multiplier_.TurnoverSmmData; }
    const auto& cashoutSmmMult() const noexcept { return multiplier_.CashoutSmmData; }
    const auto& curtailmentSmmMult() const noexcept { return multiplier_.CurtailmentSmmData; }
    const auto& factorRatioSeries() const noexcept { return factorRatioSeries_; }
    const auto& refiTunedWalaSeries() const noexcept { return walaRfTunedSeries_; }
    const auto& turnoverTunedWalaSeries() const noexcept { return walaHtTunedSeries_; }
    const auto& cashoutTunedWalaSeries() const noexcept { return walaCoTunedSeries_; }
    const auto& projHpa1ySeries() const noexcept { return projHpa1ySeries_; }
    const auto& projHpa2ySeries() const noexcept { return projHpa2ySeries_; }
    const auto& projHpa3ySeries() const noexcept { return projHpa3ySeries_; }
    const auto& projHpa5ySeries() const noexcept { return projHpa5ySeries_; }

private:
    // note: could make this public too
    using dvector = std::pmr::vector<double>;

    size_t projectionLength_;
    std::chrono::year_month factorDate_;
    // calculated items
    int wam_;
    size_t amortLength_;
    std::chrono::year_month origDate_;
    PrimaryRateType primaryRate_;
    MortgageBehavioralSubModelType subModelType_;
    double sato_;
    double waoCreditScore_;
    double waoLoanToValue_;
    double wacLoanToValue_;
    double waoLoanSize_;
    double wacLoanSize_;
    double propertyInspectionWaiver_;
    double channel_tpo_;
    double wacDiscFactor_;
    std::map<OccupancyType, double> occupancy_;
    std::map<PurposeType, double> purpose_;
    std::map<PropertyType, double> property_;
    std::map<OriginationChannel, double> channel_;
    std::map<ServicerSpeedType, double> rfSpeed_;
    std::map<ServicerSpeedType, double> htSpeed_;

    std::pmr::vector<std::chrono::year_month> amortDatesSeries_;
    dvector ficoSeries_;
    dvector loanSizeSeries_;
    dvector adjLoanSizeSeries_;
    dvector factorRatioSeries_;
    dvector ltvSeries_;
    dvector wamSeries_;
    dvector walaSeries_;
    dvector cumHpaSeries_;

    dvector walaRfTunedSeries_;
    dvector walaHtTunedSeries_;
    dvector walaCoTunedSeries_;

    // projection only
    dvector projHpa1ySeries_;
    dvector projHpa2ySeries_;
    dvector projHpa3ySeries_;
    dvector projHpa5ySeries_;

    // elbow shift
    dvector elbowRateIndependent_;

    // model component
    dvector burnoutCltvMult_;
    dvector rfRateIndependent_;
    dvector htRateIndependent_;
    dvector coRateIndependent_;
    dvector ctRateIndependent_;

    template <class Model, class Traits>
    friend struct CfpmInstrumentExBuilder;
};

//----------------------------------------------------------------------------
// InstrumentEx builder (templated on the backend model)
//----------------------------------------------------------------------------

/**
 * Builder class for the `CfpmInstrumentEx`.
 *
 * @tparam Model  `Cfpm` (CPU) or `CfpmGpuImpl` (GPU).
 * @tparam Traits `model_traits<cpu_tag>` or `model_traits<gpu_tag>`.
 */
template <class Model, class Traits>
struct CfpmInstrumentExBuilder
  : public InstrumentExBuilder<CfpmInstrumentExBuilder<Model, Traits>, Model, CfpmSession, CfpmInstrumentEx> {
    using Base = InstrumentExBuilder<CfpmInstrumentExBuilder<Model, Traits>, Model, CfpmSession, CfpmInstrumentEx>;

    /**
     * Set the builder factor date.
     */
    CfpmInstrumentExBuilder&
    withFactorDate(const std::chrono::year_month& factorDate) noexcept {
        factorDate_ = &factorDate;
        return *this;
    }

private:
    void extractFromContext(const context&) final;
    void doBuild() final;

    const std::chrono::year_month* factorDate_{nullptr};
};

template <class Model, class Traits>
void CfpmInstrumentExBuilder<Model, Traits>::extractFromContext(const context& c)
{
    Base::extractFromContext(c);
}

// The original builders relied on file-scope `using namespace std;` and
// `using namespace detail;`. Reproduce that lookup context at block scope so
// the large body can stay verbatim (and identical to the original behavior).
template <class Model, class Traits>
void CfpmInstrumentExBuilder<Model, Traits>::doBuild()
{
    using namespace std;
    using namespace detail;

    // checks
    if (!ex_.mortgage_) {
        THROW("Cfpm instrumentEx builder: mortgage not set.");
    }
    if (!factorDate_) {
        THROW("Cfpm instrumentEx builder: factorDate not set.");
    }
    if (!hpi_) {
        THROW("Missing HPI data");
    }

    const auto& primaryRates = session_->historicalMbsPrimaryRates().lookupTs_;
    const auto& derivedPrimaryRates = session_->derivedHistRates();
    // note: probably just cfpm_rate_types? as this doesn't seem related to
    // turbo rate calculation (even though currently all primary rates are
    // used for turbo rate calculation, so there is no problem here)
    const auto& requiredPrimaryRates = detail::cfpm_turbo_rate_types();
    const auto& mortgage = *ex_.mortgage_;
    const auto& assetMultiplier = mortgage.AssetMultiplier;
    auto factorDate = *factorDate_;
    const auto& mult = session_->multiplier();
    auto mortErrors = model_->validateMortgage(mortgage);
    mortErrors.throwIfErrors(mortgage.Id);

    // create the instrument logger. We use truncate mode in order to erase files
    // created in a previous run.
    storeInstrumentId(mortgage.Id);

    InstrumentLogger il{
        {
            .diag_ = session_->diagnosticSettings(),
            .fileSuffix_ = "BehavioralModelDetailOutput"
        }
    };
    if constexpr (!Traits::is_gpu) {
        if (session_->diagnosticSettings().debugOutput_) {
            // note: string literals are concatenated by preprocessor
            writeResidentialMortgageHeader(
                "SATO Override,Total Smm Asset Multiplier,Refinance Smm Asset Multiplier,"
                "Turnover Smm Asset Multiplier,Curtailment Smm Asset Multiplier,Cashout Smm Asset Multiplier,"
                "Vintage,ProductType,PrimaryRate,SubModelType,Sato,"
                "HistCurtail,RfServicerFast,RfServicerNormal,RfServicerSlow,"
                "HtServicerFast,HtServicerNormal,HtServicerSlow,AlsAdj"
            );
        }
    }

    // get SubModelType, PrimaryRate, ProductType
    ex_.subModelType_ = detail::getCfpmSubModelType(mortgage);
    ex_.primaryRate_ = detail::getCfpmPrimaryRate(ex_.subModelType());
    auto subModelTypeString = to_string(ex_.subModelType());
    MortgageProductType prodType = to_string(ex_.issuer())
        + subModelTypeString.substr(subModelTypeString.length() - 2);

    // CPU-only prepay info for the rate-independent pre-calculation blocks.
    const auto* prepay = [&]() -> const detail::CfpmCalculator::Prepay* {
        if constexpr (Traits::is_gpu) {
            return nullptr;
        }
        else {
            auto calcIt = model_->calculators().find(ex_.subModelType());
            if (calcIt == model_->calculators().end())
                THROW("Asset: " + ex_.instrumentId() + " is mapped to " + to_string(ex_.subModelType()) +
                    " model, but this model is not in the parameter map.");
            return &model_->calculators().at(ex_.subModelType()).Prepay_;
        }
    }();

    // asset level tuning
    if constexpr (Traits::is_gpu) {
        MortgageBehavioralModelUserAssetMultiplier assetMult;
        auto assignAssetMultiplier = [](const std::string& multStr, double defaultVal) {
            if (multStr.empty()) {
                return std::pair<std::pmr::vector<double>, std::pmr::vector<double>>({ 0.0 }, { defaultVal });
            }
            return convertMultiplierString(multStr);
        };
        auto& multString = mortgage.AssetMultiplier;
        ex_.multiplier_.TotalSmmData = assignAssetMultiplier(multString.TotalSmm, assetMult.TotalSmmDefaultValue);
        ex_.multiplier_.RefinanceSmmData = assignAssetMultiplier(multString.RefinanceSmm, assetMult.RefinanceSmmDefaultValue);
        ex_.multiplier_.TurnoverSmmData = assignAssetMultiplier(multString.TurnoverSmm, assetMult.TurnoverSmmSmmDefaultValue);
        ex_.multiplier_.CashoutSmmData = assignAssetMultiplier(multString.CashoutSmm, assetMult.CashoutSmmDefaultValue);
        ex_.multiplier_.CurtailmentSmmData = assignAssetMultiplier(multString.CurtailmentSmm, assetMult.CurtailmentSmmDefaultValue);
        ex_.multiplier_.TotalMdr = buildAssetMultiplierFunc(multString.TotalMdr, assetMult.TotalMdrDefaultValue);
        ex_.multiplier_.TotalSeverity = buildAssetMultiplierFunc(multString.TotalSeverity, assetMult.TotalSeverityDefaultValue);
    }
    else {
        ex_.multiplier_ = buildAssetMultiplier(mortgage);
    }

    //--------------------------intermediate value calculation started----------------------------------

    // calculated items
    ex_.wam_ = calculateWam(mortgage.Wam);
    ex_.origDate_ = calculateOrigDate(factorDate, ex_.wala());
    ex_.factorDate_ = factorDate;
    auto satoDate = ex_.origDate() - std::chrono::months(2);

    // check if primary rate of this asset is one of supported by the model
    if (std::find(requiredPrimaryRates.begin(), requiredPrimaryRates.end(),
        ex_.primaryRate()) == requiredPrimaryRates.end()) {
        THROW(std::format("{} is not a primary rate supported by CFPM",
            to_string(ex_.primaryRate())));
    }

    // {FIXME} improve the check using missing value?
    // {FIXME} create a utility function to obtain primary rates on SATO date (wait for standard rate structure)
    auto primaryRatesPos = primaryRates.key<1>().pos(ex_.primaryRate());
    auto derivedPrimaryRatesPos = derivedPrimaryRates.key<1>().pos(ex_.primaryRate());
    if (primaryRatesPos == wf::mortgage::utility::lookup_constant::MISSING_KEY &&
        derivedPrimaryRatesPos == wf::mortgage::utility::lookup_constant::MISSING_KEY) {
        THROW(std::format("Unable to find {} in historical primary rates.", to_string(ex_.primaryRate())));
    }
    const auto& thisRates = (primaryRatesPos != wf::mortgage::utility::lookup_constant::MISSING_KEY) ?
        primaryRates : derivedPrimaryRates;

    if (thisRates.key<0>().startDate() > satoDate || thisRates.key<0>().endDate() < satoDate) {
        THROW("Historical primary rates data is not available on the date: " + std::to_string(satoDate));
    }

    if (is_unset_value(mortgage.PropertyInspectionWaiver)) {
        ex_.propertyInspectionWaiver_ = 0.;
    }
    else {
        ex_.propertyInspectionWaiver_ = mortgage.PropertyInspectionWaiver;
    }

    // if SATO dial is provided, use SATO directly
    if (is_unset_value(mortgage.AssetMultiplier.SATOOverride)) {
        ex_.sato_ = ex_.wac() - thisRates.value(satoDate, ex_.primaryRate());
    }
    else {
        ex_.sato_ = mortgage.AssetMultiplier.SATOOverride;
    }

    ex_.projectionLength_ = calculateProjectionLength(ex_.wam(), session_->projectionLength());
    ex_.amortLength_ = size_t(ex_.wala()) + ex_.projectionLength();
    ex_.wacDiscFactor_ = calculateDiscFactor(ex_.wac());

    // cohort default value
    const ResidentialMortgage* defaultCohort = getCohortDefault(model_->cohortDefaults(), prodType, ex_.origDate().year());

    ex_.waoCreditScore_ = valueOrDefault(mortgage, defaultCohort, "WaoCreditScore",
        [](const ResidentialMortgage* m) { return m == nullptr ? wfmutil::unset_value<double> : m->WaoCreditScore; });
    ex_.waoLoanToValue_ = valueOrDefault(mortgage, defaultCohort, "WaoLoanToValue",
        [](const ResidentialMortgage* m) { return m == nullptr ? wfmutil::unset_value<double> : m->WaoLoanToValue; });
    ex_.waoLoanSize_ = valueOrDefault(mortgage, defaultCohort, "WaoLoanSize",
        [](const ResidentialMortgage* m) { return m == nullptr ? wfmutil::unset_value<double> : m->WaoLoanSize; });
    ex_.property_ = valueOrDefault(mortgage, defaultCohort, "Property",
        [](const ResidentialMortgage* m) -> const decltype(m->Property)& {
        return m == nullptr ? Empty<decltype(m->Property)>::v : m->Property; });
    ex_.occupancy_ = valueOrDefault(mortgage, defaultCohort, "Occupancy",
        [](const ResidentialMortgage* m) -> const decltype(m->Occupancy)& {
        return m == nullptr ? Empty<decltype(m->Occupancy)>::v : m->Occupancy; });
    ex_.purpose_ = valueOrDefault(mortgage, defaultCohort, "Purpose",
        [](const ResidentialMortgage* m) -> const decltype(m->Purpose)& {
        return m == nullptr ? Empty<decltype(m->Purpose)>::v : m->Purpose; });
    ex_.channel_ = valueOrDefault(mortgage, defaultCohort, "Channel",
        [](const ResidentialMortgage* m) -> const decltype(m->Channel)& {
        return m == nullptr ? Empty<decltype(m->Channel)>::v : m->Channel; });

    // servicer speed
    // if sum < 1, fill 1-sum with UNKNOWN
    auto servicerDistribution = mortgage.ServicerDistribution;
    detail::fillMissingWithBaseCategory(servicerDistribution, std::string("UNKNOWN"));

    ex_.rfSpeed_ = calculateServicerSpeedMap(model_->rfServicerSpeed(), servicerDistribution);
    ex_.htSpeed_ = calculateServicerSpeedMap(model_->htServicerSpeed(), servicerDistribution);

    pmr::vector<PropertyType> requiredPropertyType =
    { PropertyType::SingleFamily, PropertyType::MultiFamily, PropertyType::Condo, PropertyType::ManufacturedHome };

    pmr::vector<OccupancyType> requiredOccupancyType =
    { OccupancyType::Investor, OccupancyType::SecondHome };
    pmr::vector<ServicerSpeedType> requiredServicerSpeedType =
    { ServicerSpeedType::Fast, ServicerSpeedType::Normal, ServicerSpeedType::Slow };
    pmr::vector<OriginationChannel> requiredChannelType =
    { OriginationChannel::Broker, OriginationChannel::Correspondent, OriginationChannel::Retail };

    fillEmptyKeyWithZero<PropertyType>(ex_.property_, requiredPropertyType);
    fillEmptyKeyWithZero<OccupancyType>(ex_.occupancy_, requiredOccupancyType);
    fillEmptyKeyWithZero<OriginationChannel>(ex_.channel_, requiredChannelType);
    fillEmptyKeyWithZero<PurposeType>(ex_.purpose_, PurposeType::Purchase);
    fillEmptyKeyWithZero<ServicerSpeedType>(ex_.rfSpeed_, requiredServicerSpeedType);
    fillEmptyKeyWithZero<ServicerSpeedType>(ex_.htSpeed_, requiredServicerSpeedType);

    ex_.rfSpeed_.at(ServicerSpeedType::Normal) = 1.0 - ex_.rfSpeed_.at(ServicerSpeedType::Fast) - ex_.rfSpeed_.at(ServicerSpeedType::Slow);
    ex_.htSpeed_.at(ServicerSpeedType::Normal) = 1.0 - ex_.htSpeed_.at(ServicerSpeedType::Fast) - ex_.htSpeed_.at(ServicerSpeedType::Slow);

    ex_.channel_tpo_ = calculateChannelTpo(ex_.channel());

    auto u = calculateDiscFactor(ex_.wac());
    auto un = 1. - std::pow(u, ex_.origTerm());
    [[maybe_unused]] auto ur = 1. - std::pow(u, ex_.wam());

    // if WacLoanSize is not available, use the formula to calculate it
    double annuityAmount = ex_.oals() * (ex_.wac() / ANNUAL_TO_MONTHLY_RATE_FACTOR) / un;
    ex_.wacLoanSize_ = annuityAmount * (1. - std::pow(u, ex_.wam())) / (ex_.wac() / ANNUAL_TO_MONTHLY_RATE_FACTOR);

    // amort dates
    // note: if amortDatesSeries_ is contiguous why not just have a starting
    // amortization date and an ending one to define a contiguous range?
    ex_.amortDatesSeries_.reserve(ex_.amortLength());
    for (size_t i = 0; i < ex_.amortLength(); ++i) {
        ex_.amortDatesSeries_.emplace_back(ex_.origDate() + std::chrono::months(i));
    }
    checkSeriesSize<std::chrono::year_month>(ex_.amortDatesSeries_, ex_.amortLength(), "Amort Dates Series");

    // wala
    ex_.walaSeries_.resize(ex_.amortLength());
    std::iota(ex_.walaSeries_.begin(), ex_.walaSeries_.end(), 0.0);
    checkSeriesSize<double>(ex_.walaSeries_, ex_.amortLength(), "Wala Series");

    // cumhpa
    ex_.cumHpaSeries_ = calculateCumLookup(ex_.amortLength(), ex_.origDate(), *hpi_, mortgage.State, ex_.amortDatesSeries_);

    // projected Hpa lagged series for 1y, 2y, 3y, 5y
    vector<int> monthlag = { 12, 24, 36, 60 };
    map<int, pmr::vector<double>> hpaYearLag = calculateStateWeightedLookupLags(ex_.projectionLength(), ex_.factorDate(),
        *hpi_, mortgage.State, monthlag);
    if (hpaYearLag.find(0) != hpaYearLag.end())
        ex_.projHpa1ySeries_ = std::move(hpaYearLag[0]);
    else
        THROW("Cfpm hpa of 12 months lag not calculated");

    if (hpaYearLag.find(1) != hpaYearLag.end())
        ex_.projHpa2ySeries_ = std::move(hpaYearLag[1]);
    else
        THROW("Cfpm hpa of 24 months lag not calculated");

    if (hpaYearLag.find(2) != hpaYearLag.end())
        ex_.projHpa3ySeries_ = std::move(hpaYearLag[2]);
    else
        THROW("Cfpm hpa of 36 months lag not calculated");

    if (hpaYearLag.find(3) != hpaYearLag.end())
        ex_.projHpa5ySeries_ = std::move(hpaYearLag[3]);
    else
        THROW("Cfpm hpa of 60 months lag not calculated");

    if constexpr (!Traits::is_gpu) {
        // fico Series (computed on the GPU for the GPU backend)
        ex_.ficoSeries_ = detail::calculateCuredCreditScore(
            ex_.fico(),
            prepay->ElbowFicoCureCut,
            ex_.amortLength(),
            prepay->ElbowFicoWalaCure
        );
        checkSeriesSize<double>(ex_.ficoSeries(), ex_.amortLength(), "Fico Series");
    }

    // monthly payment
    double monthlyPayment = LoanCashflows::payment(
        ex_.oals(), ex_.wac() / 100., mortgage.OrigTerm);

    // solve historical curtail smm
    double histCurtailSmm = 0.;
    if (ex_.wala() != 0 && ex_.wam() + ex_.wala() < ex_.origTerm()) {
        try {
            histCurtailSmm = LoanCashflows::breakevenCurtailmentSmm(
                ex_.wac() / 100., mortgage.Wala, monthlyPayment, ex_.als(), ex_.oals());
        }
        catch (...) {
            THROW("Unable to solve the historical curtailment, please verify the collateral data is correct.");
        }
    }
    double calcWaoLoanSize = LoanCashflows::balance0(
        ex_.wac() / 100., ex_.wala(), monthlyPayment, ex_.als(), histCurtailSmm);

    // loan size Series
    ex_.loanSizeSeries_ = calculateAlsSeries(ex_.projectionLength(), size_t(ex_.wala()),
        monthlyPayment, ex_.wac(), calcWaoLoanSize, histCurtailSmm);
    checkSeriesSize<double>(ex_.loanSizeSeries_, ex_.amortLength(), "Loan Size Series");

    // Adj loan-size factor (CPU-only; kept in scope for the diagnostic log).
    double alsAdjFactor = 1.0;
    if constexpr (!Traits::is_gpu) {
        // adj loan size series (computed on the GPU for the GPU backend)
        alsAdjFactor = prepay->CalsAdjMult(ex_.origDate(), ex_.oals() / 1000.); // use real original loan size from asset file
        ex_.adjLoanSizeSeries_.resize(ex_.loanSizeSeries().size());
        std::transform(ex_.loanSizeSeries().begin(), ex_.loanSizeSeries().end(), ex_.adjLoanSizeSeries_.begin(),
            [&](double value) {return value / alsAdjFactor; }
        );
    }

    // factor ratio
    auto zeroCurtLoanSize = calculateAlsZeroCurtSeries(ex_.amortLength(), ex_.origTerm(), ex_.wac(), monthlyPayment);
    ex_.factorRatioSeries_.resize(ex_.amortLength());
    std::ranges::transform(ex_.loanSizeSeries(), zeroCurtLoanSize, ex_.factorRatioSeries_.begin(),
        [](double loanSize, double zeroCurtLoanSize) {return loanSize / zeroCurtLoanSize; }
    );
    checkSeriesSize<double>(ex_.factorRatioSeries_, ex_.amortLength(), "Factor Ratio Series");

    // ltv Series
    ex_.ltvSeries_ = calculateLtvSeries(ex_.cumHpaSeries(), ex_.loanSizeSeries(), ex_.oltv());
    checkSeriesSize<double>(ex_.ltvSeries_, ex_.amortLength(), "Ltv Series");

    // wam Series
    ex_.wamSeries_ = calculateWamSeries(ex_.loanSizeSeries(), monthlyPayment, ex_.wac());
    checkSeriesSize<double>(ex_.wamSeries_, ex_.amortLength(), "Wam Series");

    if constexpr (!Traits::is_gpu) {
        ex_.walaRfTunedSeries_ = applyRampingMutiplier(ex_.amortDatesSeries(), ex_.walaSeries(), mult.RefinanceRamping);
        ex_.walaHtTunedSeries_ = applyRampingMutiplier(ex_.amortDatesSeries(), ex_.walaSeries(), mult.TurnoverRamping);
        ex_.walaCoTunedSeries_ = applyRampingMutiplier(ex_.amortDatesSeries(), ex_.walaSeries(), mult.CashoutRamping);
    }

    // overwrite calculated values with real data
    ex_.wamSeries_[0] = ex_.origTerm();
    ex_.wamSeries_[ex_.wala()] = ex_.wam(); // {Question} use raw wam or rounded wam?
    //--------------------------intermediate value calculation completed----------------------------------

    if constexpr (!Traits::is_gpu) {
        if (session_->diagnosticSettings().debugOutput_) {
            TLOG_ALWAYS(retrieveInstrumentLoggerId(), logging::comma_format,
                ex_.instrumentId(),
                to_string(mortgage.Issuer),
                maybe_optional(ex_.origTerm()),
                maybe_optional(ex_.wac()),
                maybe_optional(ex_.wam()),
                maybe_optional(ex_.wala()),
                to_string(mortgage.SecType),
                maybe_optional(mortgage.Coupon),
                maybe_optional(mortgage.GrossNetCouponSpread),
                maybe_optional(mortgage.WalaMod),
                maybe_optional(mortgage.MISD90PlusDaysDelinquent),
                maybe_optional(mortgage.MISD90PlusDaysDelinquentMod),
                QuantLib::to_string(mortgage.FactorDate),
                maybe_optional(ex_.fico()),
                maybe_optional(ex_.oals()),
                maybe_optional(ex_.oltv()),
                wf::mortgage::utility::strings::to_string(mortgage.Product),
                maybe_optional(mortgage.AaoLoanSize),
                maybe_optional(mortgage.AacLoanSize),
                maybe_optional(ex_.als()),
                maybe_optional(mortgage.OriginalWac),
                maybe_optional(mortgage.AmortizationTerm),
                maybe_optional(mortgage.InitialResetMonths),
                maybe_optional(mortgage.RateResetFrequency),
                maybe_optional(mortgage.FirstCap),
                maybe_optional(mortgage.FirstFloor),
                maybe_optional(mortgage.ResetCap),
                maybe_optional(mortgage.ResetFloor),
                maybe_optional(mortgage.LifeCap),
                maybe_optional(mortgage.LifeFloor),
                maybe_optional(mortgage.GrossMargin),
                maybe_optional(mortgage.LookBackDays),
                to_string(mortgage.RateIndex),
                maybe_optional(mortgage.Margin),
                maybe_optional(mortgage.FirstTimeBuyer),
                maybe_optional(ex_.piw()),
                writeMapToString(servicerDistribution, ":", ";"),
                detail::filterMapView(OccupancyTypeEnumListU_v, ex_.occupancy()),
                detail::filterMapView(PurposeTypeEnumListU_v, ex_.purpose()),
                detail::filterMapView(PropertyTypeEnumListU_v, ex_.property()),
                detail::filterMapView(OriginationChannelEnumListU_v, ex_.channel()),
                detail::filterMapView(LoanStatusEnumListU_v, mortgage.Status),
                detail::filterMapView(DPATypeEnumListU_v, mortgage.DPA),
                detail::filterMapView(GnSubPoolTypeEnumListU_v, mortgage.SubPool),
                detail::filterMapView(DocumentTypeEnumListU_v, mortgage.Document),
                detail::filterMapView(StateEnumList_v, ex_.state()),
                detail::filterMapView(CrtLoanStatusEnumListU_v, mortgage.CrtStatus),
                maybe_optional(mortgage.AssetMultiplier.SATOOverride),
                "\"" + assetMultiplier.TotalSmm + "\"",
                "\"" + assetMultiplier.RefinanceSmm + "\"",
                "\"" + assetMultiplier.TurnoverSmm + "\"",
                "\"" + assetMultiplier.CurtailmentSmm + "\"",
                "\"" + assetMultiplier.CashoutSmm + "\"",
                maybe_optional((int)ex_.origDate().year()),
                wf::mortgage::utility::strings::to_string(prodType),
                to_string(ex_.primaryRate()),
                to_string(ex_.subModelType()),
                maybe_optional(ex_.sato()),
                maybe_optional(histCurtailSmm),
                maybe_optional(ex_.rfSpeed().at(ServicerSpeedType::Fast)),
                maybe_optional(ex_.rfSpeed().at(ServicerSpeedType::Normal)),
                maybe_optional(ex_.rfSpeed().at(ServicerSpeedType::Slow)),
                maybe_optional(ex_.htSpeed().at(ServicerSpeedType::Fast)),
                maybe_optional(ex_.htSpeed().at(ServicerSpeedType::Normal)),
                maybe_optional(ex_.htSpeed().at(ServicerSpeedType::Slow)),
                maybe_optional(alsAdjFactor)
            );
        }
    }

    pmr::vector<double> tempCltv(ex_.ltvSeries().begin() + ex_.wala(), ex_.ltvSeries().end());
    std::transform(tempCltv.begin(), tempCltv.end(), tempCltv.begin(),
        [](double i) {return i / 100.; }
    );
    pmr::vector<double> tempHpa1Y(ex_.projHpa1ySeries_);
    pmr::vector<double> tempHpa2Y(ex_.projHpa2ySeries_);
    pmr::vector<double> tempLoanSize(ex_.loanSizeSeries_);

    // {FIXME} when calcExCore is provided do not re-calculate cltv!
    if (!ex_.calcExCore_.valid()) {
        ex_.calcExCore_ =
            MortgageCalculationExCoreBuilder()
            .withCltv(std::move(tempCltv))
            .withHpa1y(std::move(tempHpa1Y))
            .withHpa2y(std::move(tempHpa2Y))
            .withLoanSize(std::move(tempLoanSize))
            .withWaoCreditScore(ex_.fico())
            .withWaoLoanSize(ex_.oals())
            .withWaoLoanToValue(ex_.oltv())
            .build();
    }

    if constexpr (!Traits::is_gpu) {
        //--------------------------elbow shift calculation started-------------------------------------------
        ex_.elbowRateIndependent_ = detail::elbowRateIndependent(
            *prepay,
            ex_,
            session_->diagnosticSettings(),
            session_->multiplier()
        );
        checkSeriesSize(ex_.elbowRateIndependent_, ex_.amortLength(), "Wam Series");
        //--------------------------elbow shift calculation completed-------------------------------------------

        //--------------------------pre-calc model components started-------------------------------------------
        ex_.burnoutCltvMult_.reserve(ex_.amortLength());
        for (size_t i = 0; i < ex_.amortLength(); ++i) {
            ex_.burnoutCltvMult_.emplace_back(prepay->Refinance_.Cltv(ex_.ltvSeries()[i]));
        }
        checkSeriesSize(ex_.burnoutCltvMult_, ex_.amortLength(), "Burnout Cltv Mult");
        //--------------------------pre-calc model components completed-------------------------------------------
        ex_.rfRateIndependent_ = detail::refinanceRateIndependent(
            prepay->Refinance_,
            ex_,
            session_->diagnosticSettings(),
            session_->multiplier()
        );
        checkSeriesSize(ex_.rfRateIndependent_, ex_.projectionLength(), "Refinance Rate Independent");
        ex_.htRateIndependent_ = detail::turnoverRateIndependent(
            prepay->Turnover_,
            ex_,
            session_->diagnosticSettings(),
            session_->multiplier()
        );
        checkSeriesSize(ex_.htRateIndependent_, ex_.projectionLength(), "Turnover Rate Independent");
        ex_.coRateIndependent_ = detail::cashoutRateIndependent(
            prepay->Cashout_,
            ex_,
            session_->diagnosticSettings(),
            session_->multiplier()
        );
        checkSeriesSize(ex_.coRateIndependent_, ex_.projectionLength(), "Cashout Rate Independent");
        ex_.ctRateIndependent_ = detail::curtailmentRateIndependent(
            prepay->Curtailment_,
            ex_,
            session_->diagnosticSettings(),
            session_->multiplier()
        );
        checkSeriesSize(ex_.ctRateIndependent_, ex_.projectionLength(), "Curtailment Rate Independent");
    }
}

}  // namespace wfmcm

#endif  // WFMCM_CFPM_INSTRUMENT_SESSION_H
