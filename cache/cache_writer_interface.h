#ifndef WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_WRITER_INTERFACE_H
#define WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_WRITER_INTERFACE_H

#include <mortgage/utility/namespaces.h>
#include <mortgage/utility/containers/cache/prune_policy_interface.h>
#include <memory>
#include <array>
#include <string>
#include <chrono>
#include <optional>
#include <functional>

namespace wf::mortgage::utility::containers {

/**
 * @brief Cache writer interface for modifying the cache
 *
 * @note Functions marked `noexcept` may still throw due to implementation but
 *  no exceptions will be caught due to the `noexcept` specification.
 *
 * @tparam Key The key type used to index cache entries
 * @tparam Cache The concrete cache type implementing this interface
 */
template <typename Key, typename Cache>
struct cache_writer_interface
{
	using key_type = Key;
	using cache_type = Cache;

	/**
	 * @brief Insert or overwrite an entry in the cache.
	 *
	 * @note Inserted entry can be removed by any time-based pruning policy.
	 *
	 * @note This is intended to be `noexcept` conditional on `NDEBUG` being
     *  defined in order to allow insertion of debugging throws.
	 *
	 * @tparam T The type of the element
	 *
	 * @param k The key
	 * @param val The value to be inserted
	 * @param ttl The time-to-live in seconds.
	 * @return A shared pointer representing the cache entry.
	*/
	template <typename T>
	std::shared_ptr<const std::decay_t<T>> write(
		const key_type& k,
		T&& val,
		std::optional<time_to_live> ttl = {}) WF_NDEBUG_NOEXCEPT
	{
		return static_cast<Cache*>(this)->do_write(k, std::forward<T>(val), ttl);
	}

	/**
	 * @brief Prune the cache.
	 *
	 * @note Any items whose lifetime is extended by holding on to the
	 * 	shared_ptrs will continue to be valid even after pruning.
	 *
	 * @param policy The pruning policy to use.
	 * @return The number of items pruned.
	 */
	template <typename Policy>
	size_t prune(const Policy& policy) noexcept
	{
		return static_cast<Cache*>(this)->do_prune(policy);
	}

	/**
	 * @brief Erase an entry in the cache.
	 *
	 * @todo Consider dropping `noexcept` specifier.
	 *
	 * @note Use a specialized regex pruning policy to prune via key regex.
	 *
	 * @param k The key to erase
	 * @return true if erased, false otherwise
	 */
	bool erase(const key_type& k) noexcept
	{
		return static_cast<Cache*>(this)->do_erase(k);
	}

	/**
	 * @brief Clear the entire cache and reset statistics.
	 */
	void clear() noexcept
	{
		static_cast<Cache*>(this)->do_clear();
		static_cast<Cache*>(this)->do_reset_stats(key_type{});
	}

	/**
	 * Reset global or key-specific statistics.
	 *
	 * Global cache statistics returned if key is default-constructed.
	 *
	 * @note Nothing should be done if the key is not in the cache.
	 *
	 * @param k Key to reset statistics for
	 */
	void reset_stats(const key_type& k = {}) noexcept
	{
		static_cast<Cache*>(this)->do_reset_stats(k);
	}
};

}  // namespace wf::mortgage::utility::containers

#endif  // WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_WRITER_INTERFACE_H
