#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/account.hpp>
#include <nano/store/component.hpp>
#include <nano/store/pending.hpp>
#include <nano/store/pruned.hpp>
#include <nano/store/rocksdb/unconfirmed_account.hpp>
#include <nano/store/rocksdb/unconfirmed_set.hpp>

nano::ledger_set_any::ledger_set_any (nano::ledger const & ledger) :
	ledger{ ledger }
{
}

std::optional<nano::amount> nano::ledger_set_any::account_balance (secure::transaction const & transaction, nano::account const & account_a) const
{
	auto block = block_get (transaction, account_head (transaction, account_a));
	if (!block)
	{
		return std::nullopt;
	}
	return block->balance ();
}

auto nano::ledger_set_any::account_begin (secure::transaction const & transaction) const -> account_iterator
{
	return account_lower_bound (transaction, 0);
}

auto nano::ledger_set_any::account_end () const -> account_iterator
{
	return account_iterator{};
}

std::optional<nano::account_info> nano::ledger_set_any::account_get (secure::transaction const & transaction, nano::account const & account) const
{
	auto unconfirmed = ledger.unconfirmed.account.get (transaction, account);
	if (unconfirmed.has_value ())
	{
		return unconfirmed.value ();
	}
	return ledger.store.account.get (transaction, account);
}

nano::block_hash nano::ledger_set_any::account_head (secure::transaction const & transaction, nano::account const & account) const
{
	auto account_info = ledger.unconfirmed.account.get (transaction, account);
	if (account_info.has_value ())
	{
		return account_info.value ().head;
	}
	auto info = account_get (transaction, account);
	if (!info)
	{
		return 0;
	}
	return info.value ().head;
}

uint64_t nano::ledger_set_any::account_height (secure::transaction const & transaction, nano::account const & account) const
{
	auto head_l = account_head (transaction, account);
	if (head_l.is_zero ())
	{
		return 0;
	}
	auto block = block_get (transaction, head_l);
	release_assert (block); // Head block must be in ledger
	return block->sideband ().height;
}

auto nano::ledger_set_any::account_lower_bound (secure::transaction const & transaction, nano::account const & account) const -> account_iterator
{
	auto disk = ledger.store.account.begin (transaction, account);
	if (disk == ledger.store.account.end ())
	{
		return account_iterator{};
	}
	return account_iterator{ transaction, *this, *disk };
}

auto nano::ledger_set_any::account_upper_bound (secure::transaction const & transaction, nano::account const & account) const -> account_iterator
{
	return account_lower_bound (transaction, account.number () + 1);
}

std::optional<nano::account> nano::ledger_set_any::block_account (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block_l = block_get (transaction, hash);
	if (!block_l)
	{
		return std::nullopt;
	}
	return block_l->account ();
}

std::optional<nano::amount> nano::ledger_set_any::block_amount (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block_l = block_get (transaction, hash);
	if (!block_l)
	{
		return std::nullopt;
	}
	return block_amount (transaction, block_l);
}

std::optional<nano::amount> nano::ledger_set_any::block_amount (secure::transaction const & transaction, std::shared_ptr<nano::block> const & block) const
{
	auto block_balance = block->balance ();
	if (block->previous ().is_zero ())
	{
		return block_balance.number ();
	}
	auto previous_balance = this->block_balance (transaction, block->previous ());
	if (!previous_balance)
	{
		return std::nullopt;
	}
	return block_balance > previous_balance.value () ? block_balance.number () - previous_balance.value ().number () : previous_balance.value ().number () - block_balance.number ();
}

std::optional<nano::amount> nano::ledger_set_any::block_balance (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	if (hash.is_zero ())
	{
		return std::nullopt;
	}
	auto block = block_get (transaction, hash);
	if (!block)
	{
		return std::nullopt;
	}
	return block->balance ();
}

bool nano::ledger_set_any::block_exists (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	if (ledger.unconfirmed.block.exists (transaction, hash))
	{
		return true;
	}
	return ledger.store.block.exists (transaction, hash);
}

bool nano::ledger_set_any::block_exists_or_pruned (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	if (ledger.unconfirmed.block.exists (transaction, hash))
	{
		return true;
	}
	if (ledger.store.pruned.exists (transaction, hash))
	{
		return true;
	}
	return ledger.store.block.exists (transaction, hash);
}

