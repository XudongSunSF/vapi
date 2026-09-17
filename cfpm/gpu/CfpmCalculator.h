#pragma once
// GPU implementation - only available if WFMCM_CFPM_GPU_ENABLED is defined at compile time
#ifdef WFMCM_CFPM_GPU_ENABLED

#include <chrono>
#include <functional>
#include <map>
#include <memory_resource>

// note: include path will change on name change
#include <mortgage/utility/types/constants.h>
#include <mortgage_cuda.h>

#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/core/states.h>
#include <wfmcm/mortgage_enums.h>

namespace wfmcm::detail {
struct CfpmParameter;
}

namespace wfmcm::gpu {

struct cpu_tag_t {};
struct gpu_tag_t {};

inline constexpr cpu_tag_t cpu_tag{};
inline constexpr gpu_tag_t gpu_tag{};

// {FIXME} document, just looks like a way to hold functors for computation
struct CfpmCalculator {

    CfpmCalculator();

    struct StateFuncs
    {
        std::pmr::vector<std::function<double(const std::map<State, double>&)>> CurtailmentState;
        std::pmr::vector<std::function<double(const std::map<State, double>&)>> TurnoverState;
        std::pmr::vector<std::function<double(const std::map<State, double>&)>> RefinanceState;
        std::pmr::vector<std::function<double(const std::map<State, double>&)>> ElbowState;
    };

    struct DerivedRateParams
    {
        double DerivedRatePmms15Coef = unset_value<double> ;
        double DerivedRatePmms30Coef = unset_value<double> ;
        double DerivedRateSpread = unset_value<double> ;
    };

    std::unique_ptr<wfmcm::cuda::cfpm::Prepay> prepay_;
    StateFuncs statefunc_;
    detail::TurboParam Turbo_;
    std::map<MortgageBehavioralSubModelType, DerivedRateParams> derivedRateParams_;
    std::map<MortgageBehavioralSubModelType, size_t> models_;

    /**
     * Compute a derived fixed tenor rate using PMMS 15, 30 rates.
     *
     * This uses the prepay structure's derived rate PMMS 15 and 30
     * coefficients and spread values to compute the derived rate. Whether the
     * output is for fixed 10 or 20 year depends on the state values.
     *
     * @param pmms15 PMMS 15 rate
     * @param pmms30 PMMS 30 rate
     */
    double derived_rate(MortgageBehavioralSubModelType mdl, double pmms15, double pmms30) const;

    void assign(std::map<MortgageBehavioralSubModelType, detail::CfpmParameter>&& param);

    bool contains(MortgageBehavioralSubModelType mdl) const { return models_.contains(mdl); }

    size_t modelMappedIndex(MortgageBehavioralSubModelType mdl) const {
        auto it = models_.find(mdl);
        if (it == models_.end()) THROW("Cannot map submodel type in CFPM");
        return it->second;
    }


};

}  // namespace wfmcm::gpu

#endif  // WFMCM_CFPM_GPU_ENABLED