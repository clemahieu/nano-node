#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_confirmed.hpp>
#include <nano/store/account.hpp>
#include <nano/store/component.hpp>
#include <nano/store/confirmation_height.hpp>
#include <nano/store/pending.hpp>
#include <nano/store/pruned.hpp>

nano::ledger_set_confirmed::ledger_set_confirmed (nano::ledger const & ledger) :
	ledger{ ledger }
{
}

std::optional<nano::amount> nano::ledger_set_confirmed::account_balance (store::transaction const & transaction, nano::account const & account_a) const
{
	auto block = block_get (transaction, account_head (transaction, account_a));
	if (!block)
	{
		return std::nullopt;
	}
	return block->balance ();
}

nano::block_hash nano::ledger_set_confirmed::account_head (store::transaction const & transaction, nano::account const & account) const
{
	auto info = ledger.store.confirmation_height.get (transaction, account);
	if (!info)
	{
		return 0;
	}
	return info.value ().frontier;
}

uint64_t nano::ledger_set_confirmed::account_height (store::transaction const & transaction, nano::account const & account) const
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

// Balance for account containing hash
std::optional<nano::amount> nano::ledger_set_confirmed::block_balance (store::transaction const & transaction, nano::block_hash const & hash) const
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

bool nano::ledger_set_confirmed::block_exists (store::transaction const & transaction, nano::block_hash const & hash) const
{
	return block_get (transaction, hash) != nullptr;
}

bool nano::ledger_set_confirmed::block_exists_or_pruned (store::transaction const & transaction, nano::block_hash const & hash) const
{
	if (ledger.store.pruned.exists (transaction, hash))
	{
		return true;
	}
	return block_exists (transaction, hash);
}

std::shared_ptr<nano::block> nano::ledger_set_confirmed::block_get (store::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block = ledger.store.block.get (transaction, hash);
	if (!block)
	{
		return nullptr;
	}
	auto info = ledger.store.confirmation_height.get (transaction, block->account ());
	if (!info)
	{
		return nullptr;
	}
	return block->sideband ().height <= info.value ().height ? block : nullptr;
}