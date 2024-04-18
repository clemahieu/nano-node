#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/observer_set.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace nano
{
class block;
class ledger;
}

namespace rocksdb
{
class ColumnFamilyHandle;
class DB;
class Snapshot;
}

namespace nano
{
/**
 * Set of blocks to be durably confirmed
 */
class confirming_set final
{
	friend class confirmation_heightDeathTest_missing_block_Test;
	friend class confirmation_height_pruned_source_Test;

public:
	using snapshot_ptr = std::unique_ptr<::rocksdb::Snapshot const, std::function<void (::rocksdb::Snapshot const *)>>;

	confirming_set (nano::ledger & ledger, std::chrono::milliseconds batch_time = std::chrono::milliseconds{ 500 });
	~confirming_set ();

	// Adds a block to the set of blocks to be confirmed
	void add (nano::block_hash const & hash);
	void start ();
	void stop ();
	// Added blocks will remain in this set until after ledger has them marked as confirmed.
	bool exists (::rocksdb::Snapshot const * snapshot, nano::block_hash const & hash) const;
	std::size_t size (::rocksdb::Snapshot const * snapshot) const;
	snapshot_ptr snapshot () const;
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name) const;

	// Observers will be called once ledger has blocks marked as confirmed
	nano::observer_set<std::shared_ptr<nano::block>> cemented_observers;
	nano::observer_set<nano::block_hash const &> block_already_cemented_observers;

private:
	void run ();
	void swap ();
	nano::ledger & ledger;
	std::chrono::milliseconds batch_time;
	std::unique_ptr<::rocksdb::DB> db;
	bool dirty{ false };
	std::unique_ptr<::rocksdb::ColumnFamilyHandle> front;
	std::unique_ptr<::rocksdb::ColumnFamilyHandle> back;
	bool stopped{ false };
	mutable std::mutex mutex;
	std::condition_variable condition;
	std::thread thread;
};
}
