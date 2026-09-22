#ifndef WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_STATS_H
#define WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_STATS_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>

#include <mortgage/utility/namespaces.h>

namespace wf::mortgage::utility::containers {

/**
 * Cache statistics type.
 *
 * @note Size-based pruning may not yet be actively used.
 *
 * @param misses_ Number of cache misses
 * @param hits_ Number of cache hits
 * @param ttlPruneNumRuns_ Number of executed pruning runs based on time to live
 * @param ttlPruneLastRun_ Time point of last executed TTL-based pruning run
 * @param sizePruneNumRuns_ Number of executed pruning runs based on size
 * @param sizePruneLastRun_ Time point of last executed size-based pruning run
 */
struct cache_stats
{
	cache_stats() = default;

	cache_stats(const cache_stats& other) noexcept;

	cache_stats& operator=(const cache_stats& other) noexcept;

	void operator+=(const cache_stats& other) noexcept;

	bool operator==(const cache_stats& other) const noexcept;

	void reset() noexcept;

	//members
	std::atomic<size_t> misses_{ 0 };
	std::atomic<size_t> hits_{ 0 };
	size_t ttlPruneNumRuns_{ 0 };
	// TODO: according to C++11 standard time_point ctor and several operator
	// members are not noexcept; should we really specify noexcept?
	std::chrono::system_clock::time_point ttlPruneLastRun_;
	size_t sizePruneNumRuns_{ 0 };
	std::chrono::system_clock::time_point sizePruneLastRun_;
};

std::ostream& operator<<(std::ostream&, const cache_stats&);

std::string to_string(const cache_stats&);

}  // namespace wf::mortgage::utility::containers

#endif  // WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_STATS_H
