#pragma once
#include <nano/lib/numbers.hpp>
#include <nano/secure/account_info.hpp>

#include <optional>
#include <utility>

namespace nano::store
{
class transaction;
}

namespace nano
{
// This class iterates account entries
template <typename T>
class account_iterator
{
public:
	account_iterator ();
	account_iterator (store::transaction const & transaction, T const & view, std::optional<std::pair<nano::account, nano::account_info>> const & item);
	bool operator== (account_iterator const & other) const;
	bool operator!= (account_iterator const & other) const;
	// Advances to the next receivable entry for the same account
	account_iterator & operator++ ();
	std::pair<nano::account, nano::account_info> const & operator* () const;
	std::pair<nano::account, nano::account_info> const * operator->() const;

private:
	store::transaction const * transaction;
	T const * view{ nullptr };
	std::optional<std::pair<nano::account, nano::account_info>> item;
};
}
