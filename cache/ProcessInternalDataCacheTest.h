/**
 * @file ProcessInternalDataCacheTest.cpp
 * @brief Unit tests for app::messages::ProcessInternalDataCache (generic, LRU eviction).
 *
 * Covers:
 *  - Single-flight: under concurrent identical requests the value is built
 *    exactly once and all callers share the same instance.
 *  - Keying: distinct keys build separate entries; a repeated key hits the
 *    existing entry. Works with a custom key type + hash.
 *  - Concurrent reuse: many threads read a shared cached value race-free.
 *  - Failure is retryable: a throwing build leaves no cached entry and a later
 *    request rebuilds successfully.
 *  - Capacity/LRU: the cap evicts least-recently-used entries; evicted values
 *    stay alive via held handles. The default cap is large, so a modest number
 *    of entries is never evicted.
 *  - Reentrancy: a same-key reentrant build is detected in debug builds.
 *
 * Framework: Boost.Test (auto-registered).
 * Suite:     wfmcm_app_common_test  (cc/test/app-common/)
 */

#include <boost/test/unit_test.hpp>

#include <src/app-common/messages/cache/ProcessInternalDataCache.h>

#include <atomic>
#include <cstddef>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

using app::messages::ProcessInternalDataCache;

namespace {

// A trivially built value stood up by the cache; the int marker lets a test
// confirm every caller received the same creator output.
struct Marker
{
    int value = 0;
};

using MarkerCache = ProcessInternalDataCache<int, Marker>;

// A custom key type + hash to exercise the KeyHash template parameter (as the
// base-context cache does with BaseContextKey / BaseContextKeyHash).
struct CustomKey
{
    std::size_t a = 0;
    std::size_t b = 0;

    bool operator==(const CustomKey&) const noexcept = default;
};

struct CustomKeyHash
{
    std::size_t operator()(const CustomKey& key) const noexcept { return key.a ^ (key.b << 1); }
};

} // namespace

BOOST_AUTO_TEST_SUITE(single_flight_cache)

// ---------------------------------------------------------------------------
// Single-flight
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(concurrent_identical_requests_build_value_once)
{
    MarkerCache cache;
    constexpr int key = 42;

    constexpr int threadCount = 32;
    std::atomic buildCount{0};
    std::latch gate(threadCount);
    std::latch entered(threadCount); // each worker counts down just before getOrBuild; the builder's creator waits on it
    std::vector<MarkerCache::SharedValue> handles(threadCount);
    std::vector<std::jthread> workers;
    workers.reserve(threadCount);

    for (int i = 0; i < threadCount; ++i) {
        workers.emplace_back([&gate, &entered, &handles, &cache, &buildCount, key, i] {
            gate.arrive_and_wait(); // release all threads together to force the race
            entered.count_down();   // signal this thread has reached getOrBuild
            handles[i] = cache.getOrBuild(key, [&buildCount, &entered] {
                buildCount.fetch_add(1, std::memory_order_relaxed);
                entered.wait(); // hold the single build open until every caller has reached getOrBuild
                return Marker{7};
            });
        });
    }
    for (auto& w : workers) w.join(); // ensure all builds completed before asserting

    BOOST_CHECK_EQUAL(buildCount.load(), 1);   // built exactly once
    BOOST_CHECK_EQUAL(cache.size(), 1u);

    // Every caller received the very same value instance.
    for (int i = 0; i < threadCount; ++i) {
        BOOST_REQUIRE(handles[i].has());
        BOOST_CHECK(&handles[i].get() == &handles[0].get());
    }
    BOOST_CHECK_EQUAL(handles[0].get().value, 7);
}

// ---------------------------------------------------------------------------
// Keying
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(distinct_keys_build_separate_entries)
{
    MarkerCache cache;
    std::atomic buildCount{0};
    auto creator = [&buildCount] {
        buildCount.fetch_add(1, std::memory_order_relaxed);
        return Marker{1};
    };

    cache.getOrBuild(1, creator);
    cache.getOrBuild(2, creator);
    cache.getOrBuild(1, creator); // hits the first entry, no rebuild

    BOOST_CHECK_EQUAL(buildCount.load(), 2);
    BOOST_CHECK_EQUAL(cache.size(), 2u);
}

