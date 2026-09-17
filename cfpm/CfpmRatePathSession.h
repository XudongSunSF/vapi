/**
 * @file CfpmRatePathSession.h
 * @author Wells Fargo MMDC
 * @brief Unified CFPM rate path session struct and builder.
 * @copyright 2024 Wells Fargo MMDC
 *
 * The CPU and GPU backends previously each owned a copy of this type
 * (`CfpmRatePathSession`/`CfpmRatePathSessionGpu`). The data is identical;
 * the only backend difference is how the calculator that derives fixed-tenor
 * rates is stored. The builder is therefore templated on that storage type and
 * routes through `cfpm::calc_contains` / `cfpm::calc_derived_rate`.
 */

#ifndef WFMCM_CFPM_RATEPATH_SESSION_H
#define WFMCM_CFPM_RATEPATH_SESSION_H

#include <chrono>
#include <cstddef>
#include <map>

#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/core/behavioral/cfpm/CfpmCalculatorPolicy.h>
#include <wfmcm/mortgage_enums.h>
#include <wfmcm/classIds.h>

// note: include path may change later in the future
#include <mortgage/utility/containers/lookup/lookup.h>
#include <mortgage/utility/time/time_series_key.h>

// Implementation dependencies (previously hidden in the .cpp files).
#include <mortgage/utility/containers/index_map.h>
#include <mortgage/utility/containers/multi_join.h>
#include <mortgage/utility/strings/to_string.h>
#include <mortgage/utility/views/matrix_view_iterator.h>
#include <src/core/behavioral/cfpm/detail/CfpmDetail.h>
#include <wfmcm/numerics/make_matrix.h>

namespace wfmcm {

//----------------------------------------------------------------------------
// Rate path session data (shared by CPU and GPU)
//----------------------------------------------------------------------------

// Instrument independent, Rate dependent, Model dependent.
struct CfpmRatePathSession {
    // mapping type to hold projected rates
    // note: namespace may change later
    using ScenMap = std::map<PrimaryRateType, wfmutil::matrix<double>>;

    auto firstHistDate() const noexcept { return firstHistDate_; }
    auto lastHistDate() const noexcept { return lastHistDate_; }
    auto firstProjDate() const noexcept { return firstProjDate_; }
    auto lastProjDate() const noexcept { return lastProjDate_; }
    auto numScenario() const noexcept { return numScenario_; }

    const auto& projTurbo() const noexcept { return projTurbo_; }
    const auto& derivedProjScenarios() const noexcept { return derivedProjScenarios_; }

private:
    CfpmRatePathSession() = default;

    std::chrono::year_month firstHistDate_;
    std::chrono::year_month lastHistDate_;
    std::chrono::year_month firstProjDate_;
    std::chrono::year_month lastProjDate_;
    std::size_t numScenario_;

    // (set by builder) calculated turbo rates for 10, 15, 20, 30 FHM rates
    ScenMap projTurbo_;
    // (set by builder) calculated pmms primary 20/10 rates
    ScenMap derivedProjScenarios_;

