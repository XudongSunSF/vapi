/**
 * @file Cfpm.h
 * @author Well Fargo MMDC
 * @brief C++ header for the CFPM model struct (GPU backend)
 * @copyright 2025 Wells Fargo MMDC
 *
 * The model and builder implementations are shared with the CPU backend via
 * `CfpmModelBase`/`CfpmModelBuilder`; this header only defines the GPU traits
 * specialization, the GPU `project()` entry point, and the CUDA input
 * marshalling helpers.
 */

#pragma once

// GPU implementation - only available if WFMCM_CFPM_GPU_ENABLED is defined at compile time
#ifdef WFMCM_CFPM_GPU_ENABLED

#include <map>
#include <memory>
#include <vector>

#include <src/core/behavioral/cfpm/CfpmModelBase.h>
#include <src/core/behavioral/cfpm/gpu/CfpmCalculator.h>
#include <src/core/behavioral/cfpm/gpu/CfpmInstrumentSession.h>
#include <src/core/behavioral/cfpm/detail/CfpmDetail.h>

#include <src/core/MortgageCalculationExCore.h>
#include <wfmcm/ResidentialMortgage.h>

#include <mortgage_cuda.h>

namespace wfmcm {

struct CfpmGpuImpl;

namespace cfpm {

// GPU backend traits: a single packed gpu::CfpmCalculator.
template <>
struct model_traits<gpu_tag> {
    using tag = gpu_tag;
    static constexpr bool is_gpu = true;
    using calculator_type = gpu::CfpmCalculator;
    using calc_storage = calculator_type;
    using business_days_ts = detail::CudaTimeSeries;
    using session_type = CfpmSession;
    using instrument_ex = CfpmInstrumentEx;
    using ex_builder = CfpmInstrumentExBuilder<CfpmGpuImpl, model_traits<gpu_tag>>;
};

}  // namespace cfpm

/**
 * CFPM GPU implementation struct (internal).
 *
 * All shared behavior lives in `CfpmModelBase`; only the GPU `project()`
 * implementation and the CUDA input marshalling helpers are backend-specific.
 */
struct CfpmGpuImpl : CfpmModelBase<CfpmGpuImpl, cfpm::model_traits<cfpm::gpu_tag>> {
    using Base = CfpmModelBase<CfpmGpuImpl, cfpm::model_traits<cfpm::gpu_tag>>;

    using CalcResult = MortgageBehavioralModel::CalcResult;
    using CalcResults = MortgageBehavioralModel::CalcResults;

    CalcResults project(
        CalcResults&& results,
        const SessionType& session,
        const InstrumentSessionType::View& instrumentSession,
        const IRatePathManager& rates,
        const wfmutil::context& context) const;

private:
    std::unique_ptr<cuda::cfpm::InstrumentInputs> createGpuInstrumentInputs(
        const InstrumentSessionType::View& instrumentSession) const;

    cuda::cfpm::CfpmMultiplier createGpuCfpmMultiplier(const SessionType& session) const;
};

// CFPM GPU model builder type (shared implementation, GPU instantiation).
using CfpmGpuImplBuilder = CfpmModelBuilder<CfpmGpuImpl, cfpm::model_traits<cfpm::gpu_tag>>;

}  // namespace wfmcm

#endif  // WFMCM_CFPM_GPU_ENABLED