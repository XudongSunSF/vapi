#include <atomic>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>
namespace utf = boost::unit_test;

#include "mortgage/utility/containers/cache/cache.h"
#include "mortgage/utility/containers/cache/caching.h"
#include "mortgage/utility/containers/cache/prune_policy.h"
#include "mortgage/utility/containers/cache/cache_entry_descriptor.h"
#include "mortgage/utility/exceptions/exceptions.h"
#include "mortgage/utility/types/rtti.h"

using namespace std;
using namespace std::chrono_literals;
using namespace wf::mortgage::utility;

auto transform_func = [](const auto& e)->prune_element<cache_key> {
	return {
		.pruneKey_ = &e.first,
		.desc_ = &e.second
	};
};

BOOST_AUTO_TEST_CASE(Cache_desc_less_than)
{
	cache_entry_descriptor desc1;
	desc1.lastAccessTime_ = desc1.creationTime_ - 1s;
	cache_entry_descriptor desc2;
	BOOST_CHECK(desc1 < desc2);
	BOOST_CHECK(desc1 != desc2);
}

BOOST_AUTO_TEST_CASE(Cache_desc_infinite_life)
{
	cache_entry_descriptor desc;
	BOOST_CHECK(!desc.is_expired());
	desc.lastAccessTime_ = chrono::system_clock::now() - 10000s;
	BOOST_CHECK(!desc.is_expired());
}


BOOST_AUTO_TEST_CASE(Cache_desc_expiry)
{
	cache_entry_descriptor desc(5s);
	desc.lastAccessTime_ = chrono::system_clock::now() - 10s;
	BOOST_CHECK(desc.is_expired());
	desc.extend_life();
	BOOST_CHECK(!desc.is_expired());
}

BOOST_AUTO_TEST_CASE(Cache_prune_ttl)
{
	//infinite expiry times
	pmr::vector<pair<cache_key, cache_entry_descriptor>> entries =
	{
		{{"one", "two"}, cache_entry_descriptor{}}, //ok
		{{"three", "four"}, cache_entry_descriptor{}},
		{{"five", "six"}, cache_entry_descriptor{}}, //ok
		{{"seven", "eight"}, cache_entry_descriptor{}}
	};
	//expire 1st and 3rd
	entries[0].second.timeToLive_ = 0s;
	entries[2].second.timeToLive_ = 0s;

	time_to_live_prune_policy<cache_key> pol;
	auto pruned = pol.prune(std::views::transform(entries, transform_func));
	BOOST_CHECK_EQUAL(2, pruned.size());
	BOOST_CHECK_EQUAL(entries[0].first, *pruned[0].pruneKey_);
	BOOST_CHECK_EQUAL(entries[2].first, *pruned[1].pruneKey_);
}

BOOST_AUTO_TEST_CASE(Cache_prune_exact_key)
{
	pmr::vector<pair<cache_key, cache_entry_descriptor>> entries =
	{
		{{"one", "two"}, cache_entry_descriptor{}},
		{{"three", "four"}, cache_entry_descriptor{}}, //ok
		{{"five", "six"}, cache_entry_descriptor{}},
		{{"seven", "eight"}, cache_entry_descriptor{}}
	};

	key_match_prune_policy<cache_key> pol({ "three", "four" });
	auto pruned = pol.prune(std::views::transform(entries, transform_func));
	BOOST_CHECK_EQUAL(1, pruned.size());
	BOOST_CHECK_EQUAL(entries[1].first, *pruned[0].pruneKey_);
}