BOOST_AUTO_TEST_CASE(custom_key_type_and_hash_are_supported)
{
    ProcessInternalDataCache<CustomKey, Marker, CustomKeyHash> cache;
    std::atomic buildCount{0};
    auto creator = [&buildCount] {
        buildCount.fetch_add(1, std::memory_order_relaxed);
        return Marker{5};
    };

    auto h1 = cache.getOrBuild(CustomKey{1, 2}, creator);
    auto h2 = cache.getOrBuild(CustomKey{1, 2}, creator); // identical key -> shared
    auto h3 = cache.getOrBuild(CustomKey{2, 1}, creator); // different key -> separate

    BOOST_CHECK_EQUAL(buildCount.load(), 2);
    BOOST_CHECK_EQUAL(cache.size(), 2u);
    BOOST_CHECK(&h1.get() == &h2.get());
    BOOST_CHECK(&h1.get() != &h3.get());
}

// ---------------------------------------------------------------------------
// Concurrent reuse of a shared cached value
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(concurrent_reuse_of_shared_value_is_race_free)
{
    MarkerCache cache;
    constexpr int key = 100;

    auto handle = cache.getOrBuild(key, [] { return Marker{7}; });
    const Marker& shared = handle.get();

    constexpr int threadCount = 16;
    std::atomic okCount{0};
    std::latch gate(threadCount);
    std::vector<std::jthread> workers;
    workers.reserve(threadCount);

    for (int i = 0; i < threadCount; ++i) {
        workers.emplace_back([&gate, &cache, &shared, &okCount, key] {
            gate.arrive_and_wait();
            auto localHandle = cache.getOrBuild(key, [] { return Marker{-1}; });
            if (localHandle.has() && &localHandle.get() == &shared && localHandle.get().value == 7) {
                okCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& w : workers) w.join(); // ensure all readers completed before asserting

    BOOST_CHECK_EQUAL(okCount.load(), threadCount);
    BOOST_CHECK_EQUAL(cache.size(), 1u);
}

// ---------------------------------------------------------------------------
// Failure handling
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(failed_build_is_not_cached_and_is_retryable)
{
    MarkerCache cache;
    constexpr int key = 5;
    std::atomic attempts{0};

    BOOST_CHECK_THROW(
        cache.getOrBuild(key, [&attempts]() -> Marker {
            attempts.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("build failed");
        }),
        std::runtime_error);

    BOOST_CHECK_EQUAL(cache.size(), 0u); // poisoned entry removed

    auto handle = cache.getOrBuild(key, [&attempts] {
        attempts.fetch_add(1, std::memory_order_relaxed);
        return Marker{3};
    });

    BOOST_REQUIRE(handle.has());
    BOOST_CHECK_EQUAL(handle.get().value, 3);
    BOOST_CHECK_EQUAL(attempts.load(), 2); // first failed, second succeeded
    BOOST_CHECK_EQUAL(cache.size(), 1u);
}

// // ---------------------------------------------------------------------------
// // LRU capacity / eviction
// // ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(capacity_evicts_least_recently_used)
{
    MarkerCache cache(4); // soft capacity of 4
    std::atomic buildCount{0};
    auto make = [&buildCount](int v) {
        return [&buildCount, v] {
            buildCount.fetch_add(1, std::memory_order_relaxed);
            return Marker{v};
        };
    };

    // Build entries 1, 2; then hit on 1 (promotes 1 to most recent).
    cache.getOrBuild(1, make(1)); // buildCount = 1
    cache.getOrBuild(2, make(2)); // buildCount = 2
    BOOST_CHECK_EQUAL(cache.size(), 2u);
    BOOST_CHECK_EQUAL(buildCount.load(), 2);

    cache.getOrBuild(1, make(1)); // HIT, no rebuild; 1 promoted to most recent
    BOOST_CHECK_EQUAL(buildCount.load(), 2);

    // Build entries 3, 4, 5. Exceeds capacity (4); triggers async eviction.
    // Last-access order before insert 5: [2, 1, 3, 4] (2 least recent, 4 most recent).
    // Insert 5, size becomes 5; evict 25% (1 entry): evict 2.
    // After eviction: [1, 3, 4, 5], size = 4
    cache.getOrBuild(3, make(3)); // buildCount = 3
    cache.getOrBuild(4, make(4)); // buildCount = 4
    cache.getOrBuild(5, make(5)); // buildCount = 5
    BOOST_CHECK_EQUAL(buildCount.load(), 5);

    cache.waitForPruning(); // wait for background prune to complete
    BOOST_CHECK_EQUAL(cache.size(), 4u);

    // Hit on 1, 3 (both still in cache; not rebuilt).
    cache.getOrBuild(1, make(1)); // HIT
    cache.getOrBuild(3, make(3)); // HIT
    BOOST_CHECK_EQUAL(buildCount.load(), 5); // no new builds
}


BOOST_AUTO_TEST_CASE(evicted_value_stays_alive_while_handle_is_held)
{
    MarkerCache cache(4); // soft capacity of 4
    std::atomic buildCount{0};
    auto h1 = cache.getOrBuild(1, [&buildCount] {
        buildCount.fetch_add(1, std::memory_order_relaxed);
        return Marker{11};
    });
    const Marker* p1 = &h1.get();
    BOOST_CHECK_EQUAL(buildCount.load(), 1);

    // Build entries 2, 3, 4, 5. After inserting 5, size exceeds the cap and the
    // least-recently-used entry (1, built first and never re-accessed) is evicted.
    cache.getOrBuild(2, [] { return Marker{22}; });
    cache.getOrBuild(3, [] { return Marker{33}; });
    cache.getOrBuild(4, [] { return Marker{44}; });
    cache.getOrBuild(5, [] { return Marker{55}; });
    cache.waitForPruning(); // wait for async eviction to complete
    BOOST_CHECK_EQUAL(cache.size(), 4u);

    // Entry 1 was evicted, but the caller's handle keeps the value alive.
    BOOST_CHECK(&h1.get() == p1);
    BOOST_CHECK_EQUAL(h1.get().value, 11);

    // Requesting 1 again rebuilds it, proving the cache entry was actually evicted.
    auto h1Again = cache.getOrBuild(1, [&buildCount] {
        buildCount.fetch_add(1, std::memory_order_relaxed);
        return Marker{12};
    });
    BOOST_CHECK_EQUAL(buildCount.load(), 2);
    BOOST_CHECK_EQUAL(h1Again.get().value, 12);
    BOOST_CHECK(&h1Again.get() != &h1.get());
}

BOOST_AUTO_TEST_CASE(default_cache_uses_large_default_capacity)
{
    MarkerCache cache; // no capacity argument -> large default cap
    for (int i = 0; i < 100; ++i) {
        cache.getOrBuild(i, [i] { return Marker{i}; });
    }
    BOOST_CHECK_EQUAL(cache.size(), 100u); // well under the default cap, nothing evicted
}

// A single-flight build for one key must not block builds for other keys:
// while key 1 is in-flight, key 2 can still be built and returned.
BOOST_AUTO_TEST_CASE(in_flight_build_does_not_block_other_keys)
{
    MarkerCache cache;

    std::latch key1Building(1); // released once key1's build has started
    std::latch releaseKey1(1);  // key1's build blocks here so it stays in-flight

    std::jthread worker([&cache, &key1Building, &releaseKey1] {
        cache.getOrBuild(1, [&key1Building, &releaseKey1] {
            key1Building.count_down();
            releaseKey1.wait();
            return Marker{1};
        });
    });

    key1Building.wait(); // ensure key1 is in-flight before building key2

    auto h2 = cache.getOrBuild(2, [] { return Marker{2}; });
    BOOST_CHECK_EQUAL(h2.get().value, 2);

    releaseKey1.count_down();
    worker.join();

    BOOST_CHECK_EQUAL(cache.size(), 2u);
}

#ifndef NDEBUG
// The debug guard turns a would-be self-deadlock into a clear exception.
BOOST_AUTO_TEST_CASE(reentrant_same_key_build_throws_in_debug)
{
    MarkerCache cache;
    BOOST_CHECK_THROW(
        cache.getOrBuild(1, [&cache] {
            cache.getOrBuild(1, [] { return Marker{2}; }); // same key, same thread
            return Marker{1};
        }),
        std::logic_error);

    // The failed build left no entry; a later request rebuilds cleanly.
    auto handle = cache.getOrBuild(1, [] { return Marker{7}; });
    BOOST_CHECK_EQUAL(handle.get().value, 7);
    BOOST_CHECK_EQUAL(cache.size(), 1u);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
