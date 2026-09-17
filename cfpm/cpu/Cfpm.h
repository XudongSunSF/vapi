/**
 * @file Cfpm.h
 * @author Well Fargo MMDC
 * @brief C++ header for the CFPM model struct (CPU backend)
 * @copyright 2024 Wells Fargo MMDC
 *
 * The model and builder implementations are shared with the GPU backend via
 * `CfpmModelBase`/`CfpmModelBuilder`; this header only defines the CPU traits
 * specialization and the CPU `project()` entry point.
 */

#ifndef WFMCM_CFPM_H_
#define WFMCM_CFPM_H_

#include <map>
#include <memory>
#include <set>
#include <vector>

#include <src/core/behavioral/cfpm/CfpmModelBase.h>
#include <src/core/behavioral/cfpm/cpu/CfpmInstrumentSession.h>
#include <src/core/behavioral/cfpm/cpu/CfpmCalculator.h>
#include <src/core/behavioral/cfpm/detail/CfpmParameter.h>

#include <src/core/MortgageCalculationExCore.h>
#include <wfmcm/ResidentialMortgage.h>

namespace wfmcm {

struct Cfpm;

namespace cfpm {

// CPU backend traits: one detail::CfpmCalculator per sub-model in a map.
template <>
struct model_traits<cpu_tag> {
    using tag = cpu_tag;
    static constexpr bool is_gpu = false;
    using calculator_type = detail::CfpmCalculator;
    using calc_storage = std::map<MortgageBehavioralSubModelType, calculator_type>;
    using business_days_ts = wfmutil::time_series<double>;
    using session_type = CfpmSession;
    using instrument_ex = CfpmInstrumentEx;
    using ex_builder = CfpmInstrumentExBuilder<Cfpm, model_traits<cpu_tag>>;
};

}  // namespace cfpm

/**
 * CFPM model struct (CPU backend).
 *
 * All shared behavior lives in `CfpmModelBase`; only the CPU `project()`
 * implementation is backend-specific.
 */
struct Cfpm : CfpmModelBase<Cfpm, cfpm::model_traits<cfpm::cpu_tag>> {
    using Base = CfpmModelBase<Cfpm, cfpm::model_traits<cfpm::cpu_tag>>;

    // Backward-compatible alias used by the CPU project implementation.
    using CalcMap = typename Base::CalcStorage;

    matrix<MortgageBehavioralModelOutput> project(
        const SessionType& session,
        const InstrumentSessionType::View& instrumentSession,
        const IRatePathManager& rates,
        const wfmutil::context& context) const;
};

// CFPM model builder type (shared implementation, CPU instantiation).
using CfpmBuilder = CfpmModelBuilder<Cfpm, cfpm::model_traits<cfpm::cpu_tag>>;

}  // namespace wfmcm

#endif  // WFMCM_CFPM_H_

