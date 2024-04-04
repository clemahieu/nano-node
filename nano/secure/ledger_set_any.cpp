#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/account.hpp>
#include <nano/store/component.hpp>
#include <nano/store/pending.hpp>
#include <nano/store/pruned.hpp>

nano::ledger_set_any::ledger_set_any (nano::ledger const & ledger) :
	ledger{ ledger }
{
}

std::optional<nano::account> nano::ledger_set_any::account (store::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block_l = get (transaction, hash);
	if (!block_l)
	{
		return std::nullopt;
	}
	return block_l->account ();
}

std::optional<nano::uint128_t> nano::ledger_set_any::amount (store::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block_l = get (transaction, hash);
	if (!block_l)
	{
		return std::nullopt;
	}
	auto block_balance = block_l->balance ();
	if (block_l->previous ().is_zero ())
	{
		return block_balance.number ();
	}
	auto previous_balance = balance (transaction, block_l->previous ());
	if (!previous_balance)
	{
		return std::nullopt;
	}
	return block_balance > previous_balance.value () ? block_balance.number () - previous_balance.value () : previous_balance.value () - block_balance.number ();
}

std::optional<nano::uint128_t> nano::ledger_set_any::balance (store::transaction const & transaction, nano::account const & account_a) const
{
	auto block = get (transaction, ledger.any.head (transaction, account_a));
	if (!block)
	{
		return std::nullopt;
	}
	return block->balance ().number ();
}

// Balance for account containing hash
std::optional<nano::uint128_t> nano::ledger_set_any::balance (store::transaction const & transaction, nano::block_hash const & hash) const
{
	if (hash.is_zero ())
	{
		return std::nullopt;
	}
	auto block = get (transaction, hash);
	if (!block)
	{
		return std::nullopt;
	}
	return block->balance ().number ();
}

bool nano::ledger_set_any::exists (store::transaction const & transaction, nano::block_hash const & hash) const
{
	return ledger.store.block.exists (transaction, hash);
}

bool nano::ledger_set_any::exists_or_pruned (store::transaction const & transaction, nano::block_hash const & hash) const
{
	if (ledger.store.pruned.exists (transaction, hash))
	{
		return true;
	}
	return ledger.store.block.exists (transaction, hash);
}

std::optional<nano::account_info> nano::ledger_set_any::get (store::transaction const & transaction, nano::account const & account) const
{
	return ledger.store.account.get (transaction, account);
}

std::shared_ptr<nano::block> nano::ledger_set_any::get (store::transaction const & transaction, nano::block_hash const & hash) const
{
	return ledger.store.block.get (transaction, hash);
}

std::optional<nano::pending_info> nano::ledger_set_any::get (store::transaction const & transaction, nano::pending_key const & key) const
{
	return ledger.store.pending.get (transaction, key);
}

nano::block_hash nano::ledger_set_any::head (store::transaction const & transaction, nano::account const & account) const
{
	auto info = get (transaction, account);
	if (!info)
	{
		return 0;
	}
	return info.value ().head;
}

uint64_t nano::ledger_set_any::height (store::transaction const & transaction, nano::account const & account) const
{
	auto head_l = head (transaction, account);
	if (head_l.is_zero ())
	{
		return 0;
	}
	return get (transaction, head_l)->sideband ().height;
}

uint64_t nano::ledger_set_any::height (store::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block = get (transaction, hash);
	if (!block)
	{
		return 0;
	}
	return block->sideband ().height;
}

std::optional<nano::block_hash> nano::ledger_set_any::successor (store::transaction const & transaction, nano::block_hash const & hash) const
{
	return successor (transaction, { hash, hash });
}

std::optional<nano::block_hash> nano::ledger_set_any::successor (store::transaction const & transaction, nano::qualified_root const & root) const
{
	if (!root.previous ().is_zero ())
	{
		return ledger.store.block.successor (transaction, root.previous ());
	}
	else
	{
		auto info = get (transaction, root.root ().as_account ());
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
