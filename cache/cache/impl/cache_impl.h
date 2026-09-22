// DO NOT INCLUDE DIRECTLY

namespace wf::mortgage::utility::containers {

inline
cache::cache(cache_options opts)
	: opts_(std::move(opts))
	, stripeRes_(opts.memPoolOptions_.value_or(std::pmr::pool_options{}), &stripeStatsRes_)
	, latch_(2)
	, sizePolicy_(size_restriction_prune_policy<key_type>::percentage{}, opts.removalPercentage_)
{
	//check if we need to preallocate stripes
	if (!opts_.dyanamicStripes_) {
		for (size_t i = 0; i < opts_.maxNumStripes_; ++i) {
			create_new_stripe(i);
		}
	}
	pruningThreads_.reserve(2);
	//create time-to-live pruning thread
	if (opts_.pruneInterval_) {
		pruningThreads_.emplace_back(&cache::ttl_pruning, this);
	}
	else {
		latch_.count_down();
	}
	//create size pruning thread
	if (opts_.maxNumElements_) {
		pruningThreads_.emplace_back(&cache::size_pruning, this);
	}
	else {
		latch_.count_down();
	}
	latch_.wait();
}

inline
cache::~cache()
{
	terminate_ = true;
	sizePolicyCond_.notify_all();
	ttlPolicyCond_.notify_all();
	for (auto&& t : pruningThreads_) {
		if (t.joinable()) t.join();
	}
}

template <typename T>
std::shared_ptr<const T>
cache::do_read(const key_type& k) const WF_NDEBUG_NOEXCEPT
{
	if (k == key_type{}) return {};
	//find the stripe and read from it
	size_t idx = std::hash<key_type>{}(k) % opts_.maxNumStripes_;
	std::shared_lock<std::shared_mutex> lock(stripeMutex_);
	auto it = stripes_.find(idx);
	if (it != stripes_.end()) {
		++stripeStats_.hits_;
		return it->second.read<T>(k);
	}
	++stripeStats_.misses_;
	return {};
}

inline
void cache::create_new_stripe(size_t idx) noexcept
{
	std::unique_lock<std::shared_mutex> lock(stripeMutex_);
	stripes_.emplace(idx, containers::detail::cache_stripe(
		opts_.memPoolOptions_,
		opts_.buildCoordinationPolicy_
	));
}

// cannot be noexcept since f invocation can throw
template <typename T>
std::shared_ptr<const T>
cache::do_read_or_insert(
	const key_type& k,
	const creator_func<T>& f,
	std::optional<time_to_live> ttl)
{
	if (k == key_type{}) return {};
	size_t idx = std::hash<key_type>{}(k) % opts_.maxNumStripes_;
start:
	{
		std::shared_lock<std::shared_mutex> lock(stripeMutex_);
		auto it = stripes_.find(idx);
		if (it != stripes_.end()) {
			std::shared_ptr<const T> ptr =
				it->second.read_or_insert<T>(k, f, ttl ? ttl : opts_.maxTimeToLive_);
			check_max_elements();
			return ptr;
		}
	}
	create_new_stripe(idx);
	goto start;
}

template <typename Policy>
size_t cache::do_prune(const Policy& policy) noexcept
{
	size_t num = 0;
	try {
		std::unique_lock<std::shared_mutex> lock(stripeMutex_);
		isPruning_.test_and_set();
		std::pmr::vector<
			std::invoke_result_t<decltype(&detail::cache_stripe::key_view), detail::cache_stripe>
		> stripeViews;
		stripeViews.reserve(stripes_.size());

		//collect prune entries
		for (auto it = stripes_.begin(); it != stripes_.end();) {
			if ((it->second.size() == 0) && opts_.dyanamicStripes_) {
				//delete empty stripe
				it = stripes_.erase(it);
				continue;
			}
			stripeViews.emplace_back(it->second.key_view());
			++it;
		}

		//prune everything
		auto pruned = policy.prune(&stripeRes_, std::views::join(stripeViews));
		std::for_each(pruned.begin(), pruned.end(), [this](const auto& p) {
			do_erase_unsafe(*p.pruneKey_);
		});
		num = pruned.size();
	}
	catch (...) {}
	isPruning_.clear();
	pruningCond_.notify_all();
	return num;
}

template <typename T>
std::shared_ptr<const std::decay_t<T>>
cache::do_write(
	const key_type& k,
	T&& value,
	std::optional<time_to_live> ttl) WF_NDEBUG_NOEXCEPT
{
	if (k == key_type{}) return {};
	size_t idx = std::hash<key_type>{}(k) % opts_.maxNumStripes_;
	// FIXME: could write as while (true) { ... } instead of using goto
start:
	{
		std::shared_lock<std::shared_mutex> lock(stripeMutex_);
		auto it = stripes_.find(idx);
		if (it != stripes_.end()) {
			std::shared_ptr<const T> ptr = it->second.write<T>(
				k,
				std::forward<T>(value),
				ttl ? ttl : opts_.maxTimeToLive_
			);
			check_max_elements();
			return ptr;
		}
	}
	create_new_stripe(idx);
	goto start;
}

inline
bool cache::do_erase(const key_type& k) noexcept
{
	if (k == key_type{}) return false;
	std::shared_lock<std::shared_mutex> lock(stripeMutex_);
	return do_erase_unsafe(k);
}

inline
bool cache::do_erase_unsafe(const key_type& k) noexcept
{
	size_t idx = std::hash<key_type>{}(k) % opts_.maxNumStripes_;
	auto it = stripes_.find(idx);
	if (it != stripes_.end()) {
		return it->second.erase(k);
	}
	++stripeStats_.misses_;
	return false;
}

inline
size_t cache::do_size() const noexcept
{
	std::shared_lock<std::shared_mutex> lock(stripeMutex_);
	return do_size_unsafe();
}

inline
size_t cache::do_size_unsafe() const noexcept
{
	size_t size = 0;
	for (const auto& [k, s] : stripes_) {
		size += s.size();
	}
	return size;
}

inline
void cache::do_clear() noexcept
{
	if (!opts_.dyanamicStripes_) {
		std::shared_lock<std::shared_mutex> lock(stripeMutex_);
		//clear stripe contents only
		for (auto& [k, s] : stripes_) {
			s.clear();
		}
	}
	else {
		std::unique_lock<std::shared_mutex> lock(stripeMutex_);
		//remove everything including stripes
		stripes_.clear();
	}
}

inline
cache_stats cache::do_stats(const key_type& k) const noexcept
{
	cache_stats stats;
	std::shared_lock<std::shared_mutex> lock(stripeMutex_);
	if (k == key_type{}) {
		//get global stats
		stats = stripeStats_;
		//clear hits since we use the numbers from the stripes
		stats.hits_ = 0;
		for (const auto& [_, stripe] : stripes_)
			stats += stripe.stats();
	}
	else {
		//get key stats
		size_t idx = std::hash<key_type>{}(k) % opts_.maxNumStripes_;
		auto it = stripes_.find(idx);
		if (it != stripes_.end()) {
			return it->second.stats(k);
		}
	}
	return stats;
}

inline
void cache::do_reset_stats(const key_type& k) noexcept
{
	if (k == key_type{}) {
		//reset global stats
		std::unique_lock<std::shared_mutex> lock(stripeMutex_);
		for (auto& [_, s] : stripes_)
			s.reset_stats();
		stripeStats_.reset();
	}
	else {
		//reset specific key stats
		std::shared_lock<std::shared_mutex> lock(stripeMutex_);
		size_t idx = std::hash<key_type>{}(k) % opts_.maxNumStripes_;
		auto it = stripes_.find(idx);
		if (it != stripes_.end()) {
			return it->second.reset_stats(k);
		}
	}
}

inline
void cache::ttl_pruning() noexcept
{
	latch_.count_down();
	while (1) {
		std::unique_lock<std::mutex> lock(ttlPolicyMutex_);
		ttlPolicyCond_.wait_for(lock, opts_.pruneInterval_.value());
		if (terminate_) break;
		do_prune(ttlPolicy_);
		stripeStats_.ttlPruneLastRun_ = std::chrono::system_clock::now();
		++stripeStats_.ttlPruneNumRuns_;
	}
}

inline
void cache::size_pruning() noexcept
{
	latch_.count_down();
	while (true) {
		size_t generation = 0;
		{
			std::unique_lock<std::mutex> lock(sizePolicyMutex_);
			sizePolicyCond_.wait(lock, [this]()->bool {
				return terminate_ || sizePruneRequested_ != sizePruneCompleted_;
			});
			if (terminate_) break;
			generation = sizePruneRequested_;
		}

		// Drain the cache back under its cap. A burst of concurrent inserts can
		// require more than one batched prune; stop if a pass makes no progress
		// (e.g. the removal percentage rounds to zero or every candidate is
		// still in-flight).
		while (opts_.maxNumElements_ && do_size() > *opts_.maxNumElements_) {
			const size_t before = do_size();
			do_prune(sizePolicy_);
			if (do_size() >= before) break;
		}

		{
			std::lock_guard<std::mutex> lock(sizePolicyMutex_);
			sizePruneCompleted_ = generation;
		}
		sizePolicyCond_.notify_all();
		stripeStats_.sizePruneLastRun_ = std::chrono::system_clock::now();
		++stripeStats_.sizePruneNumRuns_;
	}
}

inline
bool cache::do_is_prune_operation_running() const noexcept
{
	return isPruning_.test();
}

inline
void cache::do_wait_until_prune_finishes() const noexcept
{
	// Wait for any in-flight prune (size or TTL) to finish.
	{
		std::shared_lock<std::shared_mutex> lock(pruningMutex_);
		pruningCond_.wait(lock, [this]()->bool { return !isPruning_.test(); });
	}
	// Then wait until the asynchronous size pruner has acknowledged every
	// pending request (i.e. the cache has been drained back under its cap).
	std::unique_lock<std::mutex> lock(sizePolicyMutex_);
	sizePolicyCond_.wait(lock, [this]()->bool {
		return terminate_ || sizePruneRequested_ == sizePruneCompleted_;
	});
}

inline
void cache::check_max_elements() const noexcept
{
	if (opts_.maxNumElements_ && (do_size_unsafe() > opts_.maxNumElements_)) {
		{
			std::lock_guard<std::mutex> lock(sizePolicyMutex_);
			++sizePruneRequested_;
		}
		sizePolicyCond_.notify_all();
	}
}

}  // namespace wf::mortgage::utility::containers
