#pragma once

#include <cstddef>

namespace app::vasara {

struct ModelConfigData;

/**
 * Hash the immutable byte payload of a model configuration into an identity
 * value, suitable for use as the model-config field of a base-context cache key.
 * Identical bytes yield the same value.
 *
 * @note This is for in-process caching only. It uses std::hash over the bytes,
 * which is implementation-defined and NOT guaranteed to be stable across runs,
 * toolchains, or platforms. Do not persist or compare these values across
 * processes.
 */
std::size_t hashModelConfig(const ModelConfigData& config) noexcept;

} // namespace app::vasara