BOOST_AUTO_TEST_CASE(Cache_prune_full_regex)
{
	cache_key pattern = { "one.*", ".*[0-9]+.*" };
	pmr::vector<pair<cache_key, cache_entry_descriptor>> entries =
	{
		{{"one", "two22"}, cache_entry_descriptor{}}, //ok
		{{"three", "four44"}, cache_entry_descriptor{}},
		{{"one_five", "66six"}, cache_entry_descriptor{}}, //ok
		{{"seven", "eight"}, cache_entry_descriptor{}}
	};

	key_match_prune_policy<cache_key> pol(pattern, regex_full_match<2>);
	auto pruned = pol.prune(std::views::transform(entries, transform_func));
	BOOST_CHECK_EQUAL(2, pruned.size());
	BOOST_CHECK_EQUAL(entries[0].first, *pruned[0].pruneKey_);
	BOOST_CHECK_EQUAL(entries[2].first, *pruned[1].pruneKey_);
}

BOOST_AUTO_TEST_CASE(Cache_prune_partial_regex)
{
	cache_key pattern = { "neo|ree", "our|ix" };
	pmr::vector<pair<cache_key, cache_entry_descriptor>> entries =
	{
		{{"oneone", "two22"}, cache_entry_descriptor{}},
		{{"threeone", "four44"}, cache_entry_descriptor{}}, //ok
		{{"5neo5_five", "66six"}, cache_entry_descriptor{}}, //ok
		{{"seven", "eight"}, cache_entry_descriptor{}}
	};

	key_match_prune_policy<cache_key> pol(pattern, regex_partial_match<2>);
	auto pruned = pol.prune(std::views::transform(entries, transform_func));
	BOOST_CHECK_EQUAL(2, pruned.size());
	BOOST_CHECK_EQUAL(entries[1].first, *pruned[0].pruneKey_);
	BOOST_CHECK_EQUAL(entries[2].first, *pruned[1].pruneKey_);
}

BOOST_AUTO_TEST_CASE(Cache_prune_all_letters)
{
	cache_key pattern = { "foo[0-9]", ".*" };
	pmr::vector<pair<cache_key, cache_entry_descriptor>> entries =
	{
		{{"foo1", "two22"}, cache_entry_descriptor{}},
		{{"foo2", "four44"}, cache_entry_descriptor{}},
		{{"foo3", "66six"}, cache_entry_descriptor{}},
		{{"foo4", "eight"}, cache_entry_descriptor{}}
	};

	key_match_prune_policy<cache_key> pol(pattern, regex_full_match<2>);
	auto pruned = pol.prune(std::views::transform(entries, transform_func));
	BOOST_CHECK_EQUAL(4, pruned.size());
	BOOST_CHECK_EQUAL(entries[0].first, *pruned[0].pruneKey_);
	BOOST_CHECK_EQUAL(entries[1].first, *pruned[1].pruneKey_);
	BOOST_CHECK_EQUAL(entries[2].first, *pruned[2].pruneKey_);
	BOOST_CHECK_EQUAL(entries[3].first, *pruned[3].pruneKey_);
}

BOOST_AUTO_TEST_CASE(Cache_prune_size)
{
	//infinite expiry times
	pmr::vector<pair<cache_key, cache_entry_descriptor>> entries =
	{
		{{"one", "two"}, cache_entry_descriptor{}},
		{{"three", "four"}, cache_entry_descriptor{}}, //ok
		{{"five", "six"}, cache_entry_descriptor{}},
		{{"seven", "eight"}, cache_entry_descriptor{}} //ok
	};
	//set diff access times
	entries[1].second.lastAccessTime_ = chrono::system_clock::now() - 50s;  //2nd oldest
	entries[3].second.lastAccessTime_ = chrono::system_clock::now() - 100s; //oldest

	//remove the 2 oldest entries
	size_restriction_prune_policy<cache_key> pol(size_restriction_prune_policy<cache_key>::exact{}, 2);
	auto pruned = pol.prune(std::views::transform(entries, transform_func));
	BOOST_CHECK_EQUAL(2, pruned.size());
	BOOST_CHECK_EQUAL(entries[3].first, *pruned[0].pruneKey_); //oldest
	BOOST_CHECK_EQUAL(entries[1].first, *pruned[1].pruneKey_);
}

