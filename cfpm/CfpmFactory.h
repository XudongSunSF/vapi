/**
 * @file CfpmFactory.h
 * @author Wells Fargo MMDC
 * @brief Factory for creating CFPM models with runtime CPU/GPU selection
 * @copyright 2025 Wells Fargo MMDC
 */

#pragma once

#include <map>
#include <memory>
#include <string>

#include <src/core/behavioral/IMortgageBehavioralModel.h>  // IMortgageBehavioralModel, MortgageBehavioralModelType, wfmutil::context
#include <src/core/mortgage_enums_internal.h>              // ModelParamName

namespace wfmcm {

/**
 * @brief Factory class for creating CFPM models with runtime CPU/GPU selection
 * 
 * This factory checks GPU availability at runtime and creates the appropriate
 * CFPM implementation (CPU or GPU) based on user preference and hardware availability.
 */
class CfpmFactory {
public:
    /**
     * @brief Check if GPU is available at runtime
     * @return true if CUDA-capable GPU is detected, false otherwise
     */
    static bool isGpuAvailable();

    /**
     * @brief Create a GPU-backed CFPM model when supported, else nullptr.
     *
     * Returns a GPU implementation only when CUDA support is compiled in
     * (WFMCM_CFPM_GPU_ENABLED) and a device is available; otherwise returns
     * nullptr so the caller falls back to the CPU implementation. This is the
     * single point that guards GPU availability, so callers never need their
     * own preprocessor conditionals around the GPU type.
     */
    static std::unique_ptr<IMortgageBehavioralModel> tryCreateGpuModel(
        MortgageBehavioralModelType type,
        const std::map<ModelParamName, std::string>& modelSpec,
        const wfmutil::context& ctx);

private:
    static bool gpuChecked_;
    static bool gpuAvailable_;
};

} // namespace wfmcm
