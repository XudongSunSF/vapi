#ifndef WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_STRIPE_H
#define WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_STRIPE_H

#include <array>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>


#include "mortgage/utility/containers/cache/cache_entry_descriptor.h"
#include "mortgage/utility/containers/cache/cache_options.h"
#include "mortgage/utility/containers/cache/cache_reader_interface.h"
#include "mortgage/utility/containers/cache/cache_writer_interface.h"
#include "mortgage/utility/exceptions/exceptions.h"
#include "mortgage/utility/namespaces.h"
#include "mortgage/utility/memory/allocator.h"
#include "mortgage/utility/types/rtti.h"

namespace wf::mortgage::utility::containers::detail {

/**
 * Cache stripe implementation satisfying the cache reader/writer interfaces.
 *
 * @note Locks can fail on Linux as `shared_mutex::lock()` in libstdc++ is not
 *  `noexcept` as one can get `EDEADLK` from the underlying pthreads lock.
 *  Currently we don't mark any locking `do_` functions as `noexcept`. We
 *  could conditionally check with `noexcept(mutex_.lock())`.
 *
 * @note Some `noexcept` functions call STL functions that are technically not
 *  `noexcept` according to the standard (and from looking at implementation).
 *
 * @note Some functions are `noexcept` conditional on `NDEBUG` being defined
 *  because they may have some debugging throws inserted in them.
 */
struct cache_stripe : public cache_reader_interface<cache_key, cache_stripe>,
		              public cache_writer_interface<cache_key, cache_stripe>
{
	// types
	using key_type = cache_key;
	using reader = cache_reader_interface<key_type, cache_stripe>;
	using writer = cache_writer_interface<key_type, cache_stripe>;

	template <typename T>
	using creator_func = typename reader::creator_func<T>;

	struct cache_entry {
		cache_entry() = default;
		mutable cache_entry_descriptor desc_;
		std::shared_ptr<const void> value_;
	};

    using container = std::pmr::unordered_map<key_type, cache_entry>;

	// State used to coordinate single-flight read_or_insert operations for a
	// given key. The shared_ptr indirection keeps the state alive for threads
	// that are still waiting after the leader has removed it from the map.
	struct in_flight {
		std::mutex mutex_;
		std::condition_variable cv_;
		bool done_{ false };
		std::exception_ptr error_;
		// Thread that is creating the value; used to detect a reentrant same-key
		// build on the same thread (which would otherwise wait on itself).
		std::thread::id leaderId_{};
	};

	using in_flight_map = std::pmr::unordered_map<key_type, std::shared_ptr<in_flight>>;

	// ctor
	cache_stripe(
		const std::optional<std::pmr::pool_options>& memPoolOptions,
		build_coordination_policy coordinationPolicy =
			build_coordination_policy::concurrent);
	cache_stripe(const cache_stripe&) = delete;
	cache_stripe(cache_stripe&& other) noexcept;
	cache_stripe& operator=(const cache_stripe&) = delete;
	cache_stripe& operator=(cache_stripe&& other) noexcept;

	auto key_view() const noexcept;

protected:
	friend reader;
	friend writer;

    /**
     * Read a cache object given the specified key.
     *
     * If the object does not exist in the cache the shared pointer is empty.
     *
     * @tparam T type
     *
     * @param k Key
     */
	template <typename T>
    std::shared_ptr<const T> do_read(const key_type& k) const WF_NDEBUG_NOEXCEPT;

    /**
     * Read a cache object given the specified key, otherwise insert.
     *
     * @note This function is thread-safe.
     *
     * @tparam T type
     *
     * @param k Key
     * @param f Callable use to create the object if not found
     * @param ttl Optional time to live
     */
	template <typename T>
    std::shared_ptr<const T>
    do_read_or_insert(
		const key_type& k,
		const creator_func<T>& f,
		std::optional<time_to_live> ttl);

    /**
     * Insert the object into the cache given the specified key.
     *
     * @note This is not thread-safe since it does not lock the mutex.
     * @note This may still throw if `T&&` move ctor can throw.
     *
     * @tparam T type
     *
     * @param k Key
     * @param value Value to insert from move
     * @param ttl Optional time to live
     */
	template <typename T>
	std::shared_ptr<const std::decay_t<T>>
    do_insert(
		const key_type& k,
		T&& value,
		std::optional<time_to_live> ttl) noexcept;

    /**
     * Insert the object or overwrite an existing object using the given key.
     *
     * @tparam T type
     *
     * @param k key
     * @param val Value to insert/overwrite with from move
     * @param ttl Optional time to live
     */
	template <typename T>
	std::shared_ptr<const std::decay_t<T>>
	do_write(
		const key_type& k,
		T&& val,
		std::optional<time_to_live> ttl) WF_NDEBUG_NOEXCEPT;

    /**
     * Perform pruning.
     *
     * @note The cache stripe itself doesn't actually do any pruning.
     *
     * @tparam Policy Pruning policy type
     *
     * @param policy
     * @returns Number of items pruned from cache
     */
	template <typename Policy>
	size_t do_prune(const Policy& policy) noexcept;

    /**
     * Erase the value given a cache key.
     *
     * @param k Cache key
     * @returns `true` if erase succeeded, `false` otherwise
     */
	bool do_erase(const key_type& k) noexcept;

    /**
     * Return the number of objects in the cache stripe.
     */
	size_t do_size() const noexcept;

	void do_clear() noexcept;

	cache_stats do_stats(const key_type& k) const noexcept;

	void do_reset_stats(const key_type& k) noexcept;

	//NOTE: this is managed by the parent cache
	bool do_is_prune_operation_running() const noexcept { return false; }

	//NOTE: this is managed by the parent cache
	void do_wait_until_prune_finishes() const noexcept {}

	//Removes the in-flight build for k and wakes up any threads that are
	//waiting on it. `error` is null for a successful build.
	void release_flight(
		const key_type& k,
		const std::shared_ptr<in_flight>& flight,
		std::exception_ptr error) noexcept;

	//members
	mutable std::shared_mutex mutex_;
	std::pmr::pool_options memPoolOptions_;
	memory::statistics_memory_resource statsRes_;
	std::pmr::synchronized_pool_resource res_;
	container cache_{ &res_ };
	mutable cache_stats stats_;
	build_coordination_policy coordinationPolicy_{ build_coordination_policy::concurrent };
	std::mutex inFlightMutex_;
	in_flight_map inFlight_{ &res_ };
};

///////////////////////////////////////////////////////////////////////////////
// IMPLEMENTATIONS
///////////////////////////////////////////////////////////////////////////////

inline
cache_stripe::cache_stripe(
	const std::optional<std::pmr::pool_options>& memPoolOptions,
	build_coordination_policy coordinationPolicy)
	: memPoolOptions_(memPoolOptions.value_or(std::pmr::pool_options{}))
	, statsRes_(synchronization_type::synchronized, std::pmr::get_default_resource())
	, res_(memPoolOptions_, &statsRes_)
	, coordinationPolicy_(coordinationPolicy)
{}

inline
cache_stripe::cache_stripe(cache_stripe&& other) noexcept
	: memPoolOptions_(other.memPoolOptions_)
	, statsRes_(synchronization_type::synchronized, std::pmr::get_default_resource())
	, res_(memPoolOptions_, &statsRes_)
	, cache_(std::move(other.cache_), &res_)
	, stats_(other.stats_)
	, coordinationPolicy_(other.coordinationPolicy_)
	, inFlight_(std::move(other.inFlight_), &res_)
{
}

inline cache_stripe&
cache_stripe::operator=(cache_stripe&& other) noexcept
{
	if (this == &other) return *this;
	cache_ = std::move(other.cache_);
	stats_ = other.stats_;
	coordinationPolicy_ = other.coordinationPolicy_;
	inFlight_ = std::move(other.inFlight_);
	return *this;
}

inline auto
cache_stripe::key_view() const noexcept
{
	return std::views::transform(
        cache_,
        [&](const auto& elemPair) -> prune_element<cache_key>
        {
            return {
                .pruneKey_ = &elemPair.first,
                .desc_ = &elemPair.second.desc_
            };
	    }
    );
}

template <typename T>
std::shared_ptr<const T>
cache_stripe::do_read(const key_type& k) const WF_NDEBUG_NOEXCEPT
{
    // C++17: use CTAD
    std::shared_lock lock(mutex_);
    auto it = cache_.find(k);
    // couldn't find, increment misses and return empty
    if (it == cache_.end()) {
        stats_.misses_++;
        return {};
    }
    // found in cache. increment cache descriptor and mutable stats
    cache_entry_descriptor& desc = it->second.desc_;
    // debug only: allow checking exact types
#ifndef NDEBUG
    if (!desc.is_same_type(typeid(T))) {
        THROW_EX(
            InvalidTypeException,
            std::format(
                "Mismatched types: stored [{}] desired [{}]",
                desc.type_name(),
                WF_DEMANGLED_NAME(T)
            )
        );
    }
#endif  // NDEBUG
    desc.extend_life();
    desc.increment_hits();
    ++stats_.hits_;
    return std::static_pointer_cast<const T>(it->second.value_);
}

template <typename T>
std::shared_ptr<const T>
cache_stripe::do_read_or_insert(
	const key_type& k,
	const creator_func<T>& f,
	std::optional<time_to_live> ttl)
{
	assert(f);

	// Concurrent cache behavior: every thread that misses the cache invokes the
	// creator function independently. The first insertion for a key wins.
	if (coordinationPolicy_ == build_coordination_policy::concurrent) {
		auto ptr = do_read<T>(k);
		if (ptr)
			return ptr;
		// run user-supplied function outside of any locks
		T val = f();
		// C++17: use CTAD
		std::unique_lock lock(mutex_);
		return do_insert(k, std::move(val), ttl);
	}

	// Single-flight insertion: only one thread creates the value for a given
	// key at a time. All other threads block until the value has been inserted
	// and then read the inserted value.
	while (true) {
		{
			auto ptr = do_read<T>(k);
			if (ptr)
				return ptr;
		}

		std::shared_ptr<in_flight> flight;
		bool isLeader = false;
		{
			std::unique_lock<std::mutex> lock(inFlightMutex_);
			auto [it, inserted] = inFlight_.emplace(k, nullptr);
			if (inserted) {
				isLeader = true;
				flight = std::allocate_shared<in_flight>(
					std::pmr::polymorphic_allocator<in_flight>(&res_)
				);
				flight->leaderId_ = std::this_thread::get_id();
				it->second = flight;
			}
			else {
				flight = it->second;
				// A creator that re-enters read_or_insert for the same key on the
				// same thread would block waiting on its own in-flight build. Fail
				// fast instead of deadlocking.
				if (flight->leaderId_ == std::this_thread::get_id()) {
					THROW_EX(
						InvalidStateException,
						"cache: reentrant single-flight build for the same key on the "
						"same thread would deadlock");
				}
			}
		}

		if (!isLeader) {
			// Wait for the leader to finish creating the value.
			std::unique_lock<std::mutex> lock(flight->mutex_);
			flight->cv_.wait(lock, [&flight] { return flight->done_; });
			if (flight->error_)
				std::rethrow_exception(flight->error_);
			// Loop back to read the inserted value.
			continue;
		}

		// The value may have been inserted between our initial read above and
		// acquiring leadership; re-check before invoking the creator.
		if (auto ptr = do_read<T>(k); ptr) {
			release_flight(k, flight, {});
			return ptr;
		}

		// This thread is the leader: create the value outside of any locks.
		std::shared_ptr<const T> result;
		try {
			T val = f();
			std::unique_lock<std::shared_mutex> lock(mutex_);
			result = do_insert(k, std::move(val), ttl);
		}
		catch (...) {
			release_flight(k, flight, std::current_exception());
			throw;
		}
		release_flight(k, flight, {});
		return result;
	}
}

inline void
cache_stripe::release_flight(
	const key_type& k,
	const std::shared_ptr<in_flight>& flight,
	std::exception_ptr error) noexcept
{
	{
		std::unique_lock<std::mutex> lock(inFlightMutex_);
		inFlight_.erase(k);
	}
	{
		std::lock_guard<std::mutex> lock(flight->mutex_);
		flight->error_ = std::move(error);
		flight->done_ = true;
	}
	flight->cv_.notify_all();
}

template <typename T>
std::shared_ptr<const std::decay_t<T>>
cache_stripe::do_insert(
	const key_type& k,
	T&& value,
	std::optional<time_to_live> ttl) noexcept
{
	using value_type = std::decay_t<T>;
	// populate cache entry info
	cache_entry entry;
#if defined(NDEBUG)
    entry.desc_ = cache_entry_descriptor(ttl);
#else
    entry.desc_ = cache_entry_descriptor(typeid(value_type), ttl);
#endif  // !defined(NDEBUG)
    std::pmr::polymorphic_allocator<value_type> alloc(&res_);
    entry.value_ = std::allocate_shared<const value_type>(alloc, std::forward<T>(value));
    auto result = cache_.emplace(k, std::move(entry));
    return std::static_pointer_cast<const value_type>(result.first->second.value_);
}

template <typename Policy>
size_t cache_stripe::do_prune(const Policy& /*policy*/) noexcept
{
	return 0;
}

template <typename T>
std::shared_ptr<const std::decay_t<T>>
cache_stripe::do_write(
	const key_type& k,
	T&& value,
	std::optional<time_to_live> ttl) WF_NDEBUG_NOEXCEPT
{
	using value_type = std::decay_t<T>;
    // need unique locking here for writes (C++17: using CTAD)
    std::unique_lock lock(mutex_);
    auto it = cache_.find(k);
    // couldn't find in cache so just insert
    if (it == cache_.end())
        return do_insert(k, std::forward<T>(value), ttl);
    // otherwise, found in cache
    cache_entry_descriptor& desc = it->second.desc_;
#ifndef NDEBUG
    //ensure the new type is identical to the old
    if (!desc.is_same_type(typeid(value_type))) {
        THROW_EX(
            InvalidTypeException,
            std::format(
                "Mismatched types: stored [{}] desired [{}]",
                desc.type_name(),
                WF_DEMANGLED_NAME(value_type)
            )
        );
    }
#endif  // NDEBUG
    // update
    std::pmr::polymorphic_allocator<value_type> alloc(&res_);
    desc.extend_life();
    desc.increment_hits();
    if (ttl) {
        desc.timeToLive_ = ttl.value();
    }
    it->second.value_ = std::allocate_shared<const value_type>(
        alloc, std::forward<T>(value)
    );
    return std::static_pointer_cast<const value_type>(it->second.value_);
}

inline bool
cache_stripe::do_erase(const key_type& k) noexcept
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    // can throw if Hash or KeyEqual type operator() operators throw. will
    // only return 0 or 1 for number of items erased
    // note: !! to silence C4800
    return !!cache_.erase(k);
}

