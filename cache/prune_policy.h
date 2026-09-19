#ifndef WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_PRUNE_POLICY_H
#define WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_PRUNE_POLICY_H

#include <mortgage/utility/namespaces.h>
#include <mortgage/utility/containers/cache/prune_policy_interface.h>
#include <memory>
#include <array>
#include <string>
#include <chrono>
#include <functional>
#include <optional>
#include <algorithm>
#include <regex>
#include <ranges>

namespace wf::mortgage::utility::containers
{
	//==========================================================
	//                   KEY MATCHERS
	//==========================================================
	//string specializations
	template <size_t N>
	bool regex_full_match(
		const std::array<std::string, N>& input,
		const std::array<std::string, N>& pattern) noexcept
	{
		for (size_t i = 0; i < N; ++i) {
			if (!std::regex_match(input[i], std::regex(pattern[i]), std::regex_constants::match_any)) {
				return false;
			}
		}
		return true;
	}

	template <size_t N>
	bool regex_partial_match(
		const std::array<std::string, N>& input,
		const std::array<std::string, N>& pattern) noexcept
	{
		for (size_t i = 0; i < N; ++i) {
			if (!std::regex_search(input[i], std::regex(pattern[i]), std::regex_constants::match_any)) {
				return false;
			}
		}
		return true;
	}

	//==========================================================
	//                  PRUNE POLICIES
	//==========================================================
	/**
	 * @brief Prune policy for expired cache entries
	*/
	template <typename Key>
	struct time_to_live_prune_policy : public prune_policy_interface<Key, time_to_live_prune_policy<Key>>
	{
		using base_type = prune_policy_interface<Key, time_to_live_prune_policy<Key>>;
		using key_type = Key;

	private:
		friend base_type;

		template <std::ranges::view V> 
		std::pmr::vector<prune_element<key_type>> 
		do_prune(std::pmr::memory_resource* res, const V& cacheView) const noexcept
		{
			try {
				size_t viewSize = std::distance(cacheView.begin(), cacheView.end());
				std::pmr::vector<prune_element<key_type>> vec(res); 
				vec.reserve(std::max((size_t)100, viewSize/20));
				for (const auto& elem : cacheView) {
					if (elem.desc_->is_expired()) {
						vec.emplace_back(elem); //trivially copy-able
					}
				}
				return vec;
			}
			catch (...) {}
			return {};
		}
	};

	/**
	 * @brief Prune policy for keys matching a certain match criteria (e.g. regex)
	*/
	template <typename Key>
	struct key_match_prune_policy : public prune_policy_interface<Key, key_match_prune_policy<Key>>
	{
		using base_type = prune_policy_interface<Key, key_match_prune_policy<Key>>;
		using key_type = Key;
		using match_func = std::function<bool(const key_type&, const key_type&)>;

		explicit key_match_prune_policy(
			key_type matchString, 
			match_func func = std::equal_to<key_type>{})
			: matchString_(std::move(matchString))
			, match_(std::move(func))
		{}

	private:
		friend base_type;

		template <std::ranges::view V>
		std::pmr::vector<prune_element<key_type>>
		do_prune(std::pmr::memory_resource* res, const V& cacheView) const noexcept
		{
			try {
				size_t viewSize = std::distance(cacheView.begin(), cacheView.end());
				std::pmr::vector<prune_element<key_type>> vec(res);
				vec.reserve(std::max((size_t)100, viewSize/20));
				for (const auto& elem : cacheView) {
					if (match_(*elem.pruneKey_, matchString_)) {
						vec.emplace_back(elem); //trivially copy-able
					}
				}
				return vec;
			}
			catch (...) {}
			return {};
		}

	private:
		key_type matchString_;
		match_func match_;
	};

	/**
	 * @brief Prune policy for removing N oldest entries
	*/
	template <typename Key>
	struct size_restriction_prune_policy : public prune_policy_interface<Key, size_restriction_prune_policy<Key>>
	{
		using base_type = prune_policy_interface<Key, size_restriction_prune_policy<Key>>;
		using key_type = Key;
		struct exact{};
		struct percentage{};

		size_restriction_prune_policy(exact, size_t value)
			: numElements_(value)
		{}

		size_restriction_prune_policy(percentage, double value)
			: percentage_(value)
		{}

	private:
		friend base_type;

		template <std::ranges::view V>
		std::pmr::vector<prune_element<key_type>>
		do_prune(std::pmr::memory_resource* res, const V& cacheView) const noexcept
		{
			try {
				size_t numItemsToRemove = numElements_.value_or(0);
				size_t viewSize = std::distance(cacheView.begin(), cacheView.end());
				if (percentage_) numItemsToRemove = static_cast<size_t>((double)viewSize * percentage_.value() / 100);
				std::pmr::vector<prune_element<key_type>> vec(numItemsToRemove, res);

				//loop and sort
				std::partial_sort_copy(cacheView.begin(), cacheView.end(), vec.begin(), vec.end(),
					[](const prune_element<key_type>& lhs, const prune_element<key_type>& rhs)->bool {
						return *lhs.desc_ < *rhs.desc_;
					});

				return vec;
			}
			catch (...) {}
			return {};
		}

		std::optional<size_t> numElements_;
		std::optional<double> percentage_;
	};

	/**
	 * @brief Prune policy implementing least-recently-used (LRU) eviction.
	 *
	 * Entries are ordered by their last-access time and the `N` least recently
	 * used entries are selected for pruning. Because the cache refreshes an
	 * entry's last-access time on every successful read or write, applying this
	 * policy to the cache turns it into an LRU cache.
	 *
	 * `N` is specified either as an exact number of entries or as a percentage
	 * of the current cache size.
	*/
	template <typename Key>
	struct lru_prune_policy : public prune_policy_interface<Key, lru_prune_policy<Key>>
	{
		using base_type = prune_policy_interface<Key, lru_prune_policy<Key>>;
		using key_type = Key;
		struct exact{};
		struct percentage{};

		lru_prune_policy(exact, size_t value)
			: numElements_(value)
		{}

		lru_prune_policy(percentage, double value)
			: percentage_(value)
		{}

	private:
		friend base_type;

		template <std::ranges::view V>
		std::pmr::vector<prune_element<key_type>>
		do_prune(std::pmr::memory_resource* res, const V& cacheView) const noexcept
		{
			try {
				size_t numItemsToRemove = numElements_.value_or(0);
				size_t viewSize = std::distance(cacheView.begin(), cacheView.end());
				if (percentage_) numItemsToRemove = static_cast<size_t>((double)viewSize * percentage_.value() / 100);
				numItemsToRemove = std::min(numItemsToRemove, viewSize);

				//sort by least recently used (oldest last-access time first)
				//and keep only the first numItemsToRemove entries
				std::pmr::vector<prune_element<key_type>> vec(numItemsToRemove, res);
				auto last = std::partial_sort_copy(cacheView.begin(), cacheView.end(), vec.begin(), vec.end(),
					[](const prune_element<key_type>& lhs, const prune_element<key_type>& rhs)->bool {
						return *lhs.desc_ < *rhs.desc_;
					});
				vec.resize(static_cast<size_t>(std::distance(vec.begin(), last)));

				return vec;
			}
			catch (...) {}
			return {};
		}

		std::optional<size_t> numElements_;
		std::optional<double> percentage_;
	};

} //namespace 

#endif