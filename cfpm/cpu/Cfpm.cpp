/**
 * @file Cfpm.cpp
 * @author Wells Fargo MMDC
 * @brief CPU backend implementation: CFPM projection.
 * @copyright 2024 Wells Fargo MMDC
 *
 * The model struct, builder, session/instrument construction and validation
 * are shared with the GPU backend in `CfpmModelBase.h`. This translation unit
 * contains only the CPU-specific `project()` path.
 */

#include <src/core/behavioral/cfpm/cpu/Cfpm.h>

#include <algorithm>
#include <cassert>
#include <concepts>
#include <iostream>
#include <iterator>
#include <fstream>
#include <functional>
#include <chrono>
#include <tuple>
#include <type_traits>
#include <utility>

#include <ql/time/calendars/nullcalendar.hpp>

#include <mortgage/utility/containers/index_map.h>
#include <mortgage/utility/containers/lookup/lookup_matrix_view.h>
#include <mortgage/utility/strings/from_string.h>
#include <mortgage/utility/types/optional.h>
#include <mortgage/utility/views/matrix_view_impl.h>

#include <src/app-common/InstrumentLogger.h>
#include <src/core/behavioral/cfpm/CfpmInstrumentRatePathSession.h>
#include <src/core/behavioral/cfpm/cpu/CfpmInstrumentSession.h>
#include <src/core/behavioral/cfpm/cpu/CfpmRatePathSession.h>
#include <src/core/behavioral/cfpm/cpu/CfpmSession.h>
#include <src/core/behavioral/cfpm/cpu/CfpmCalculator.h>
#include <src/core/behavioral/cfpm/detail/CfpmDetail.h>
#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/core/behavioral/detail/PathManagerModifier.h>
#include <src/core/ContextComponentNameManager.h>
#include <src/core/MortgageCashflows.h>
#include <src/core/numerics/FunctionBuilder.h>
#include <src/core/numerics/interpolator.h>
#include <src/core/states.h>
#include <src/core/utilities/date_util.h>
#include <src/io/CfpmReader.h>
#include <src/io/CohortDefaultValuesReader.h>

#include <src/io/PolyvarReader.h>
#include <src/io/ServicerSpeedReader.h>
#include <src/io/TimeSeriesReader.h>
#include <wfmcm/inst_session_mortgage_iterator.h>
#include <wfmcm/mortgage_enums.h>

namespace ql = QuantLib;
using namespace std;

