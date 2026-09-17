#ifndef WFMCM_CFPM_DETAIL_H
#define WFMCM_CFPM_DETAIL_H

#include <map>
// note: libstdc++ requires memory_resource for pmr allocator to be complete
#include <memory_resource>
#include <set>
#include <span>
#include <vector>

// {TODO} include paths may change
#include <mortgage/utility/namespaces.h>  // namespace forwarding
#include <mortgage/utility/time/time_series.h>

#include <src/core/behavioral/cfpm/cpu/CfpmCalculator.h>
#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/core/numerics/interpolator.h>
#include <src/core/behavioral/MortgageBehavioralModelDial.h>
#include <wfmcm/classIds.h>
#include <wfmcm/mortgage_enums.h>
#include <wfmcm/MortgageBehavioralModelOutput.h>
// {FIXME} this leaks the entire wfmcm::mortgage::utility namespace
#include <wfmcm/ResidentialMortgage.h>

// {TODO} top-level namespace may change
namespace wfmcm {

// forward decls
template <class Model, class Traits> struct CfpmInstrumentExBuilder;
struct CfpmInstrumentEx;
struct CfpmInstrumentRatePathSession;
struct CfpmRatePathSession;
struct DiagnosticSettings;

namespace detail {

// {FIXME} shouldn't be static as constexpr implies inline
constexpr static const char* MULTIPLIER_FORMAT_VERSION = "3";

/**
 * A callable functor backed by interpolation vectors.
 * Stores (x, y) curve data and provides operator() for CPU lookup
 * and .first/.second accessors for GPU device transfer.
 * Replaces the previous dual std::function + pair<vector,vector> pattern.
 */
struct MultiplierCurve {
    using dvector = std::pmr::vector<double>;
    std::pair<dvector, dvector> data;   // {x, y} curve; x is absolute months since 1900/01
    double startMonth = 0.0;            // absolute month of the multiplier start date
    double defaultValue = 1.0;

    MultiplierCurve() = default;
    MultiplierCurve(std::pair<dvector, dvector> d, double sm, double dv)
        : data(std::move(d)), startMonth(sm), defaultValue(dv) {}

    // CPU callable: linear interpolation keyed by year_month.
    // Mirrors buildMultiplierFunc: dates before the start month return the
    // default value, a single-point curve is flat, otherwise linear interp.
    double operator()(std::chrono::year_month date) const {
        static const std::chrono::year_month baseYm{
            std::chrono::year{1900}, std::chrono::month{1}};
        double month = static_cast<double>((date - baseYm).count()) + 1;
        if (month < startMonth || data.first.empty()) return defaultValue;
        if (data.first.size() == 1) return data.second[0];
        return Interpolator::linear(
            std::span<const double>(data.first),
            std::span<const double>(data.second),
            month);
    }

