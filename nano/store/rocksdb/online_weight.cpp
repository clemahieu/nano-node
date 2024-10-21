#include <nano/store/rocksdb/online_weight.hpp>
#include <nano/store/rocksdb/rocksdb.hpp>

nano::store::rocksdb::online_weight::online_weight (nano::store::rocksdb::component & store_a) :
	store{ store_a }
{
}

void nano::store::rocksdb::online_weight::put (store::write_transaction const & transaction, uint64_t time, nano::amount const & amount)
{
	auto status = store.put (transaction, tables::online_weight, time, amount);
	store.release_assert_success (status);
}

void nano::store::rocksdb::online_weight::del (store::write_transaction const & transaction, uint64_t time)
{
	auto status = store.del (transaction, tables::online_weight, time);
	store.release_assert_success (status);
}

auto nano::store::rocksdb::online_weight::begin (store::transaction const & transaction) const -> iterator
{
	return store.make_iterator<uint64_t, nano::amount> (transaction, tables::online_weight);
}

auto nano::store::rocksdb::online_weight::rbegin (store::transaction const & transaction) const -> iterator
{
	return store.make_iterator<uint64_t, nano::amount> (transaction, tables::online_weight, false);
}

auto nano::store::rocksdb::online_weight::end (store::transaction const & transaction_a) const -> iterator
{
	return iterator{ nullptr };
}

size_t nano::store::rocksdb::online_weight::count (store::transaction const & transaction) const
{
	return store.count (transaction, tables::online_weight);
}

void nano::store::rocksdb::online_weight::clear (store::write_transaction const & transaction)
{
	auto status = store.drop (transaction, tables::online_weight);
	store.release_assert_success (status);
}
