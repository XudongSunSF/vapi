#ifndef WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_H
#define WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_H

#include <atomic>
#include <latch>
#include <thread>

#include "mortgage/utility/namespaces.h"
#include "mortgage/utility/containers/cache/impl/cache_stripe.h"
#include "mortgage/utility/containers/cache/cache_options.h"
#include "mortgage/utility/containers/cache/prune_policy.h"
#include "mortgage/utility/containers/hash.h"

namespace wf::mortgage::utility::containers {

/**
 * Polymorphic cache implementation.
 *
 * This provides a general caching implementation that evicts items based on
 * time to live and has improved throughput due to use of individual mutexes
 * for each cache "stripe" that comprises the cache.
 */
struct cache : public cache_reader_interface<cache_key, cache>,
	           public cache_writer_interface<cache_key, cache>
{
	using key_type = cache_key;
	using reader = cache_reader_interface<key_type, cache>;
	using writer = cache_writer_interface<key_type, cache>;
	template <typename T>
	using creator_func = typename reader::creator_func<T>;

	explicit cache(cache_options opts = {});
	~cache();
	cache(const cache&) = delete;
	cache(cache&&) = delete;
	cache& operator=(const cache&) = delete;
	cache& operator=(cache&&) = delete;

protected:
	friend reader;
	friend writer;

	// TODO: add some doc comments. for now refer to impl/cache_stripe.h

	template <typename T>
	std::shared_ptr<const T>
	do_read(const key_type& k) const WF_NDEBUG_NOEXCEPT;

	template <typename T>
	std::shared_ptr<const T>
	do_read_or_insert(
		const key_type& k,
		const creator_func<T>& f,
		std::optional<time_to_live> ttl);

	template <typename T>
	std::shared_ptr<const std::decay_t<T>>
	do_write(
		const key_type& k,
		T&& val,
		std::optional<time_to_live> ttl) WF_NDEBUG_NOEXCEPT;

	template <typename Policy>
	size_t do_prune(const Policy& policy) noexcept;

	bool do_erase(const key_type& k) noexcept;

	//NOTE: must be called under lock
	bool do_erase_unsafe(const key_type& k) noexcept;

	size_t do_size() const noexcept;

	//NOTE: must be called under lock
	size_t do_size_unsafe() const noexcept;

	void do_clear() noexcept;

	cache_stats do_stats(const key_type& k) const noexcept;

	void do_reset_stats(const key_type& k) noexcept;

	void ttl_pruning() noexcept;

	void size_pruning() noexcept;

	bool do_is_prune_operation_running() const noexcept;

	void do_wait_until_prune_finishes() const noexcept;

	void check_max_elements() const noexcept;

	void create_new_stripe(size_t idx) noexcept;

	//members
	using stripe_container = std::pmr::unordered_map<size_t, detail::cache_stripe>;

	//cache members
	cache_options opts_;
	mutable cache_stats stripeStats_;
	mutable std::shared_mutex stripeMutex_;
	wfmutil::memory::statistics_memory_resource stripeStatsRes_;
	std::pmr::unsynchronized_pool_resource stripeRes_;
	stripe_container stripes_{ &stripeRes_ };

	//prune related members
	std::latch latch_;
	std::pmr::vector<std::thread> pruningThreads_;
	std::atomic<bool> terminate_{ false };
	time_to_live_prune_policy<key_type> ttlPolicy_;
	mutable std::mutex ttlPolicyMutex_;
	mutable std::condition_variable ttlPolicyCond_;
	size_restriction_prune_policy<key_type> sizePolicy_;
	mutable std::mutex sizePolicyMutex_;
	mutable std::condition_variable sizePolicyCond_;
	// Pending vs completed size-prune requests. Guarded by sizePolicyMutex_.
	mutable size_t sizePruneRequested_{ 0 };
	mutable size_t sizePruneCompleted_{ 0 };
	mutable std::shared_mutex pruningMutex_;
	mutable std::condition_variable_any pruningCond_;
	mutable std::atomic_flag isPruning_{};
};

}  // namespace wf::mortgage::utility::containers

#include "mortgage/utility/containers/cache/impl/cache_impl.h"

#endif  // WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_H