    template <class CalcStorage>
    friend struct CfpmRatePathSessionBuilder;
};

namespace detail {

/**
 * Check that the raw 15 and 30 year proj scenarios have the same shape.
 *
 * @param pmms15 PMMS 15 projected scenarios matrix view
 * @param pmms30 PMMS 30 projected scenarios matrix view
 * @param n_scen Number of scenarios to do projection for
 */
inline void check_projection_sizes(
    const RatePathsView& pmms15, const RatePathsView& pmms30, std::size_t /*n_scen*/)
{
    // number of rows (scenarios/paths) must be equal
    // note: this is the original exception message
    if (pmms15.rows() != pmms30.rows())
        THROW(
            "pmms fixed 30 year projected scenario numbers: " +
            std::to_string(pmms30.rows()) +
            " cannot match pmms fixed 15 year projected scenario numbers: " +
            std::to_string(pmms15.rows())
        );
    // check that number of columns (length of each path) is equal
    if (pmms15.columns() != pmms30.columns())
        THROW(
            "pmms fixed 30 year projected path length " +
            std::to_string(pmms30.columns()) +
            " does not equal pmms fixed 15 year projected path length " +
            std::to_string(pmms15.columns())
        );
}

}  // namespace detail

//----------------------------------------------------------------------------
// Rate path session builder (templated on calculator storage)
//----------------------------------------------------------------------------

/**
 * CFPM rate path session builder.
 *
 * @tparam CalcStorage CPU: `std::map<MortgageBehavioralSubModelType,
 *                      detail::CfpmCalculator>`, GPU: `gpu::CfpmCalculator`.
 *
 * @todo Need to determine if turbo-related pieces should be separated.
 */
template <class CalcStorage>
struct CfpmRatePathSessionBuilder {
    // convenience type aliases for self, historical rates
    using SelfType = CfpmRatePathSessionBuilder<CalcStorage>;
    using HistRateLookup = HistRateTsLookup<HistPrimaryRateKey>;
    // convenience type alias for derived-rate maps
    using RateMap = std::map<MortgageBehavioralSubModelType, PrimaryRateType>;

    CfpmRatePathSessionBuilder() = default;

    // {TODO} document more
    // note: all noexcept as they only set pointers

    SelfType& withHistRates(const HistRateLookup& rates) noexcept {
        histRates_ = &rates;
        return *this;
    }

    SelfType& withDerivedHistRates(const HistRateLookup& rates) noexcept {
        derivedHistRates_ = &rates;
        return *this;
    }

    SelfType& withProjScenarios(const IRatePathManager& projScenarios) noexcept {
        projScenarios_ = &projScenarios;
        return *this;
    }

    SelfType& withAsOfDate(const std::chrono::year_month& asOfDate) noexcept {
        asOfDate_ = &asOfDate;
        return *this;
    }

    SelfType& withRequiredIndices(const IndexSet& requiredIndices) noexcept {
        requiredIndices_ = &requiredIndices;
        return *this;
    }

    SelfType& withDerivedRatesParam(
        const CalcStorage& modelCalculator,
        const RateMap& derived_model_rates) noexcept {
        modelCalculator_ = &modelCalculator;
        cfpmDerivedModelRates_ = &derived_model_rates;
        return *this;
    }

    CfpmRatePathSession build() {
        doBuild();
        return std::move(rates_);
    }

    // turbo items (also noexcept as they set pointers):
    SelfType& withProjectionLength(std::size_t projLength) noexcept {
        projLength_ = projLength;
        return *this;
    }

    SelfType& withTurboParam(const detail::TurboParam& turboParam) noexcept {
        turboParam_ = &turboParam;
        return *this;
    }

private:
    /**
     * Perform self validity check.
     *
     * This function checks that required state members have been set. If there
     * is any missing state, it will simply throw an exception.
     */
    void pre_build_check() const {
        if (!histRates_)
            THROW("CfpmRatePathSessionBuilder: historical rates not set");
        if (!derivedHistRates_)
            THROW("CfpmRatePathSessionBuilder: derived historical rates not set");
        if (!projScenarios_)
            THROW("CfpmRatePathSessionBuilder: projected scenarios not set");
        if (!asOfDate_)
            THROW("CfpmRatePathSessionBuilder: as of date not set");
        if (!requiredIndices_)
            THROW("CfpmRatePathSessionBuilder: required indices not set");
        if (!modelCalculator_)
            THROW("CfpmRatePathSessionBuilder: model param not set");
        if (!cfpmDerivedModelRates_)
            THROW("CfpmRatePathSessionBuilder: derived rates map not set");
    }

