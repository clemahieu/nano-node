#include <nano/lib/thread_roles.hpp>
#include <nano/secure/confirming_set.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/utility.hpp>
#include <nano/store/component.hpp>
#include <nano/store/write_queue.hpp>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

nano::confirming_set::confirming_set (nano::ledger & ledger, std::chrono::milliseconds batch_time) :
	ledger{ ledger },
	batch_time{ batch_time }
{
	rocksdb::Options options;

	// Set up the database options
	options.create_if_missing = true;
	options.compression = rocksdb::kNoCompression; // Disable compression
	options.allow_mmap_reads = true;

	// Configure PlainTable specific options for fixed-size keys
	rocksdb::PlainTableOptions plain_table_options;
	plain_table_options.user_key_len = 8; // Set key length to 8 bytes
	plain_table_options.bloom_bits_per_key = 10; // Good default for Bloom filters
	plain_table_options.hash_table_ratio = 0.75;

	// Enable PlainTable format
	rocksdb::TableFactory * plain_table_factory = rocksdb::NewPlainTableFactory (plain_table_options);
	options.table_factory.reset (plain_table_factory);

	std::filesystem::path db_path = nano::unique_path () / "confirming_set";
	std::string db_path_str = db_path.string ();
	rocksdb::DB * db_l;
	auto front_status = rocksdb::DB::Open (options, db_path_str, &db_l);
	release_assert (front_status.ok (), "Unable to open front database");
	db.reset (db_l);
	rocksdb::ColumnFamilyHandle * front_l;
	auto front_column_status = db->CreateColumnFamily (rocksdb::ColumnFamilyOptions{}, "left", &front_l);
	release_assert (front_column_status.ok ());
	front.reset (front_l);
	rocksdb::ColumnFamilyHandle * back_l;
	auto back_column_status = db->CreateColumnFamily (rocksdb::ColumnFamilyOptions{}, "right", &back_l);
	release_assert (back_column_status.ok ());
	back.reset (back_l);
}

nano::confirming_set::~confirming_set ()
{
	debug_assert (!thread.joinable ());
}

void nano::confirming_set::add (nano::block_hash const & hash)
{
	std::lock_guard lock{ mutex };
	rocksdb::Slice key{ reinterpret_cast<const char *> (&hash), sizeof (hash) };
	rocksdb::Slice value{ "" };
	auto status = db->Put (rocksdb::WriteOptions (), front.get (), key, value);
	release_assert (status.ok (), "Failed to put set item");
	dirty = true;
	condition.notify_all ();
}

void nano::confirming_set::start ()
{
	thread = std::thread{ [this] () { run (); } };
}

void nano::confirming_set::stop ()
{
	{
		std::lock_guard lock{ mutex };
		stopped = true;
		condition.notify_all ();
	}
	if (thread.joinable ())
	{
		thread.join ();
	}
	std::cerr << '\0';
}

bool nano::confirming_set::exists (rocksdb::Snapshot const * snapshot, nano::block_hash const & hash) const
{
	std::lock_guard lock{ mutex };
	std::string junk;
	rocksdb::Slice slice{ reinterpret_cast<const char *> (&hash), sizeof (hash) };
	rocksdb::ReadOptions options;
	options.snapshot = snapshot;
	auto front_status = db->Get (options, front.get (), slice, &junk);
	debug_assert (front_status.ok () || front_status.IsNotFound ());
	if (front_status.ok ())
	{
		return true;
	}
	auto back_status = db->Get (rocksdb::ReadOptions{}, back.get (), slice, &junk);
	debug_assert (back_status.ok () || back_status.IsNotFound ());
	return back_status.ok ();
}

std::size_t nano::confirming_set::size (rocksdb::Snapshot const * snapshot) const
{
	uint64_t front_size;
	db->GetIntProperty (front.get (), "rocksdb.estimate-num-keys", &front_size);
	uint64_t back_size;
	db->GetIntProperty (back.get (), "rocksdb.estimate-num-keys", &back_size);
	return front_size + back_size;
}

rocksdb::Snapshot const * nano::confirming_set::snapshot () const
{
	return db->GetSnapshot();
}

void nano::confirming_set::run ()
{
	nano::thread_role::set (nano::thread_role::name::confirmation_height_processing);
	std::unique_lock lock{ mutex };
	while (!stopped)
	{
		condition.wait (lock, [&] () { return stopped || dirty; });
		// Loop if there are items to process
		if (!stopped && dirty)
		{
			swap ();
			std::deque<std::shared_ptr<nano::block>> cemented;
			std::deque<nano::block_hash> already;
			auto i = db->NewIterator (rocksdb::ReadOptions{}, back.get ());
			i->SeekToFirst ();
			while (!stopped && i->Valid ())
			{
				lock.unlock (); // Waiting for db write is potentially slow
				auto guard = ledger.store.write_queue.wait (nano::store::writer::confirmation_height);
				auto tx = ledger.tx_begin_write ({ nano::tables::confirmation_height });
				lock.lock ();
				// Process items in the back buffer within a single transaction for a limited amount of time
				for (auto timeout = std::chrono::steady_clock::now () + batch_time; !stopped && std::chrono::steady_clock::now () < timeout && i->Valid (); i->Next ())
				{
					auto slice = i->key ();
					debug_assert (slice.size () == sizeof (nano::block_hash));
					auto item = reinterpret_cast<nano::block_hash const *> (slice.data ());
					lock.unlock ();
					auto added = ledger.confirm (tx, *item);
					if (!added.empty ())
					{
						// Confirming this block may implicitly confirm more
						cemented.insert (cemented.end (), added.begin (), added.end ());
					}
					else
					{
						already.push_back (*item);
					}
					lock.lock ();
				}
			}
			lock.unlock ();
			for (auto const & i : cemented)
			{
				cemented_observers.notify (i);
			}
			for (auto const & i : already)
			{
				block_already_cemented_observers.notify (i);
			}
			lock.lock ();
			auto name = back->GetName ();
			auto status = db->DropColumnFamily (back.get ());
			debug_assert (status.ok ());
			back.reset ();
			rocksdb::ColumnFamilyHandle * back_l;
			auto back_column_status = db->CreateColumnFamily (rocksdb::ColumnFamilyOptions{}, name, &back_l);
			release_assert (back_column_status.ok ());
			back.reset (back_l);
		}
	}
	std::cerr << '\0';
}

void nano::confirming_set::swap ()
{
	std::swap (front, back);
	dirty = false;
}

std::unique_ptr<nano::container_info_component> nano::confirming_set::collect_container_info (std::string const & name) const
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "size", size (snapshot ()), sizeof (nano::block_hash) }));
	return composite;
}
