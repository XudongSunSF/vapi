/**
 * @file CfpmCalculatorPolicy.h
 * @author Wells Fargo MMDC
 * @brief Free-function policy surface over the two CFPM calculator storages.
 * @copyright 2025 Wells Fargo MMDC
 *
 * The CPU backend stores one `detail::CfpmCalculator` per sub-model in a
 * `std::map`, while the GPU backend stores a single packed `gpu::CfpmCalculator`
 * that internally maps sub-models to flat device-transfer vectors. Everything
 * else in the model only ever needs three operations:
 *
 *   - does a sub-model exist?
 *   - compute its derived fixed-tenor rate
 *   - obtain the shared turbo parameters
 *
 * These overloads give those operations a single name so the shared session
 * and rate-path code can be written once against either storage.
 */

#ifndef WFMCM_CFPM_CALCULATOR_POLICY_H
#define WFMCM_CFPM_CALCULATOR_POLICY_H

#include <map>

#include <src/core/behavioral/cfpm/cpu/CfpmCalculator.h>
#ifdef WFMCM_CFPM_GPU_ENABLED
#include <src/core/behavioral/cfpm/gpu/CfpmCalculator.h>
#endif

namespace wfmcm {
namespace cfpm {

//----------------------------------------------------------------------------
// calc_contains
//----------------------------------------------------------------------------

template <class Calc>
bool calc_contains(const std::map<MortgageBehavioralSubModelType, Calc>& m,
                   MortgageBehavioralSubModelType t) {
    return m.contains(t);
}

#ifdef WFMCM_CFPM_GPU_ENABLED
inline bool calc_contains(const gpu::CfpmCalculator& c,
                          MortgageBehavioralSubModelType t) {
    return c.contains(t);
}
#endif

//----------------------------------------------------------------------------
// calc_derived_rate
//----------------------------------------------------------------------------

inline double calc_derived_rate(
    const std::map<MortgageBehavioralSubModelType, detail::CfpmCalculator>& m,
    MortgageBehavioralSubModelType t, double pmms15, double pmms30) {
    return m.at(t).derived_rate(pmms15, pmms30);
}

#ifdef WFMCM_CFPM_GPU_ENABLED
inline double calc_derived_rate(const gpu::CfpmCalculator& c,
                                MortgageBehavioralSubModelType t,
                                double pmms15, double pmms30) {
    return c.derived_rate(t, pmms15, pmms30);
}
#endif

//----------------------------------------------------------------------------
// calc_turbo_param
//----------------------------------------------------------------------------

inline const detail::TurboParam& calc_turbo_param(
    const std::map<MortgageBehavioralSubModelType, detail::CfpmCalculator>& m) {
    // All CPU sub-models share identical turbo parameters (validated in
    // CfpmModelBuilder::doBuild); any one of them is representative.
    return m.begin()->second.Prepay_.Turbo_;
}

#ifdef WFMCM_CFPM_GPU_ENABLED
inline const detail::TurboParam& calc_turbo_param(const gpu::CfpmCalculator& c) {
    return c.Turbo_;
}
#endif

}  // namespace cfpm
}  // namespace wfmcm

#endif  // WFMCM_CFPM_CALCULATOR_POLICY_H