BOOST_AUTO_TEST_CASE(Cache_empty)
{
	cache c;
	BOOST_CHECK_EQUAL(0, c.size());

	//Check stats
	BOOST_CHECK_EQUAL(0, c.stats().hits_);
	BOOST_CHECK_EQUAL(0, c.stats().misses_);
}

BOOST_AUTO_TEST_CASE(Cache_read_not_found)
{
	cache c;
	auto p = c.read<int>({ "this","that" });
	BOOST_CHECK(!p);
	BOOST_CHECK_EQUAL(0, c.stats().hits_);
	BOOST_CHECK_EQUAL(1, c.stats().misses_);
}

BOOST_AUTO_TEST_CASE(Cache_read_found)
{
	cache c;
	auto p = c.write({ "this","that" }, 10);
	BOOST_CHECK(p);
	auto p2 = c.read<int>({ "this","that" });
	BOOST_CHECK(p2);
	BOOST_CHECK_EQUAL(*p, *p2);
	BOOST_CHECK_EQUAL(1, c.stats().hits_);
	BOOST_CHECK_EQUAL(0, c.stats().misses_);
}

BOOST_AUTO_TEST_CASE(Cache_read_or_insert_not_found)
{
	cache c;
	auto p = c.read_or_insert<int>({ "this","that" }, [](){ return 5; });
	BOOST_CHECK(p);
	BOOST_CHECK_EQUAL(5, *p);
	BOOST_CHECK_EQUAL(0, c.stats().hits_);
	BOOST_CHECK_EQUAL(1, c.stats().misses_);
}

BOOST_AUTO_TEST_CASE(Cache_read_or_insert_found)
{
	cache c;
	auto p = c.write({ "this","that" }, 10);
	p = c.read_or_insert<int>({ "this","that" }, []() { return 5; });
	BOOST_CHECK(p);
	BOOST_CHECK_EQUAL(10, *p); //still has original value
	BOOST_CHECK_EQUAL(1, c.stats().hits_);
	BOOST_CHECK_EQUAL(1, c.stats({ "this","that" }).hits_);
	BOOST_CHECK_EQUAL(0, c.stats().misses_);
}

#ifndef NDEBUG

BOOST_AUTO_TEST_CASE(Cache_read_wrong_type)
{
	cache c;
	c.write({ "this", "that" }, 5);
	// value intentionally discarded
	BOOST_CHECK_THROW(
		static_cast<void>(c.read<double>({ "this", "that" })),
		InvalidTypeException
	);
}

BOOST_AUTO_TEST_CASE(Cache_overwrite_wrong_type)
{
	cache c;
	c.write({ "this", "that" }, 5);
	BOOST_CHECK_THROW(c.write({ "this", "that" }, (double)10), InvalidTypeException);
}

#endif

BOOST_AUTO_TEST_CASE(Cache_overwrite)
{
	cache c;
	c.write({ "this","that" }, 10);
	c.write({ "this","that" }, 20);
	BOOST_CHECK_EQUAL(1, c.size());
	BOOST_CHECK_EQUAL(20, *c.read<int>({ "this","that" }));
}

BOOST_AUTO_TEST_CASE(Cache_erase_existing_element)
{
	cache c;
	c.write({ "this","that" }, 10);
	c.write({ "this2","that2" }, 20);
	BOOST_CHECK_EQUAL(2, c.size());
	cache_stats s = c.stats(); //get stats before we erase an element
	BOOST_CHECK(c.erase({ "this2","that2" }));
	BOOST_CHECK_EQUAL(1, c.size());
	//check stats remain the same
	BOOST_CHECK_EQUAL(s, c.stats());
}

BOOST_AUTO_TEST_CASE(Cache_erase_nonexisting_element)
{
	cache c;
	c.write({ "this","that" }, 10);
	c.write({ "this2","that2" }, 20);
	BOOST_CHECK_EQUAL(2, c.size());
	BOOST_CHECK(!c.erase({ "this3", "" }));
	BOOST_CHECK_EQUAL(2, c.size());
}

