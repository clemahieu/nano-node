#pragma once

#include <optional>

namespace nano
{
class block_hash;
class ledger;
class qualified_root;
}
namespace nano::store
{
class transaction;
}

namespace nano
{
class ledger_set_confirmed
{
public:
	ledger_set_confirmed (nano::ledger const & ledger);

	std::optional<nano::uint128_t> balance (store::transaction const & transaction, nano::account const & account) const;
	std::optional<nano::uint128_t> balance (store::transaction const & transaction, nano::block_hash const & hash) const;
	bool exists (store::transaction const & transaction, nano::block_hash const & hash) const;
	bool exists_or_pruned (store::transaction const & transaction, nano::block_hash const & hash) const;
	std::optional<nano::account_info> get (store::transaction const & transaction, nano::account const & account) const;
	std::shared_ptr<nano::block> get (store::transaction const & transaction, nano::block_hash const & hash) const;
	std::optional<nano::pending_info> get (store::transaction const & transaction, nano::pending_key const & key) const;
	nano::block_hash head (store::transaction const & transaction, nano::account const & account) const;
	uint64_t height (store::transaction const & transaction, nano::account const & account) const;
	std::optional<nano::block_hash> successor (store::transaction const & transaction, nano::qualified_root const & root) const;

private:
	nano::ledger const & ledger;
}; // class ledger_set_confirmed
} // namespace nano