namespace wfmcm {

namespace {

/**
 * Create rate path manager with joined historical and projected rates.
 *
 * The inserted views row-extend the historical rates which may be truncated
 * with the projected rate paths. I.e. if the historical rates are shape
 * (1, n_hist_orig) and the projected rates are shape (n_scen, n_proj), for
 * n_hist <= n_hist_orig, the views have shape (n_scen, n_hist + n_proj).
 *
 * One precondition is that all historical paths must have the same start date
 * and path lengths for logical consistency.
 *
 * @param n_hist Number of historical rates to use before using projected rates
 * @param raw_hist Historical lookup for raw primary rates (PMMS 15, 30)
 * @param derived_hist Historical lookup for derived rates (conv fixed 10, 20)
 * @param raw_proj PMMS 15, 30 projected rates
 * @param derived_proj conv fixed 10, 20 projected rates
 */
auto make_path_views(
    std::size_t n_hist,
    const HistRateTsLookup<HistPrimaryRateKey>& raw_hist,
    const HistRateTsLookup<HistPrimaryRateKey>& derived_hist,
    const IRatePathManager& raw_proj,
    const CfpmRatePathSession::ScenMap& derived_proj)
{
    RatePathManager rpm;
    // raw projected rates manager intf
    const auto& raw_proj_mgr = raw_proj.imanager<PrimaryRateType>();
    // for the PMMS 15, 30 primary rates
    for (auto rate : detail::cfpm_raw_rate_types()) {
        // historical view, possibly truncated
        auto hist = wfmutil::
            make_matrix_view(raw_hist, wfmutil::col_select(n_hist), rate);
        // insert (n_scen, n_hist + n_proj) view from (1, n_hist) and
        // (n_scen, n_proj) views into rate path manager
        rpm.insert(rate, wfmutil::row_extend_views(hist, raw_proj_mgr[rate]));
    }
    // for the derived conv fixed 10, 20 derived rates
    for (auto rate : detail::cfpm_derived_rate_types()) {
        // historical view, possibly truncated, and projected paths view
        auto hist = wfmutil::
            make_matrix_view(derived_hist, wfmutil::col_select(n_hist), rate);
        auto proj = wfmutil::take_view(derived_proj.at(rate));
        // insert (n_scen, n_hist + n_proj) view from (1, n_hist) and
        // (n_scen, n_proj) views into rate path manager
        rpm.insert(rate, wfmutil::row_extend_views(hist, proj));
    }
    // done
    return rpm;
}

// project for a single instrument. no logging is done here
// note: might move this to CfpmDetail.cpp
auto project(
    std::size_t n_paths,
    const CfpmSession& session,
    const CfpmRatePathSession& rps,
    const Cfpm::CalcMap& calc_map,
    const CfpmInstrumentEx& inst,
    const IRatePathManager& rates,
    const IRatePathManager& turbo_rates,
    const wfmutil::time_series<double>& adj_days)
{
    // get prepay info for model calculator for this instrument
    const auto& prepay = calc_map.at(inst.subModelType()).Prepay_;
    // diagnostics options and multiplier from the session
    const auto& diag_settings = session.diagnosticSettings();
    const auto& multiplier = session.multiplier();
    // projection output for this instrument
    std::pmr::vector<MortgageBehavioralModelOutput> res(n_paths);
    // index of first projected date for the instrument relative to first
    // historical date that will be available in the rates views
    auto proj_index = detail::calculateUnsignedMonthDifference(inst.projDate(0), rps.firstHistDate());
    // compute per-path outputs
    // {TODO} can we parallelize the computation?
    const auto& instId = inst.instrumentId();
    TROUBLESHOOT_INIT
    for (decltype(n_paths) j = 0; j < n_paths; j++) {
        TROUBLESHOOT_INFO(std::format("Projecting instrument {} using the CFPM model at path number {}", instId, std::to_string(j + 1)))
        // {TODO} document what all this stuff is
        auto pathDenpendent = detail::
            amortize(prepay, inst, rates, j, proj_index, diag_settings, multiplier);
        // {TODO} we can compute rfSmm, htSmm, coSmm in parallel
        auto rfSmm = detail::refinanceRateDependent(
            prepay.Refinance_,
            inst,
            pathDenpendent,
            turbo_rates,
            j,
            proj_index,
            diag_settings,
            multiplier
        );
        auto htSmm = detail::turnoverRateDependent(
            prepay.Turnover_, inst, pathDenpendent, j, diag_settings, multiplier
        );
        auto coSmm = detail::cashoutRateDependent(
            prepay, inst, pathDenpendent, j, diag_settings, multiplier
        );
        auto output = detail::forecast(
            prepay,
            inst,
            pathDenpendent,
            rfSmm,
            htSmm,
            coSmm,
            inst.ctRateIndependent(),  // ctSmm
            adj_days,
            diag_settings,
            multiplier
        );
        // move, not copy
        res[j] = std::move(output);
    }
    return res;
}

}  // namespace

matrix<MortgageBehavioralModelOutput> Cfpm::project(
    const SessionType& session,
    const InstrumentSessionType::View& instrumentSession,
    const IRatePathManager& rates,
    const context& context) const
{
    const auto& rateMgr = rates.imanager<PrimaryRateType>();
    // empty output if no rates or context
    if (rateMgr.empty() || instrumentSession.empty())
        return {};
    // assume first rates view defines counts of  paths and rates
    size_t numPaths = rateMgr.begin()->second.rows();
    size_t numRates = rateMgr.begin()->second.columns();
    // thread-local context name scenario storage
    app::storeScenarioId(context.name());

    // {FIXME} better way to get requiredIndices?
    // {FIXME} need to determine if turbo param is fixed or may change later

    // note: don't actually need the vector but signature required one. so we
    // just pass a default-constructed vector and call it a day
    auto requiredIndices = determineRequiredIndices({});
    // debug context key. must be string (for implicit conversion reasons)
    std::string key{app::retrieveModelDebugContextId("cfpm_ratepathSession_key")};
    // create rate path session shared pointer. note that if there is no debug
    // context ID (e.g. empty) then no caching is done
    auto rps = wfmutil::cached(
        // nullptr cache pointer means no caching is done
        key.size() ? app::caching::getSessionCache() : nullptr,
        {"cfpm_ratepath_session", key},
        // note: return type is deduced from the callable's return type
        [this, &session, &rates, &requiredIndices]
        {
            return CfpmRatePathSessionBuilder<CalcMap>{}
                .withHistRates(session.hist_rates_lookup())
                .withDerivedHistRates(session.derivedHistRates())
                .withProjScenarios(rates)
                .withAsOfDate(session.asOf())	// rate cut off date
                .withRequiredIndices(requiredIndices)
                .withDerivedRatesParam(calculators_, detail::cfpm_derived_rates_map())
                .withProjectionLength(session.projectionLength())
                .withTurboParam(turboParam_)
                .build();
        }
    );
    // number of time steps between first historical and first projected. this
    // gives the number of historical rates that are used. this of course
    // assumes that there is sufficient history for this to be positive
    auto n_hist = detail::calculateUnsignedMonthDifference(rps->firstProjDate(), rps->firstHistDate());
    // create combined views of the historical and projected rate paths. the
    // projected will override the historical as necessary
    auto paths = make_path_views(
        n_hist,
        session.hist_rates_lookup(),
        session.derivedHistRates(),
        rates,
        rps->derivedProjScenarios()
    );
    // create combined views of the historical and projected turbo rates. the
    // projected will override the historical as necessary
    auto turbo = detail::make_turbo_path_views(
        // number of historical steps
        n_hist,
        // shape (1, n_hist)
        session.historicalTurbo(),
        detail::cfpm_turbo_rate_types(),
        // shape (n_paths, n_proj)
        rps->projTurbo()
    );
    // session diagnostic settings for instrument logger
    auto diag = session.diagnosticSettings();
    diag.appendToFile_ = true;
    // projection output. reserve some rows (number of instruments in session)
    matrix<MortgageBehavioralModelOutput> output;
    output.reserve(instrumentSession.size());
    // compute for each instrument in session
    for (size_t i = 0; i < instrumentSession.size(); ++i) {
        // instrument ref. store ID too
        const auto& inst = instrumentSession[i];
        app::storeInstrumentId(inst.instrumentId());
        // RAII instrument logger context
        InstrumentLogger logger{{diag, "BehavioralModelDetailOutput"}};
        // attempt to produce output for a single instrument
        // note: need namespace qual since project() namespace is anonymous
        auto output_i = ::project(
            numPaths,
            session,
            *rps,
            calculators_,
            inst,
            paths,
            turbo,
            businessDaysAdjRatio_);
        output.push_back(std::move(output_i));
    }
    // done
    return output;
}

}  // namespace wfmcm

