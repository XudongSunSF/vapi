/**
 * @file prune_policy_interface.h
 * @author Wells Fargo MMDC
 * @brief C++ header for the cache's pruny policy interface
 * @copyright 2025 Wells Fargo MMDC
 */

#ifndef WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_PRUNE_POLICY_INTERFACE_H
#define WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_PRUNE_POLICY_INTERFACE_H

#include <mortgage/utility/namespaces.h>
#include <mortgage/utility/containers/cache/cache_entry_descriptor.h>
#include <memory>
#include <array>
#include <string>
#include <chrono>
#include <algorithm>
#include <ranges>
#include <type_traits>

namespace wf::mortgage::utility::containers
{
	template <typename Key>
	struct prune_element
	{
		using key_type = Key;
		const key_type* pruneKey_{ nullptr };
		const cache_entry_descriptor* desc_{ nullptr };
	};

	/**
	 * @brief Cache pruning policy CRTP interface.
	 *
	 * The prune policy is used to determine the criteria by which cache
	 * entries are to be deleted.
	 *
	 * @tparam Key Key type used to index cache entries
	 * @tparam Policy Concrete policy type
	*/
	template <typename Key, typename Policy>
	struct prune_policy_interface
	{
		using policy_type = Policy;
		using key_type = Key;

		/**
		 * @brief Returns a list of keys to prune.
		 *
		 * This uses the default memory resource from
		 * `std::pmr::get_default_resource()`.
		 *
		 * @note The prune policy simply identifies the elements to be pruned.
		 *
		 * @param cacheView `prune_element<K>` range view into the cache
		 */
		template <std::ranges::view V>
		requires std::is_same_v<std::ranges::range_value_t<V>, prune_element<key_type>>
		std::pmr::vector<prune_element<key_type>>
		prune(const V& cacheView) const noexcept {
			return static_cast<const Policy*>(this)->do_prune(std::pmr::get_default_resource(), cacheView);
		}

		/**
		 * @brief Returns a list of keys to prune.
		 *
		 * @note The prune policy simply identifies the elements to be pruned.
		 *
		 * @param res Memory resource to use for internal allocations
		 * @param cacheView `prune_element<K>` range view into the cache
		 */
		template <std::ranges::view V>
	    requires std::is_same_v<std::ranges::range_value_t<V>, prune_element<key_type>>
		std::pmr::vector<prune_element<key_type>>
		prune(std::pmr::memory_resource* res,
			  const V& cacheView) const noexcept {
			return static_cast<const Policy*>(this)->do_prune(res, cacheView);
		}
	};

}

#endif  // WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_PRUNE_POLICY_INTERFACE_H
