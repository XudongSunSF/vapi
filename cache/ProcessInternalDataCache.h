// Type your code here, or load an example.
#pragma once

#include <src/app-common/api/PandoInternalData.h>

#include <mortgage/utility/containers/cache/cache.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace app::messages {

/**
 * Process-wide, engine-internal single-flight cache with a soft size cap.
 *
 * Thin adapter over wf::mortgage::utility::containers::cache configured with the
 * single-flight build-coordination policy and a maximum-entry cap:
 *
 * - Single-flight: for a given key the creator runs on exactly one thread; other
 *   threads requesting the same key block until it completes and then share the
 *   same value. If the creator throws, nothing is cached and all joiners see the
 *   exception, so a later request can retry.
 * - Shared immutable values: values are held as shared_ptr<const Value> and handed
 *   back as PandoInternalData<Value>; a caller's handle keeps an evicted value alive.
 * - Size cap: the least-recently-used *resolved* entries are pruned once the size
 *   exceeds the cap. Pruning is asynchronous (a background thread) and batched (a
 *   fraction of entries at a time), so the size is a soft bound and may transiently
 *   exceed the cap. In-flight builds are never pruned.
 *
 * Not reentrant: a creator must not call getOrBuild() for the same key on the same
 * thread — the underlying cache detects this and throws instead of deadlocking.
 *
 * Never exposed through any public API surface.
 */
template <typename Key, typename Value, typename KeyHash = std::hash<Key>>
class ProcessInternalDataCache
{
public:
    using SharedValue = app::vasara::PandoInternalData<Value>;

    // Default soft cap: large enough that typical workloads never evict, while
    // still guaranteeing the cache cannot grow without bound.
    static constexpr std::size_t DefaultMaxEntries = (1 << 16);

    explicit ProcessInternalDataCache(std::size_t maxEntries = DefaultMaxEntries)
        : cache_(makeOptions(maxEntries))
    {}

    /**
     * Return the cached value for key, building it via creator on first use.
     * creator is any callable returning Value; it is invoked at most once per key
     * under concurrency.
     *
     * @warning Not reentrant: creator must not call getOrBuild() for the same key
     *          on the same thread.
     */
    template <class Creator>
    SharedValue getOrBuild(const Key& key, Creator&& creator)
    {
        std::function<Value()> build =
            [&creator]() -> Value { return std::forward<Creator>(creator)(); };
        std::shared_ptr<const Value> value =
            cache_.read_or_insert<Value>(toCacheKey(key), build);
        return SharedValue(std::move(value));
    }

    /** Number of resolved entries. Diagnostic only (pruning is asynchronous). */
    std::size_t size() const { return cache_.size(); }

    /**
     * Block until any in-progress pruning operations complete. Used by tests to
     * synchronize on the asynchronous eviction background thread.
     */
    void waitForPruning() const { cache_.wait_until_prune_finishes(); }

private:
    static wf::mortgage::utility::containers::cache_options makeOptions(std::size_t maxEntries)
    {
        wf::mortgage::utility::containers::cache_options opts;
        opts.maxNumElements_ = maxEntries;
        opts.buildCoordinationPolicy_ =
            wf::mortgage::utility::containers::build_coordination_policy::single_flight;
        return opts;
    }

    // The cache keys on a {stripe, entry} string pair. Encode the hashed key as the
    // entry id under a fixed tag; suitable here because these caches are keyed by
    // precomputed hash ids.
    static wf::mortgage::utility::containers::cache_key toCacheKey(const Key& key)
    {
        return { std::string("pidc"), std::to_string(KeyHash{}(key)) };
    }

    wf::mortgage::utility::containers::cache cache_;
};

} // namespace app::messages