    /**
     * Populate the rate path session dates and scenario lengths/counts.
     *
     * This will populate first + last historical dates, first projected date
     * (using the as-of date).
     */
    void set_session_dates() {
        // set up begin and end dates
        const auto& dateKey = histRates_->key<0>();

        std::chrono::year_month asOfDate = *asOfDate_;

        // asOfDate is used to cut off historical primary rates and projected
        // primary rates: date <= asOfDate - 1 uses historical primary rates,
        // date >= asOfDate uses projected primary rates.
        rates_.firstHistDate_ = dateKey.startDate();
        rates_.lastHistDate_ = asOfDate - std::chrono::months(1);
        if (dateKey.pos(rates_.lastHistDate_) == lookup_constant::MISSING_KEY)
            THROW(
                "The historical primary rate is not long enough to cover one month "
                "before valuation date: " + std::to_string(rates_.lastHistDate_) +
                " , please extend the file."
            );
        rates_.firstProjDate_ = asOfDate;
        std::size_t projScenarioLength = 0;

        for (auto type : requiredIndices_->view_of<PrimaryRateType>()) {
            const auto& projRates = projScenarios_->get_if(type);
            if (!projRates)
                THROW(
                    "Required primary rate: " + to_string(type)
                    + " is missing. Please verify the projected rates are good."
                );

            // {FIXME} cannot use projScenarios_->begin()
            // {FIXME} duplicated logic? this is done for each required primary rate
            projScenarioLength = projRates[0].size() - 1;
            rates_.lastProjDate_ = asOfDate + std::chrono::months(projScenarioLength);
            rates_.numScenario_ = projRates.size();
        }
    }

    /**
     * Compute the session 10 and 20 year FHM derived rate projected scenarios.
     *
     * Computed from the PMMS fixed 15 and 30 projected scenario matrix views.
     */
    void project_derived_rates() {
        // calculate derived rates; use existing pmms 30 and pmms 15 to create
        // pmms 10 and 20
        auto proj30 = projScenarios_->get_if(PrimaryRateType::fhmrate_pmms);
        auto proj15 = projScenarios_->get_if(PrimaryRateType::fhcr15_pmms);
        // number of scenarios (rate paths)
        auto n_scen = rates_.numScenario_;
        // ensure that PMMS 15 and 30 projected scenarios rates have the same dims
        detail::check_projection_sizes(proj30, proj15, n_scen);
        // for each model type (derived rate, which is fixed 10 or 20)
        for (const auto& [model, derivedRate] : *cfpmDerivedModelRates_) {
            // find model calculator in model calculator storage
            if (!cfpm::calc_contains(*modelCalculator_, model))
                THROW("Model: " + to_string(model) + " is not in the parameter map.");
            // functor to compute derived rates from PMMS 15 and PMMS 30 rates
            auto get_derived = [this, &proj15, &proj30, model](auto i, auto j) {
                return cfpm::calc_derived_rate(
                    *modelCalculator_, model, proj15[i][j], proj30[i][j]);
            };
            // compute derived rates matrix. note that the number of columns can
            // also be taken from PMMS 15 (same size, already checked)
            auto rates = make_matrix(n_scen, proj30.columns(), get_derived);
            rates_.derivedProjScenarios_.insert({derivedRate, std::move(rates)});
        }
    }