BOOST_AUTO_TEST_CASE(Cache_clear)
{
	cache c;
	c.write({ "this","that" }, 10);
	c.write({ "this2","that2" }, 20);
	// value intentially discarded
	static_cast<void>(c.read<int>({ "this","that" }));
	BOOST_CHECK_EQUAL(1, c.stats().hits_);
	BOOST_CHECK_EQUAL(2, c.size());
	c.clear();
	BOOST_CHECK_EQUAL(0, c.size());
	bool equal = c.stats() == cache_stats{};
	BOOST_CHECK(equal);
}

BOOST_AUTO_TEST_CASE(Cache_prune_policy)
{
	cache c;
	c.write({ "this","that" }, 10);
	c.write({ "this2","that2" }, 20, -1s); //ensure entry is expired
	BOOST_CHECK_EQUAL(2, c.size());
	//run prune policy
	BOOST_CHECK_EQUAL(1, c.prune(time_to_live_prune_policy<cache_key>{}));
	BOOST_CHECK_EQUAL(1, c.size());
	//check remaining item
	BOOST_CHECK(c.read<int>({ "this","that" }));
	BOOST_CHECK(!c.read<int>({ "this2","that2" }));
}

BOOST_AUTO_TEST_CASE(Cache_extend_life)
{
	cache c;
	c.write({ "this","that" }, 10, 3s);
	c.write({ "this2","that2" }, 10, 3s);
	c.write({ "this3","that3" }, 10, 3s);
	BOOST_CHECK_EQUAL(0, c.prune(time_to_live_prune_policy<cache_key>{}));
	this_thread::sleep_for(3s);
	// value intentially discarded. entry lifetime reset
	static_cast<void>(c.read<int>({ "this","that" }));
	BOOST_CHECK_EQUAL(2, c.prune(time_to_live_prune_policy<cache_key>{}));
	BOOST_CHECK_EQUAL(1, c.size());
}

BOOST_AUTO_TEST_CASE(Cache_prune_size_on_insert)
{
	cache_options co;
	co.maxNumElements_ = 4;
	cache c(co);
	c.write({ "this","that" }, 1);
	this_thread::sleep_for(100ms);
	c.write({ "this2","that2" }, 1);
	this_thread::sleep_for(100ms);
	c.write({ "this3","that3" }, 1);
	this_thread::sleep_for(100ms);
	c.write({ "this4","that4" }, 1);
	this_thread::sleep_for(100ms);
	c.write({ "this5","that5" }, 1); //this should trigger pruning
	auto end = chrono::steady_clock::now() + 10s; //max wait time otherwise bail out
	while ((c.stats().sizePruneNumRuns_ == 0) && (chrono::steady_clock::now() < end)) {
		this_thread::sleep_for(100ms);
	}
	BOOST_CHECK_EQUAL(4, c.size());
	BOOST_CHECK_EQUAL(1, c.stats().sizePruneNumRuns_);
	//make sure the oldest entry has been removed
	BOOST_CHECK(!c.read<int>({ "this","that" }));
}

BOOST_AUTO_TEST_CASE(Cache_prune_interval_ttl)
{
	cache_options co;
	co.pruneInterval_ = 1s;
	cache c(co);
	//no expiries
	c.write({ "this","that" }, 1);
	c.write({ "this2","that2" }, 1);
	//1s ttl
	c.write({ "this3","that3" }, 1, 1s);
	c.write({ "this4","that4" }, 1, 1s);
	c.write({ "this5","that5" }, 1, 1s);
	auto end = chrono::steady_clock::now() + 10s; //max wait time otherwise bail out
	while ((c.size() > 2) && (chrono::steady_clock::now() < end)) {
		this_thread::sleep_for(100ms);
	}
	BOOST_CHECK_EQUAL(2, c.size());
	BOOST_CHECK_NE(0, c.stats().ttlPruneNumRuns_);
}