std::shared_ptr<nano::block> nano::ledger_set_any::block_get (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	auto unconfirmed = ledger.unconfirmed.block.get (transaction, hash);
	if (unconfirmed != nullptr)
	{
		return unconfirmed;
	}
	return ledger.store.block.get (transaction, hash);
}

uint64_t nano::ledger_set_any::block_height (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block = block_get (transaction, hash);
	if (!block)
	{
		return 0;
	}
	return block->sideband ().height;
}

std::optional<std::pair<nano::pending_key, nano::pending_info>> nano::ledger_set_any::receivable_lower_bound (secure::transaction const & transaction, nano::account const & account_a, nano::block_hash const & hash_a) const
{
	auto account = account_a;
	auto hash = hash_a;
	std::optional<std::pair<nano::pending_key, nano::pending_info>> result;
	do
	{
		auto unconfirmed = ledger.unconfirmed.receivable.lower_bound (transaction, account, hash);
		auto confirmed = ledger.store.pending.begin (transaction, { account, hash });
		if (confirmed == ledger.store.pending.end () && !unconfirmed.has_value ())
		{
			return std::nullopt;
		}
		else if (!unconfirmed.has_value () && confirmed != ledger.store.pending.end ())
		{
			result = *confirmed;
		}
		else if (unconfirmed.has_value () && confirmed == ledger.store.pending.end ())
		{
			result = unconfirmed.value ();
		}
		else if (unconfirmed.value ().first < confirmed->first)
		{
			result = unconfirmed.value ();
		}
		account = result.has_value () ? result.value ().first.account : account;
		hash = result.has_value () ? result.value ().first.hash.number () + 1 : hash;
	} while (result.has_value () && ledger.unconfirmed.received.exists (transaction, result.value ().first));
	return *result;
}

auto nano::ledger_set_any::receivable_end () const -> receivable_iterator
{
	return receivable_iterator{};
}

bool nano::ledger_set_any::receivable_exists (secure::transaction const & transaction, nano::account const & account) const
{
	if (ledger.unconfirmed.receivable_exists (transaction, account))
	{
		return true;
	}
	auto next = receivable_upper_bound (transaction, account, 0);
	return next != receivable_end ();
}

auto nano::ledger_set_any::receivable_upper_bound (secure::transaction const & transaction, nano::account const & account) const -> receivable_iterator
{
	return receivable_iterator{ transaction, *this, receivable_lower_bound (transaction, account.number () + 1, 0) };
}

auto nano::ledger_set_any::receivable_upper_bound (secure::transaction const & transaction, nano::account const & account, nano::block_hash const & hash) const -> receivable_iterator
{
	auto result = receivable_lower_bound (transaction, account, hash.number () + 1);
	if (!result || result.value ().first.account != account)
	{
		return receivable_iterator{ transaction, *this, std::nullopt };
	}
	return receivable_iterator{ transaction, *this, result };
}

std::optional<nano::block_hash> nano::ledger_set_any::block_successor (secure::transaction const & transaction, nano::block_hash const & hash) const
{
	return block_successor (transaction, { hash, hash });
}

std::optional<nano::block_hash> nano::ledger_set_any::block_successor (secure::transaction const & transaction, nano::qualified_root const & root) const
{
	auto unconfirmed = ledger.unconfirmed.successor.get (transaction, root.previous ());
	if (unconfirmed.has_value ())
	{
		return unconfirmed.value ();
	}
	if (!root.previous ().is_zero ())
	{
		return ledger.store.block.successor (transaction, root.previous ());
	}
	else
	{
		auto info = account_get (transaction, root.root ().as_account ());
		if (info)
		{
			return info->open_block;
		}
		else
		{
			return std::nullopt;
		}
	}
}

std::optional<nano::pending_info> nano::ledger_set_any::pending_get (secure::transaction const & transaction, nano::pending_key const & key) const
{
	if (ledger.unconfirmed.received.exists (transaction, key))
	{
		return std::nullopt;
	}
	auto info = ledger.unconfirmed.receivable.get (transaction, key);
	if (info.has_value ())
	{
		return info;
	}
	return ledger.store.pending.get (transaction, key);
}