    // GPU accessors — x and y vectors for device transfer
    const dvector& first()  const { return data.first;  }
    const dvector& second() const { return data.second; }
};

/**
 * CFPM multipliers.
 * Contains MultiplierCurve instances that serve both CPU (via operator())
 * and GPU (via .first()/.second() vector accessors) code paths.
 */
struct CfpmMultiplier : public MortgageBehavioralModelDial {
    MultiplierCurve TotalSmm;
    MultiplierCurve RefinanceSmm;
    MultiplierCurve TurnoverSmm;
    MultiplierCurve CashoutSmm;
    MultiplierCurve CurtailmentSmm;
    MultiplierCurve RefinanceRamping;
    MultiplierCurve TurnoverRamping;
    MultiplierCurve CashoutRamping;
    MultiplierCurve ExtraElbowShift;
    MultiplierCurve BurnoutCumulativeSpeed;
    MultiplierCurve RefinanceMediaEffect;
    MultiplierCurve TurnoverLockinEffect;
    MultiplierCurve TurnoverBurnoutCumulativeSpeed;
};

/**
 * Vector of all the CFPM primary rates used.
 *
 * This includes the raw and the derived primary rates.
 */
inline const auto& cfpm_rate_types()
{
    // we could use std::array but since model code is often run in the context
    // of a global pooled memory resource we use pmr vector again
    // note: CTAD doesn't work as this is an alias template
    static std::pmr::vector<PrimaryRateType> rates{
        // non-derived
        PrimaryRateType::fhcr15_pmms,   // PMMS 15
        PrimaryRateType::fhmrate_pmms,  // PMMS 30
        // derived from PMMS 15, PMMS 30
        PrimaryRateType::conv_fixed10_pmms,
        PrimaryRateType::conv_fixed20_pmms
    };
    return rates;
}

/**
 * Return the vector of supported CFPM turbo rate types.
 *
 * Currently this is exactly the same as `cfpm_rate_types()`.
 */
inline const auto& cfpm_turbo_rate_types()
{
    return cfpm_rate_types();
}

/**
 * Return a view of the non-derived primary rates used by CFPM.
 */
inline auto cfpm_raw_rate_types()
{
    const auto& types = cfpm_rate_types();
    return std::span{types.begin(), types.begin() + 2};  // PMMS 15, 30
}

/**
 * Return a view of the derived primary rates used by CFPM.
 */
inline auto cfpm_derived_rate_types()
{
    const auto& types = cfpm_rate_types();
    return std::span{types.begin() + 2, types.end()};  // conv fixed 10, 20
}

/**
 * Mapping of the CFPM model subtype to primary derived rate types.
 *
 * This used to be a static map but C++11 thread-safe static init is better.
 */
inline const auto& cfpm_derived_rates_map()
{
    static std::map<MortgageBehavioralSubModelType, PrimaryRateType> map{
        {
            MortgageBehavioralSubModelType::Cfpm_F10,
            cfpm_derived_rate_types()[0]  // conv fixed 10
        },
        {
            MortgageBehavioralSubModelType::Cfpm_F20,
            cfpm_derived_rate_types()[1]  // conv fixed 20
        }
    };
    return map;
};

/**
 * Return the set of supported CFPM sub-model types.
 *
 * @todo We can combine into a map with `cfpm_rate_types()` for mapping.
 */
inline const auto& CfpmSupportedSubModelTypes()
{
    static std::set<MortgageBehavioralSubModelType> subModelTypes{
        MortgageBehavioralSubModelType::Cfpm_F10,
        MortgageBehavioralSubModelType::Cfpm_F15,
        MortgageBehavioralSubModelType::Cfpm_F20,
        MortgageBehavioralSubModelType::Cfpm_F30
    };
    return subModelTypes;
}

// {TODO} document harder

// output range: [0, wala + projLen - 1]
std::pmr::vector<double> elbowRateIndependent(
    const CfpmCalculator::Prepay& prepay,
    const CfpmInstrumentEx& ex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult);

// output range: [wala, wala + projLen - 1]
std::pmr::vector<double> refinanceRateIndependent(
    const CfpmCalculator::Prepay::Refinance& rf,
    const CfpmInstrumentEx& ex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult);

// output range: [wala, wala + projLen - 1]
std::pmr::vector<double> turnoverRateIndependent(
    const CfpmCalculator::Prepay::Turnover& ht,
    const CfpmInstrumentEx& ex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult);

// output range: [wala, wala + projLen - 1]
std::pmr::vector<double> cashoutRateIndependent(
    const CfpmCalculator::Prepay::Cashout& co,
    const CfpmInstrumentEx& ex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult);

// output range: [wala, wala + projLen - 1]
std::pmr::vector<double> curtailmentRateIndependent(
    const CfpmCalculator::Prepay::Curtailment& ct,
    const CfpmInstrumentEx& ex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult);

// {TODO} document harder
CfpmInstrumentRatePathSession amortize(
    const CfpmCalculator::Prepay& prepay,
    const CfpmInstrumentEx& ex,
    const IRatePathManager& rates,
    size_t pathIndex,   // index of projection path
    size_t proj_index,  // index of first projected rate in projection path (>1)
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult);

/**
 * Compute rate-dependent refinance rates.
 *
 * Output range covers dates [wala, wala + projLen - 1] inclusive.
 *
 * @note The projection path dates must all have the same unit time spacing.
 *
 * @todo Not sure if this is what the function does; get confirmation.
 *
 * @param rf CFPM prepayment refinancing struct
 * @param ex CFPM instrument
 * @param pathDependent CFPM rate path session
 * @param turbo Rate path manager with CFPM turbo rates
 * @param pathIndex Index of projection path
 * @param proj_index Index of first projected rate in projection path (>2)
 * @param diagSettings Diagnostic settings
 * @param mult CFPM multipliers
 */
std::pmr::vector<double> refinanceRateDependent(
    const CfpmCalculator::Prepay::Refinance& rf,
    const CfpmInstrumentEx& ex,
    const CfpmInstrumentRatePathSession& pathDependent,
    const IRatePathManager& turbo,
    size_t pathIndex,
    size_t proj_index,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult);

// output range: [wala, wala + projLen - 1]
std::pmr::vector<double> turnoverRateDependent(
    const CfpmCalculator::Prepay::Turnover& ht,
    const CfpmInstrumentEx& ex,
    const CfpmInstrumentRatePathSession& pathDependent,
    size_t pathIndex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult);

// output range: [wala, wala + projLen - 1]
std::pmr::vector<double> cashoutRateDependent(
    const CfpmCalculator::Prepay& prepay,
    const CfpmInstrumentEx& ex,
    const CfpmInstrumentRatePathSession& pathDependent,
    size_t pathIndex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult);

MortgageBehavioralModelOutput forecast(
    const CfpmCalculator::Prepay& prepay,
    const CfpmInstrumentEx& ex,
    const CfpmInstrumentRatePathSession& pathDependent,
    // note: span implies contiguous data
    std::span<const double> rfSmm,
    std::span<const double> htSmm,
    std::span<const double> coSmm,
    std::span<const double> ctSmm,
    // note: consider using a view type here instead
    const wfmutil::time_series<double>& businessDaysAdjRatio,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult);

CfpmMultiplier& convert(polyvar&& from, CfpmMultiplier* to);

void checkMultiplier(const CfpmMultiplier& mult);

int getCfpmSubModelTerm(const ResidentialMortgage& mortgage);

//
// {FIXME}
//
// temp code. After we change the interface to pass subModelType directly, we
// do not need this function. however, it is still be used in several places
//
MortgageBehavioralSubModelType
getCfpmSubModelType(const ResidentialMortgage& mortgage);

PrimaryRateType
getCfpmPrimaryRate(const MortgageBehavioralSubModelType& subModelType);

}  // namespace detail

}  // namespace wfmcm

#endif  // WFMCM_CFPM_DETAIL_H