BOOST_AUTO_TEST_CASE(Cache_max_ttl)
{
	cache_options co;
	co.pruneInterval_ = 1s;
	co.maxTimeToLive_ = 3s;
	cache c(co);
	//no expiries (uses max TTL)
	c.write({ "this","that" }, 1);
	c.write({ "this2","that2" }, 1);
	//1s ttl
	c.write({ "this3","that3" }, 1, 1s);
	c.write({ "this4","that4" }, 1, 1s);
	c.write({ "this5","that5" }, 1, 1s);
	auto end = chrono::steady_clock::now() + 10s; //max wait time otherwise bail out
	while ((c.size() > 0) && (chrono::steady_clock::now() < end)) {
		this_thread::sleep_for(100ms);
	}
	BOOST_CHECK_EQUAL(0, c.size());
	BOOST_CHECK_NE(0, c.stats().ttlPruneNumRuns_);
}

BOOST_AUTO_TEST_CASE(Cache_stress_test)
{
	cache_options co;
	co.pruneInterval_ = 100ms;
	co.maxTimeToLive_ = 100ms;
	co.maxNumElements_ = 5;
	co.dyanamicStripes_ = true;
	co.maxNumStripes_ = 2;
	cache c(co);

	//keys for threads
	std::pmr::vector<cache_key> keys = {
		{"1","10"},
		{"2", "20"},
		{"3", "30"},
		{ "4", "40"},
		{ "5", "50"},
		{ "6", "60"},
		{ "7", "70"},
		{ "8", "80"},
		{ "9", "90"},
		{ "10", "100"}
	};

	atomic_flag terminate = ATOMIC_FLAG_INIT;
	pmr::vector<std::thread> threads;

	//thread func
	auto tfunc = [&terminate, &keys](const std::function<void(const cache_key&)>& exec) {
		while (!terminate.test()) {
			for (const auto& k : keys) {
				exec(k);
			}
			this_thread::sleep_for(10ms);
		}
	};

	//executors
	//reader
	auto e1 = [&c](const cache_key& k) {
		[[maybe_unused]] auto r = c.read<int>(k);
	};
	threads.emplace_back(tfunc, e1);

	auto e2 = [&c](const cache_key& k) {
		[[maybe_unused]] auto r = c.read_or_insert<int>(k, []()->int {return 1;} );
	};
	threads.emplace_back(tfunc, e2);

	auto e3 = [&c](const cache_key& k) {
		c.write(k, 2);
	};
	threads.emplace_back(tfunc, e3);

	auto e4 = [&c](const cache_key& k) {
		[[maybe_unused]] auto r = c.stats();
	};
	threads.emplace_back(tfunc, e4);

	std::chrono::seconds sleepDuration = 10s;
	size_t maxTtlPrunes = sleepDuration.count() * 1000 / co.pruneInterval_.value().count();
	this_thread::sleep_for(sleepDuration);
	terminate.test_and_set();
	for (auto& t : threads) {
		t.join();
	}
	size_t size = c.size();
	auto stats = c.stats();
	BOOST_CHECK(size >= 0 && size <= keys.size());
	BOOST_CHECK(stats.ttlPruneNumRuns_ <= maxTtlPrunes);
}

// TODO: consider placing tests in test suite instead of solo test cases
BOOST_AUTO_TEST_SUITE(cache)

namespace {

/**
 * Simple class that throws a `LibException` when its argument is not positive.
 */
class pos_holder {
public:
    // default ctor for std::promise (have to copy or move)
    pos_holder() noexcept : value_{1} {}

    pos_holder(int value)
    {
        if (value < 1)
            throw exceptions::ArgumentException{"value must be positive"};
        value_ = value;
    }

    auto value() const noexcept { return value_; }

private:
    int value_;
};

}  // namespace

/**
 * Test that exceptions correctly propagate out of the cache.
 */
BOOST_AUTO_TEST_CASE(propagate_exceptions)
{
    // default options are fine here
    containers::cache cache;
    BOOST_CHECK_THROW(
        caching::cached(
            &cache,
            {WF_DEMANGLED_NAME(pos_holder), "key"},
            [] { return pos_holder{-1}; }
        ),
        exceptions::ArgumentException
    );
}

