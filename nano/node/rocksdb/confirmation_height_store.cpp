#include <nano/node/rocksdb/confirmation_height_store.hpp>

#include <nano/node/rocksdb/rocksdb.hpp>

nano::confirmation_height_store_rocksdb::confirmation_height_store_rocksdb (nano::rocksdb_store & store) :
	store{ store }
{
}

void nano::confirmation_height_store_rocksdb::put (nano::write_transaction const & transaction, nano::account const & account, nano::confirmation_height_info const & confirmation_height_info)
{
	auto status = store.put (transaction, tables::confirmation_height, account, confirmation_height_info);
	release_assert_success (store, status);
}

bool nano::confirmation_height_store_rocksdb::get (nano::transaction const & transaction, nano::account const & account, nano::confirmation_height_info & confirmation_height_info)
{
	nano::rocksdb_val value;
	auto status = store.get (transaction, tables::confirmation_height, account, value);
	release_assert (store.success (status) || store.not_found (status));
	bool result (true);
	if (store.success (status))
	{
		nano::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
		result = confirmation_height_info.deserialize (stream);
	}
	if (result)
	{
		confirmation_height_info.height = 0;
		confirmation_height_info.frontier = 0;
	}

	return result;
}

bool nano::confirmation_height_store_rocksdb::exists (nano::transaction const & transaction, nano::account const & account) const
{
	return store.exists (transaction, tables::confirmation_height, account);
}

void nano::confirmation_height_store_rocksdb::del (nano::write_transaction const & transaction, nano::account const & account)
{
	auto status = store.del (transaction, tables::confirmation_height, account);
	release_assert_success (store, status);
}

uint64_t nano::confirmation_height_store_rocksdb::count (nano::transaction const & transaction)
{
	return store.count (transaction, tables::confirmation_height);
}

void nano::confirmation_height_store_rocksdb::clear (nano::write_transaction const & transaction, nano::account const & account)
{
	del (transaction, account);
}

void nano::confirmation_height_store_rocksdb::clear (nano::write_transaction const & transaction)
{
	store.drop (transaction, nano::tables::confirmation_height);
}

nano::store_iterator<nano::account, nano::confirmation_height_info> nano::confirmation_height_store_rocksdb::begin (nano::transaction const & transaction, nano::account const & account) const
{
	return store.make_iterator<nano::account, nano::confirmation_height_info> (transaction, tables::confirmation_height, account);
}

nano::store_iterator<nano::account, nano::confirmation_height_info> nano::confirmation_height_store_rocksdb::begin (nano::transaction const & transaction) const
{
	return store.make_iterator<nano::account, nano::confirmation_height_info> (transaction, tables::confirmation_height);
}

nano::store_iterator<nano::account, nano::confirmation_height_info> nano::confirmation_height_store_rocksdb::end () const
{
	return nano::store_iterator<nano::account, nano::confirmation_height_info> (nullptr);
}

void nano::confirmation_height_store_rocksdb::for_each_par (std::function<void (nano::read_transaction const &, nano::store_iterator<nano::account, nano::confirmation_height_info>, nano::store_iterator<nano::account, nano::confirmation_height_info>)> const & action_a) const
{
	parallel_traversal<nano::uint256_t> (
	[&action_a, this] (nano::uint256_t const & start, nano::uint256_t const & end, bool const is_last) {
		auto transaction (this->store.tx_begin_read ());
		action_a (transaction, this->begin (transaction, start), !is_last ? this->begin (transaction, end) : this->end ());
	});
}
