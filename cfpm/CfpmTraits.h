/**
 * @file CfpmTraits.h
 * @author Wells Fargo MMDC
 * @brief Execution tags and per-tag type traits shared by the CPU and GPU CFPM
 *        implementations.
 * @copyright 2025 Wells Fargo MMDC
 *
 * The CPU and GPU paths of the CFPM model are the same algorithm run through a
 * different execution backend. This header introduces a single compile-time
 * `Tag` that selects the backend, and a `model_traits<Tag>` mapping that lets
 * the templated model/builder code stay backend-agnostic.
 */

#ifndef WFMCM_CFPM_TRAITS_H
#define WFMCM_CFPM_TRAITS_H

#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <vector>

namespace wfmcm {
namespace detail {

/**
 * Minimal time-series view used by the GPU backend for the business-days
 * adjustment ratio. Mirrors the accessors the CUDA boundary needs while
 * keeping a plain, CUDA-free layout.
 */
struct CudaTimeSeries {
    std::chrono::year_month start_;
    std::chrono::year_month end_;
    std::size_t spacing_{1};
    std::pmr::vector<double> values_;

    std::chrono::year_month startDate() const { return start_; }
    std::chrono::year_month endDate() const { return end_; }
    std::size_t spacing() const { return spacing_; }
    const std::pmr::vector<double>& data() const { return values_; }
};

}  // namespace detail

namespace cfpm {

//----------------------------------------------------------------------------
// Execution tags
//----------------------------------------------------------------------------

/// CPU backend tag (sequential std::function-based calculators).
struct cpu_tag {
    static constexpr bool on_gpu = false;
};

/// GPU backend tag (packed CUDA calculators).
struct gpu_tag {
    static constexpr bool on_gpu = true;
};

inline constexpr cpu_tag cpu{};
inline constexpr gpu_tag gpu{};

template <class Tag>
inline constexpr bool is_gpu_v = Tag::on_gpu;

//----------------------------------------------------------------------------
// GPU-only model state (empty-base optimization)
//----------------------------------------------------------------------------

/**
 * Holds the per-model state that only the GPU backend needs (sizes of the
 * packed CUDA input buffers, computed while building the instrument session).
 *
 * Specialized to be empty for the CPU tag so `[[no_unique_address]]` makes the
 * CPU model pay nothing for the GPU members.
 */
template <class Tag>
struct GpuOnlyStorage {
    // CPU: no extra state.
};

template <>
struct GpuOnlyStorage<gpu_tag> {
    mutable std::size_t maxAmortLength_{0};
    mutable std::size_t maxProjLength_{0};
    mutable std::size_t totalAmortLength_{0};
    mutable std::size_t totalProjLength_{0};
    mutable std::size_t multTotalSmmLen_{0};
    mutable std::size_t multRefiSmmLen_{0};
    mutable std::size_t multHtSmmLen_{0};
    mutable std::size_t multCoSmmLen_{0};
    mutable std::size_t multCtSmmLen_{0};
};

//----------------------------------------------------------------------------
// Model traits (specialized where the concrete backend types are known)
//----------------------------------------------------------------------------

/**
 * Primary template intentionally left undefined. Each backend provides a
 * specialization exposing:
 *
 *   - calculator_type   the per-submodel calculator (CPU) / packed (GPU)
 *   - calc_storage      how a model stores its calculators (map vs. single)
 *   - session_type      the session struct used by the backend
 *   - instrument_ex     the instrument struct used by the backend
 *   - business_days_ts  time-series type backing businessDaysAdjRatio_
 */
template <class Tag>
struct model_traits;

}  // namespace cfpm
}  // namespace wfmcm

#endif  // WFMCM_CFPM_TRAITS_H
