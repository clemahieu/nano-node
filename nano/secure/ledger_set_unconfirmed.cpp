#include <nano/lib/blocks.hpp>
#include <nano/secure/ledger_set_unconfirmed.hpp>

nano::ledger_set_unconfirmed::ledger_set_unconfirmed (nano::ledger & ledger) :
	ledger{ ledger }
{
}

auto nano::ledger_set_unconfirmed::account_begin (store::transaction const & transaction) const -> account_iterator
{
	return account_upper_bound (transaction, 0);
}

auto nano::ledger_set_unconfirmed::account_end () const -> account_iterator
{
	return account_iterator{};
}

// Returns the next receivable entry equal or greater than 'key'
auto nano::ledger_set_unconfirmed::account_lower_bound (store::transaction const & transaction, nano::account const & account) const -> account_iterator
{
	std::lock_guard lock{ mutex };
	auto existing = this->account.lower_bound (account);
	if (existing == this->account.end ())
	{
		return account_iterator{ transaction, *this, std::nullopt };
	}
	return account_iterator{ transaction, *this, *existing };
}

// Returns the next receivable entry for an account greater than 'account'
auto nano::ledger_set_unconfirmed::account_upper_bound (store::transaction const & transaction, nano::account const & account) const -> account_iterator
{
	std::lock_guard lock{ mutex };
	auto existing = this->account.upper_bound (account);
	if (existing == this->account.end ())
	{
		return account_iterator{ transaction, *this, std::nullopt };
	}
	return account_iterator{ transaction, *this, *existing };
}

bool nano::ledger_set_unconfirmed::receivable_any (nano::account const & account) const
{
	std::lock_guard lock{ mutex };
	nano::pending_key begin{ account, 0 };
	nano::pending_key end{ account.number () + 1, 0 };
	return receivable.lower_bound (begin) != receivable.lower_bound (end);
}

void nano::ledger_set_unconfirmed::weight_add (nano::account const & account, nano::amount const & amount, nano::amount const & base)
{
	std::lock_guard lock{ mutex };
	auto existing = weight.find (account);
	if (existing != weight.end ())
	{
		auto new_val = existing->second.number () + amount.number ();
		if (new_val != base.number ())
		{
			existing->second = new_val;
		}
		else
		{
			weight.erase (existing);
		}
	}
	else
	{
		weight[account] = base.number () + amount.number ();
	}
}

size_t nano::ledger_set_unconfirmed::block_size () const
{
	std::lock_guard lock{ mutex };
	return block.size ();
}

size_t nano::ledger_set_unconfirmed::account_size () const
{
	std::lock_guard lock{ mutex };
	return account.size () - accounts_updated;
}
