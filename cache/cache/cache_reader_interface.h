#ifndef WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_READER_INTERFACE_H
#define WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_READER_INTERFACE_H

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "mortgage/utility/namespaces.h"
#include "mortgage/utility/containers/cache/cache_stats.h"

namespace wf::mortgage::utility::containers {

/**
 * @brief Cache reader interface for querying the cache.
 *
 * @note Functions marked `noexcept` may still throw due to implementation but
 *  no exceptions will be caught due to the `noexcept` specification.
 *
 * @tparam Key The key type used to index cache entries
 * @tparam Cache The concrete cache type implementing this interface
 */
template <typename Key, typename Cache>
struct cache_reader_interface
{
    using key_type = Key;
    using cache_type = Cache;

    /**
     * Type alias for the creator callable.
     *
     * @tparam T type
     */
    template <typename T>
    using creator_func = std::function<T()>;

    /**
     * @brief Read a value from the cache.
     *
     * @note This is intended to be `noexcept` conditional on `NDEBUG` being
     *  defined in order to allow insertion of debugging throws.
     *
     * @tparam T Value type
     * @param k The key
     * @return Shared pointer to value, empty if key does not map to a value
     */
    template <typename T> [[nodiscard]]
    std::shared_ptr<const T> read(const key_type& k) const WF_NDEBUG_NOEXCEPT
        // concrete examples of conditional noexcept specification:
        // noexcept(noexcept(static_cast<const Cache*>(this)->do_read<T>(k)))
        // noexcept(noexcept(std::declval<const Cache>().do_read<T>(k)))
    {
        // note: template keyword required due to template parameter dependency
        return static_cast<const Cache*>(this)->template do_read<T>(k);
    }

    /**
     * @brief Read a value from the cache, creating if the value does not exit.
     *
     * If no value exists for the given key, the user-supplied callable is used
     * to create a value, which is then inserted into the cache and retrieved as
     * a shared pointer. Shared pointer creation does not re-allocate.
     *
     * @note Throws if `f()` throws or if move ctor of `T` throws.
     *
     * @tparam T Value type
     *
     * @param k The key
     * @param f A functor object which creates the cache entry if it's empty.
     * @param ttl The time-to-live in seconds.
     * @return Non-empty shared pointer to the value
    */
    template <typename T> [[nodiscard]]
    std::shared_ptr<const T> read_or_insert(
        const key_type& k,
        const creator_func<T>& f,
        std::optional<time_to_live> ttl = {})
    {
        // note: template keyword required due to template parameter dependency
        return static_cast<Cache*>(this)->template do_read_or_insert<T>(k, f, ttl);
    }

    /**
     * Return number of items in the cache.
     */
    [[nodiscard]]
    size_t size() const noexcept
    {
        return static_cast<const Cache*>(this)->do_size();
    }

    /**
     * Indicate whether the pruning operation is running or not.
     */
    [[nodiscard]]
    bool is_prune_operation_running() const noexcept
    {
        return static_cast<const Cache*>(this)->do_is_prune_operation_running();
    }

    /**
     * Block the current thread until pruning operation finishes.
     *
     * @todo Check whether implementations are actually blocking.
     * @todo Potentially remove `noexcept` specifier.
     */
    void wait_until_prune_finishes() const noexcept
    {
        return static_cast<const Cache*>(this)->do_wait_until_prune_finishes();
    }

    /**
     * Return cache stats.
     *
     * If a key is provided, only stats belonging to the key are fetched.
     *
     * @tparam k Key to fetch stats for (empty for global stats)
     */
    [[nodiscard]]
    cache_stats stats(const key_type& k = {}) const noexcept
    {
        return static_cast<const Cache*>(this)->do_stats(k);
    }
};

}  // namespace wf::mortgage::utility::containers

#endif  // WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_READER_INTERFACE_H
