#pragma once

namespace wf::mortgage::utility::containers
{
	/**
	 * Controls how concurrent `read_or_insert` calls for the same key are
	 * coordinated while a cache entry is being created.
	 */
	enum class build_coordination_policy
	{
		/**
		 * Current cache behavior.
		 *
		 * Every thread that misses the cache invokes the creator function
		 * independently, outside of any lock. The first insertion for a key
		 * wins and subsequent insertions for the same key are discarded, but
		 * their creator functions have still been invoked.
		 */
		concurrent,

		/**
		 * Single-flight insertion.
		 *
		 * When multiple threads miss the cache for the same key, only one
		 * thread invokes the creator function. All other threads block until
		 * the value has been inserted and then read the inserted value. If the
		 * creator throws, the exception is propagated to all threads that
		 * joined the in-flight build.
		 */
		single_flight
	};
}
