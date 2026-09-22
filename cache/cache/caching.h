#ifndef WF_MORTGAGE_UTILITY_CACHEING_H
#define WF_MORTGAGE_UTILITY_CACHEING_H

// TODO: remove unused headers
#include <atomic>
#include <chrono>
#include <memory>
#include <type_traits>

#include "mortgage/utility/containers/cache/cache.h"
#include "mortgage/utility/namespaces.h"

namespace wf::mortgage::utility::caching {

/**
 * @brief This object must be created at the start of main or on DLL load.
 *        It automatically calls shutdown() when it gets destroyed.
 */
class program_caching
{
public:
    //Creates the global cache via global::init()
    program_caching(const containers::cache_options& settings);

    //Terminate global cache via global::shutdown()
    ~program_caching();
};

namespace global {

//access the global read cache
containers::cache::reader* get_read_cache() noexcept;

//access the global write cache
containers::cache::writer* get_write_cache() noexcept;

//Creates the global cache
void init(const containers::cache_options& settings);

//shutdown global cache
void shutdown() noexcept;

}  // namespace global

/**
 * Read or insert helper for the cache.
 *
 * This should be the function used to facilitate use of a caching pattern as
 * it can be abstractly treated as a thin wrapper around standard object
 * creation. Direct use of the cache reader interface methods is discouraged.
 *
 * @tparam F Object creation callable type. This can be a lambda, type-erased
 *  functor, a free function, etc. used to run some object creation logic.
 *
 * @param cache Pointer to cache. If `nullptr`, no caching is used at all.
 * @param key Cache key to associate with created object
 * @param creator Object creation callable
 * @param ttl Time to live. If not provided, cache's max time to live is used.
 * @returns Shared pointer to the created object (may/may not be in the cache)
 */
template <typename F>
inline auto cached(
    containers::cache::reader* cache,
    const containers::cache_key& key,
    const F& creator,
    std::optional<containers::time_to_live> ttl = {})
{
    if (cache && (key != containers::null_cache_key))
        return cache->read_or_insert<std::invoke_result_t<F>>(key, creator, ttl);
    return std::make_shared<const std::invoke_result_t<F>>(creator());
}

/**
 * Read or insert helper for the cache.
 *
 * Convenience overload that uses the global read cache. If the global read
 * cache was not initialized, i.e. is `nullptr`, no caching is done.
 *
 * @tparam F Object creation callable type. This can be a lambda, type-erased
 *  functor, a free function, etc. used to run some object creation logic.
 *
 * @param key Cache key to associate with created object
 * @param creator Object creation callable
 * @param ttl Time to live. If not provided, cache's max time to live is used.
 * @returns Shared pointer to the created object (may/may not be in the cache)
 */
template <typename F>
inline auto cached(
    const containers::cache_key& key,
    const F& creator,
    std::optional<containers::time_to_live> ttl = {})
{
    return cached(global::get_read_cache(), key, creator, ttl);
}

}  // namespace wf::mortgage::utility::caching

#endif  // WF_MORTGAGE_UTILITY_CACHEING_H