inline size_t
cache_stripe::do_size() const noexcept
{
    // can throw std::system_error in which case std::terminate is called
	std::shared_lock lock(mutex_);
	return cache_.size();
}

inline void
cache_stripe::do_clear() noexcept
{
    // C++17: using CTAD (can throw std::system_error)
    std::unique_lock lock(mutex_);
    cache_.clear();
    stats_.reset();
}

inline cache_stats
cache_stripe::do_stats(const key_type& k) const noexcept
{
    // note: can throw std::system_error
	std::shared_lock lock(mutex_);
    // get full stripe stats
	if (k == key_type{})
		return stats_;
	// get specific key stats
	cache_stats stats;
	auto it = cache_.find(k);
	if (it != cache_.end()) {
		const cache_entry_descriptor& desc = it->second.desc_;
		stats.hits_ = desc.hits_.load();
	}
	return stats;
}

inline void
cache_stripe::do_reset_stats(const key_type& k) noexcept
{
    // can throw std::system_error
	std::unique_lock lock(mutex_);
	if (k == key_type{}) {
		//reset stripe stats
		stats_.reset();
		//reset individual key stats
		for (auto& [key, entry] : cache_) {
			entry.desc_.reset_hits();
		}
	}
	else if (auto it = cache_.find(k); it != cache_.end()) {
		//reset specific key stats
		it->second.desc_.reset_hits();
	}
}

}  // namespace wf::mortgage::utility::containers::detail

#endif  // WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_STRIPE_H
