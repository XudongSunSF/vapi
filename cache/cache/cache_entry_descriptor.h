#ifndef WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_ENTRY_DESCRIPTOR_INTERFACE_H
#define WF_MORTGAGE_UTILITY_CONTAINERS_CACHE_ENTRY_DESCRIPTOR_INTERFACE_H

#include <mortgage/utility/namespaces.h>
#include <mortgage/utility/containers/cache/cache_options.h>

#include <chrono>
#include <optional>
#include <atomic>
#include <typeinfo>

namespace wf::mortgage::utility::containers
{
	struct cache_entry_descriptor
	{
		explicit cache_entry_descriptor(std::optional<time_to_live> ttl = {})
			: timeToLive_(ttl)
		{}

		cache_entry_descriptor(const cache_entry_descriptor& desc)
		{
			*this = desc;
		}

		cache_entry_descriptor(cache_entry_descriptor&& desc) noexcept
		{
			*this = desc;
		}

		cache_entry_descriptor& operator=(const cache_entry_descriptor& desc)
		{
			creationTime_ = desc.creationTime_;
			lastAccessTime_ = desc.lastAccessTime_.load();
			timeToLive_ = desc.timeToLive_;
#ifndef NDEBUG
			ti_ = desc.ti_;
#endif
			return *this;
		}

		cache_entry_descriptor& operator=(cache_entry_descriptor&& desc) noexcept
		{
			return *this = desc;
		}

		bool is_expired() const noexcept {
			return timeToLive_ 
				? std::chrono::system_clock::now() > (lastAccessTime_.load() + timeToLive_.value())
				: false;
		}

		void extend_life() noexcept {
			lastAccessTime_.store(std::chrono::system_clock::now());
		}

		void increment_hits() noexcept {
			++hits_;
		}

		void reset_hits() noexcept {
			hits_ = 0;
		}

#ifndef NDEBUG
		explicit cache_entry_descriptor(const std::type_info& ti, std::optional<time_to_live> ttl = {})
			: ti_(&ti)
			, timeToLive_(ttl)
		{}

		bool is_same_type(const std::type_info& other) const
		{
			return *ti_ == other;
		}

		const char* type_name() const
		{
			return ti_->name();
		}

		const std::type_info* ti_{ nullptr };
#endif

		//members
		std::chrono::system_clock::time_point creationTime_{ std::chrono::system_clock::now() };
		std::atomic<std::chrono::system_clock::time_point> lastAccessTime_{ creationTime_ };
		std::optional<time_to_live> timeToLive_; //if unset, no expiry
		std::atomic<size_t> hits_{ 0 };
	};

	inline
	bool operator<(const cache_entry_descriptor& lhs, const cache_entry_descriptor& rhs) {
		return lhs.lastAccessTime_.load() < rhs.lastAccessTime_.load();
	}

	inline
	bool operator==(const cache_entry_descriptor& lhs, const cache_entry_descriptor& rhs) {
		return lhs.lastAccessTime_.load() == rhs.lastAccessTime_.load();
	}
}

#endif
