#ifndef WFMCM_MODEL_CONFIG_DATA_H
#define WFMCM_MODEL_CONFIG_DATA_H

#include <src/app-common/api/PandoInternalData.h>
#include <wfmcm/classIds.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace app::vasara {

/**
 * Parsed behavioral model map spec data.
 *
 * Stored as PandoInternalData<T> inside ModelConfigData — not registered in
 * HandleContainer — so that no custom deleter / container callback is needed.
 * Multiple request contexts share the same data via the shared_ptr
 * inside PandoInternalData; the default deleter handles cleanup.
 */
struct BehavioralModelMapSpec
{
    wfmcm::ModelParamSpec params_;  // map<ModelParamName, string>
};

/**
 * Tier 1 (service-level) model configuration for the Vasara integration.
 *
 * This is the cached, immutable bundle of model *inputs* backing a
 * model-config handle — NOT a behavioral-model "model session" (see
 * wfmcm::MortgageBehavioralModel::SessionType, which is a stateful per-path
 * model instance).  It carries the raw model-parameter bytes, the behavioral
 * model-map CSV, and the parsed map spec.
 *
 * Populated inside createModelConfig: byte payloads at construction, then
 * modelMapSpec_ once the behavioral map CSV has been parsed.  After
 * createModelConfig returns, the instance is frozen — no member may be
 * mutated — and may be cached per worker / evaluator and read concurrently
 * (contexts hold it via shared_ptr<const ModelConfigData>).
 *
 * Ownership: the behavioral model map spec is stored as a plain shared_ptr
 * with the default deleter.  Multiple request contexts share it via their
 * copy of ModelConfigData.  No HandleContainer registration, no custom
 * deleter callbacks.
 *
 * NOTE: this type is stored in HandleData's std::any, which requires a
 * copy-constructible type — do not make it move-only.
 */
struct ModelConfigData
{
    std::vector<char> modelParams_;
    std::vector<char> modelMapCsv_;

    /**
     * Parsed behavioral model map spec; empty when the model config has no
     * behavioral map.  Shared (not unique) because request contexts copy
     * this struct and multiple concurrent batches may reference the same spec.
     */
    PandoInternalData<BehavioralModelMapSpec> modelMapSpec_;

    /** True when the model config carries a behavioral model map. */
    bool hasModelMap() const noexcept { return modelMapSpec_.has(); }

    /** Access the parsed spec params.  Caller must check hasModelMap() first. */
    const wfmcm::ModelParamSpec& modelMapParams() const
    {
        return modelMapSpec_.get().params_;
    }

    ModelConfigData() = default;

    ModelConfigData(std::vector<char> modelParams, std::vector<char> modelMapCsv)
        : modelParams_(std::move(modelParams))
        , modelMapCsv_(std::move(modelMapCsv))
    {}
};

} // namespace app::vasara

#endif // WFMCM_MODEL_CONFIG_DATA_H
