#include <nano/node/lmdb/account_store.hpp>

#include <nano/node/lmdb/lmdb.hpp>

nano::account_store_mdb::account_store_mdb (nano::mdb_store & store_a) :
	store (store_a){};

void nano::account_store_mdb::put (nano::write_transaction const & transaction, nano::account const & account, nano::account_info const & info)
{
	auto status = store.put (transaction, tables::accounts, account, info);
	store.release_assert_success (status);
}

bool nano::account_store_mdb::get (nano::transaction const & transaction, nano::account const & account, nano::account_info & info)
{
	nano::mdb_val value;
	auto status1 (store.get (transaction, tables::accounts, account, value));
	release_assert (store.success (status1) || store.not_found (status1));
	bool result (true);
	if (store.success (status1))
	{
		nano::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
		result = info.deserialize (stream);
	}
	return result;
}

void nano::account_store_mdb::del (nano::write_transaction const & transaction_a, nano::account const & account_a)
{
	auto status = store.del (transaction_a, tables::accounts, account_a);
	release_assert_success (store, status);
}

bool nano::account_store_mdb::exists (nano::transaction const & transaction_a, nano::account const & account_a)
{
	auto iterator (begin (transaction_a, account_a));
	return iterator != end () && nano::account (iterator->first) == account_a;
}

size_t nano::account_store_mdb::count (nano::transaction const & transaction_a)
{
	return store.count (transaction_a, tables::accounts);
}

nano::store_iterator<nano::account, nano::account_info> nano::account_store_mdb::begin (nano::transaction const & transaction, nano::account const & account) const
{
	return static_cast<nano::store_partial<MDB_val, mdb_store> &> (store).template make_iterator<nano::account, nano::account_info> (transaction, tables::accounts, account);
}

nano::store_iterator<nano::account, nano::account_info> nano::account_store_mdb::begin (nano::transaction const & transaction) const
{
	return static_cast<nano::store_partial<MDB_val, mdb_store> &> (store).template make_iterator<nano::account, nano::account_info> (transaction, tables::accounts);
}

nano::store_iterator<nano::account, nano::account_info> nano::account_store_mdb::rbegin (nano::transaction const & transaction_a) const
{
	return store.template make_iterator<nano::account, nano::account_info> (transaction_a, tables::accounts, false);
}

nano::store_iterator<nano::account, nano::account_info> nano::account_store_mdb::end () const
{
	return nano::store_iterator<nano::account, nano::account_info> (nullptr);
}

void nano::account_store_mdb::for_each_par (std::function<void (nano::read_transaction const &, nano::store_iterator<nano::account, nano::account_info>, nano::store_iterator<nano::account, nano::account_info>)> const & action_a) const
{
	parallel_traversal<nano::uint256_t> (
	[&action_a, this] (nano::uint256_t const & start, nano::uint256_t const & end, bool const is_last) {
		auto transaction (this->store.tx_begin_read ());
		action_a (transaction, this->begin (transaction, start), !is_last ? this->begin (transaction, end) : this->end ());
	});
}
