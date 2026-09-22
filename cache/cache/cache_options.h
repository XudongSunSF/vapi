#ifndef WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_OPTIONS_H
#define WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_OPTIONS_H

#include <mortgage/utility/namespaces.h>
#include <mortgage/utility/containers/hash.h>
#include <mortgage/utility/containers/cache/build_coordination_policy.h>
#include <memory_resource>
#include <array>
#include <chrono>
#include <optional>

namespace wf::mortgage::utility::containers
{
	using cache_key = std::array<std::string, 2>; //{stripe-id, entry-id}
	inline const cache_key null_cache_key;
	using time_to_live = std::chrono::milliseconds;

	struct cache_options
	{
		std::optional<std::pmr::pool_options> memPoolOptions_;

		//Once max is reached oldest elements are automatically purged
		std::optional<size_t> maxNumElements_; 
		
		//If set, cache entries cannot live past ttl. 
		std::optional<time_to_live> maxTimeToLive_; 

		//If set, controls the interval at which the prune policy will run
		std::optional<time_to_live> pruneInterval_; 

		//If cache reaches maxNumElements, reduce the cache size by 25%.
		double removalPercentage_{ 25 };  

		//Max number of stripes the cache will create. The higher the number the 
		//less collisions there will be, providing increased performance.
		//To debug cache contents set this number to 1.
		size_t maxNumStripes_{ 50 }; 
		                             
		//If set to false, the cache will preallocate stripes on startup and will
		//never delete them. If set to true, the cache will dynamically allocate 
		//stripes up to maxNumStripes_ and will de-allocate them when empty. Dynamic
		//stripes reduces cache size but slow down access due to additional locking
		//at the stripe level.
		bool dyanamicStripes_{ false };

		//Controls how concurrent read_or_insert calls that miss the cache for
		//the same key are coordinated while the entry is being created.
		build_coordination_policy buildCoordinationPolicy_{
			build_coordination_policy::concurrent
		};
	};
}

#endif
