#ifndef WFMCM_INTERNAL_DATA_H
#define WFMCM_INTERNAL_DATA_H

#include <memory>
#include <utility>

namespace app::vasara {

/**
 * Shared, immutable, C++-only data — never registered in HandleContainer.
 *
 * Use PandoInternalData<T> for any domain object that is created and consumed
 * entirely inside the C++ layer (behavioral model map, profitability
 * assumptions, calibration state, rate caches, …).  These objects have no
 * reason to be Handles: they are never returned to Java, never appear in
 * the external Handle API, and registering them in HandleContainer causes
 * custom-deleter re-entrancy / deadlock bugs.
 *
 * Properties:
 * - Zero overhead vs. raw shared_ptr<const T> — this is a thin wrapper.
 * - Copy-constructible (required for storage in HandleData's std::any).
 * - Default deleter only — no container callbacks.
 * - Immutable after construction — shared_ptr<const T>.
 *
 * Usage:
 *   // In ModelConfigData:
 *   PandoInternalData<BehavioralModelMapSpec> modelMapSpec_;
 *
 *   // Populate once during createModelConfig:
 *   config.modelMapSpec_ = PandoInternalData<BehavioralModelMapSpec>(
 *       BehavioralModelMapSpec{std::move(params)});
 *
 *   // Access at calc time:
 *   if (config.modelMapSpec_.has()) {
 *       const auto& spec = config.modelMapSpec_.get();
 *   }
 */
template <typename T>
class PandoInternalData
{
    std::shared_ptr<const T> data_;

public:
    PandoInternalData() = default;

    /** Construct from a value — typically called once at config creation. */
    explicit PandoInternalData(T value)
        : data_(std::make_shared<const T>(std::move(value))) {}

    /** Adopt an existing shared value without copying (e.g. handed back by a cache). */
    explicit PandoInternalData(std::shared_ptr<const T> data)
        : data_(std::move(data)) {}

    /** True when this data has been populated. */
    bool has()  const noexcept { return data_ != nullptr; }

    /** Access the stored data.  Caller must check has() first. */
    const T& get() const      { return *data_; }
};

} // namespace app::vasara

#endif // WFMCM_INTERNAL_DATA_H