/**
 * Test that exceptions propagate out of the cache in multithreaded context.
 */
BOOST_AUTO_TEST_CASE(propagate_exceptions_mt)
{
    // cache and number of tasks
    containers::cache cache;
    constexpr unsigned int n_tasks = 50U;
    // get futures for execution in separate threads
    std::vector<std::future<pos_holder>> futures;
    for (int i = 0; i < static_cast<int>(n_tasks); i++)
        futures.push_back(
            std::async(
                std::launch::async,
                [&cache, i]
                {
                    return *caching::cached(
                        &cache,
                        {WF_DEMANGLED_NAME(pos_holder), "key" + std::to_string(-i)},
                        [i] { return pos_holder{-i}; }
                    );
                }
            )
        );
    // check that each future has thrown
    for (auto& future : futures)
        BOOST_CHECK_THROW(future.get(), exceptions::ArgumentException);
}

BOOST_AUTO_TEST_SUITE_END()  // cache

BOOST_AUTO_TEST_SUITE(single_flight_cache)

// ---------------------------------------------------------------------------
// Single-flight
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(concurrent_identical_requests_build_value_once)
{
	cache_options co;
	co.buildCoordinationPolicy_ = build_coordination_policy::single_flight;
	cache c(co);
	const cache_key key = { "single", "flight" };

	constexpr int threadCount = 32;
	std::atomic buildCount{ 0 };
	std::latch gate(threadCount);
	// each worker counts down just before read_or_insert; the builder's creator waits on it
	std::latch entered(threadCount);
	std::vector<std::shared_ptr<const int>> handles(threadCount);
	std::vector<std::thread> workers;
	workers.reserve(threadCount);

	for (int i = 0; i < threadCount; ++i) {
		workers.emplace_back([&gate, &entered, &handles, &c, &buildCount, key, i] {
			gate.arrive_and_wait(); // release all threads together to force the race
			entered.count_down();   // signal this thread has reached read_or_insert
			handles[i] = c.read_or_insert<int>(key, [&buildCount, &entered] {
				buildCount.fetch_add(1, std::memory_order_relaxed);
				entered.wait(); // hold the single build open until every caller has reached read_or_insert
				return 7;
			});
		});
	}
	for (auto& w : workers) w.join(); // ensure all builds completed before asserting

	BOOST_CHECK_EQUAL(buildCount.load(), 1);   // built exactly once
	BOOST_CHECK_EQUAL(c.size(), 1u);

	// Every caller received the very same value instance.
	for (int i = 0; i < threadCount; ++i) {
		BOOST_REQUIRE(handles[i] != nullptr);
		BOOST_CHECK(handles[i].get() == handles[0].get());
	}
	BOOST_CHECK_EQUAL(*handles[0], 7);
}

// ---------------------------------------------------------------------------
// Keying
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(distinct_keys_build_separate_entries)
{
	cache_options co;
	co.buildCoordinationPolicy_ = build_coordination_policy::single_flight;
	cache c(co);
	std::atomic buildCount{ 0 };
	auto creator = [&buildCount] {
		buildCount.fetch_add(1, std::memory_order_relaxed);
		return 1;
	};

	c.read_or_insert<int>({ "single", "one" }, creator);
	c.read_or_insert<int>({ "single", "two" }, creator);
	c.read_or_insert<int>({ "single", "one" }, creator); // hits the first entry, no rebuild

	BOOST_CHECK_EQUAL(buildCount.load(), 2);
	BOOST_CHECK_EQUAL(c.size(), 2u);
}