    /**
     * Compute projected turbo rates using historical and projected rates.
     *
     * The raw and derived historical and projected rates are used for each
     * rate type depending on if the rate is derived (10, 20) or not (15, 30).
     */
    void project_turbo_rates() {
        // turbo rate scenario matrix computation for each of the turbo rates
        for (auto turboRate : detail::cfpm_turbo_rate_types()) {
            // select historical rates. FHM 10, 20 are derived, hence conditional
            auto [histRateBegin, histRateEnd] = [this, turboRate] {
                // raw historical
                auto raw_begin = histRates_->begin_with_key(_, turboRate);
                auto raw_end = histRates_->end_with_key(_, turboRate);
                // derived historical
                auto der_begin = derivedHistRates_->begin_with_key(_, turboRate);
                auto der_end = derivedHistRates_->end_with_key(_, turboRate);
                // select
                return (raw_begin != raw_end) ?
                    std::make_pair(raw_begin, raw_end) :
                    std::make_pair(der_begin, der_end);
            }();
            // check whether rate range is non-empty
            if (histRateBegin == histRateEnd)
                THROW(to_string(turboRate) + " historical raw/derived rates range is empty");
            // input projected rates matrix view to use. FHM 10, 20 are derived
            auto proj_view = [this, turboRate] {
                // get possibly-empty view of the non-derived projected scenario rates
                auto rates_view = projScenarios_->get_if(turboRate);
                if (rates_view)
                    return rates_view;
                // iterator to derived projected scenarios map element for the rate
                auto derived_it = rates_.derivedProjScenarios_.find(turboRate);
                if (derived_it != rates_.derivedProjScenarios_.end())
                    return take_view(derived_it->second);
                // error
                THROW(
                    to_string(turboRate) +
                    " is not available, projected turbo cannot be calculated"
                );
            }();
            // number of historical and total rates
            auto histLength = detail::calculateUnsignedMonthDifference(
                rates_.firstProjDate_, rates_.firstHistDate_);
            auto rateLength = detail::calculateUnsignedMonthDifference(
                rates_.lastProjDate_, rates_.firstHistDate_) + 1;
            // iterator for when projected rates begin
            auto histRateAsOf = histRateBegin + histLength;
            // {TODO} we can pre-allocate the entire matrix and then fill directly
            // dimensions are (n_scen, [projected data lengths])
            auto n_scen = rates_.numScenario_;
            matrix<double> turboScenario;
            turboScenario.reserve(n_scen);
            // perform turbo rate computation for each scenario
            for (decltype(n_scen) i = 0; i < n_scen; ++i) {
                // compute turbo rates using scenario row of the matrix view
                auto proj_begin = matrix_view_iterator::begin_of_row(proj_view, i);
                auto proj_end = matrix_view_iterator::end_of_row(proj_view, i);
                // join historical and projected iterators into view
                auto rates = multi_join(histRateBegin, histRateAsOf, proj_begin, proj_end);
                // allocate turbo rates output (length of projected rates)
                std::pmr::vector<double> turboValues;
                turboValues.reserve(rateLength - histLength);
                // compute
                detail::calculateTurboRates(
                    rates.begin(),
                    rates.end(),
                    rates.begin() + histLength,
                    *turboParam_,
                    turboValues
                );
                // emplace calculated turbo values for this scenario
                turboScenario.emplace_back(std::move(turboValues));
            }
            // insert turbo rates scenario matrix for the rate
            rates_.projTurbo_.insert({turboRate, std::move(turboScenario)});
        }
    }

    /**
     * Perform rate path session state build.
     *
     * This sets session dates and projects the derived and turbo rates.
     *
     * @note May be unnecessary as we can just put the subroutines in `build`.
     */
    void doBuild() {
        // perform pre-build state validity check
        pre_build_check();
        // compute the historical start/end and projected start/end dates as well
        // as the number of primary rate scenarios
        set_session_dates();
        // compute session derived projected rate scenarios using PMMS 15 and 30
        project_derived_rates();
        // compute session projected turbo rate scenarios
        project_turbo_rates();
    }

    std::size_t projLength_;
    const CalcStorage* modelCalculator_{};
    const RateMap* cfpmDerivedModelRates_{};
    const detail::TurboParam* turboParam_{};
    // historical/projected rates cut off date
    const std::chrono::year_month* asOfDate_{};

    const IRatePathManager* projScenarios_{};
    const HistRateLookup* histRates_{};
    const IndexSet* requiredIndices_{};
    const HistRateLookup* derivedHistRates_{};

    // session to populate
    CfpmRatePathSession rates_;
};

}  // namespace wfmcm

#endif  // WFMCM_CFPM_RATEPATH_SESSION_H
