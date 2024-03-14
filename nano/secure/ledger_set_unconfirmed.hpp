#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/secure/account_info.hpp>
#include <nano/secure/account_iterator.hpp>
#include <nano/secure/block_delta.hpp>
#include <nano/secure/pending_info.hpp>

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace nano
{
class block;
}

namespace nano
{
class ledger_set_unconfirmed
{
public:
	using account_iterator = nano::account_iterator<ledger_set_unconfirmed>;

	ledger_set_unconfirmed (nano::ledger & ledger);

	account_iterator account_begin (store::transaction const & transaction) const;
	account_iterator account_end () const;
	// Returns the next receivable entry equal or greater than 'key'
	account_iterator account_lower_bound (store::transaction const & transaction, nano::account const & account) const;
	// Returns the next receivable entry for an account greater than 'account'
	account_iterator account_upper_bound (store::transaction const & transaction, nano::account const & account) const;
	bool receivable_any (nano::account const & account) const;
	void weight_add (nano::account const & account, nano::amount const & amount, nano::amount const & base);
	size_t account_size () const;
	size_t block_size () const;

	nano::ledger & ledger;
	std::unordered_map<nano::block_hash, block_delta> block;
	std::map<nano::account, nano::account_info> account;
	std::map<nano::pending_key, nano::pending_info> receivable;
	std::unordered_set<nano::pending_key> received;
	std::unordered_map<nano::block_hash, nano::block_hash> successor;
	std::unordered_map<nano::account, nano::amount> weight;
	uint64_t accounts_updated{ 0 };
	mutable std::recursive_mutex mutex;
};
}