// ---------------------------------------------------------------------------
// Concurrent reuse of a shared cached value
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(concurrent_reuse_of_shared_value_is_race_free)
{
	cache_options co;
	co.buildCoordinationPolicy_ = build_coordination_policy::single_flight;
	cache c(co);
	const cache_key key = { "single", "shared" };

	auto handle = c.read_or_insert<int>(key, [] { return 7; });
	const int* shared = handle.get();

	constexpr int threadCount = 16;
	std::atomic okCount{ 0 };
	std::latch gate(threadCount);
	std::vector<std::thread> workers;
	workers.reserve(threadCount);

	for (int i = 0; i < threadCount; ++i) {
		workers.emplace_back([&gate, &c, &shared, &okCount, key] {
			gate.arrive_and_wait();
			auto localHandle = c.read_or_insert<int>(key, [] { return -1; });
			if (localHandle && localHandle.get() == shared && *localHandle == 7) {
				okCount.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	for (auto& w : workers) w.join(); // ensure all readers completed before asserting

	BOOST_CHECK_EQUAL(okCount.load(), threadCount);
	BOOST_CHECK_EQUAL(c.size(), 1u);
}

// ---------------------------------------------------------------------------
// Failure handling
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(failed_build_is_not_cached_and_is_retryable)
{
	cache_options co;
	co.buildCoordinationPolicy_ = build_coordination_policy::single_flight;
	cache c(co);
	const cache_key key = { "single", "flight" };
	std::atomic attempts{ 0 };

	BOOST_CHECK_THROW(
		c.read_or_insert<int>(key, [&attempts]() -> int {
			attempts.fetch_add(1, std::memory_order_relaxed);
			throw std::runtime_error("build failed");
		}),
		std::runtime_error);

	BOOST_CHECK_EQUAL(c.size(), 0u); // poisoned entry removed

	auto handle = c.read_or_insert<int>(key, [&attempts] {
		attempts.fetch_add(1, std::memory_order_relaxed);
		return 3;
	});

	BOOST_REQUIRE(handle != nullptr);
	BOOST_CHECK_EQUAL(*handle, 3);
	BOOST_CHECK_EQUAL(attempts.load(), 2); // first failed, second succeeded
	BOOST_CHECK_EQUAL(c.size(), 1u);
}

// A single-flight build for one key must not block builds for other keys:
// while key 1 is in-flight, key 2 can still be built and returned.
BOOST_AUTO_TEST_CASE(in_flight_build_does_not_block_other_keys)
{
	cache_options co;
	co.buildCoordinationPolicy_ = build_coordination_policy::single_flight;
	cache c(co);

	std::latch key1Building(1); // released once key1's build has started
	std::latch releaseKey1(1);  // key1's build blocks here so it stays in-flight

	std::thread worker([&c, &key1Building, &releaseKey1] {
		c.read_or_insert<int>({ "single", "one" }, [&key1Building, &releaseKey1] {
			key1Building.count_down();
			releaseKey1.wait();
			return 1;
		});
	});

	key1Building.wait(); // ensure key1 is in-flight before building key2

	auto h2 = c.read_or_insert<int>({ "single", "two" }, [] { return 2; });
	BOOST_CHECK_EQUAL(*h2, 2);

	releaseKey1.count_down();
	worker.join();

	BOOST_CHECK_EQUAL(c.size(), 2u);
}

#ifndef NDEBUG
// The debug guard turns a would-be self-deadlock into a clear exception.
BOOST_AUTO_TEST_CASE(reentrant_same_key_build_throws_in_debug)
{
	cache_options co;
	co.buildCoordinationPolicy_ = build_coordination_policy::single_flight;
	cache c(co);
	const cache_key key = { "single", "reentrant" };

	BOOST_CHECK_THROW(
		c.read_or_insert<int>(key, [&c, &key] {
			c.read_or_insert<int>(key, [] { return 2; }); // same key, same thread
			return 1;
		}),
		exceptions::InvalidStateException);

	// The failed build left no entry; a later request rebuilds cleanly.
	auto handle = c.read_or_insert<int>(key, [] { return 7; });
	BOOST_CHECK_EQUAL(*handle, 7);
	BOOST_CHECK_EQUAL(c.size(), 1u);
}
#endif

BOOST_AUTO_TEST_SUITE_END()  // single_flight_cache
