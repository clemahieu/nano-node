#include <nano/lib/blocks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/lib/work.hpp>
#include <nano/node/make_store.hpp>
#include <nano/secure/block_check_context.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_confirmed.hpp>
#include <nano/secure/ledger_set_unconfirmed.hpp>
#include <nano/secure/rep_weights.hpp>
#include <nano/store/account.hpp>
#include <nano/store/block.hpp>
#include <nano/store/component.hpp>
#include <nano/store/confirmation_height.hpp>
#include <nano/store/final.hpp>
#include <nano/store/online_weight.hpp>
#include <nano/store/peer.hpp>
#include <nano/store/pending.hpp>
#include <nano/store/pruned.hpp>
#include <nano/store/rep_weight.hpp>
#include <nano/store/version.hpp>

#include <stack>

#include <cryptopp/words.h>

namespace
{
/**
 * Determine the representative for this block
 */
class representative_visitor final : public nano::block_visitor
{
public:
	representative_visitor (nano::store::transaction const & transaction_a, nano::ledger & ledger);
	~representative_visitor () = default;
	void compute (nano::block_hash const & hash_a);
	void send_block (nano::send_block const & block_a) override;
	void receive_block (nano::receive_block const & block_a) override;
	void open_block (nano::open_block const & block_a) override;
	void change_block (nano::change_block const & block_a) override;
	void state_block (nano::state_block const & block_a) override;
	nano::store::transaction const & transaction;
	nano::ledger & ledger;
	nano::block_hash current;
	nano::block_hash result;
};

representative_visitor::representative_visitor (nano::store::transaction const & transaction_a, nano::ledger & ledger) :
	transaction{ transaction_a },
	ledger{ ledger },
	result{ 0 }
{
}

void representative_visitor::compute (nano::block_hash const & hash_a)
{
	current = hash_a;
	while (result.is_zero ())
	{
		auto block_l = ledger.any.get (transaction, current);
		debug_assert (block_l != nullptr);
		block_l->visit (*this);
	}
}

void representative_visitor::send_block (nano::send_block const & block_a)
{
	current = block_a.previous ();
}

void representative_visitor::receive_block (nano::receive_block const & block_a)
{
	current = block_a.previous ();
}

void representative_visitor::open_block (nano::open_block const & block_a)
{
	result = block_a.hash ();
}

void representative_visitor::change_block (nano::change_block const & block_a)
{
	result = block_a.hash ();
}

void representative_visitor::state_block (nano::state_block const & block_a)
{
	result = block_a.hash ();
}
} // namespace

nano::ledger::ledger (nano::store::component & store_a, nano::stats & stat_a, nano::ledger_constants & constants, nano::generate_cache_flags const & generate_cache_flags_a, nano::uint128_t min_rep_weight_a) :
	constants{ constants },
	store{ store_a },
	cache{ store_a.rep_weight, min_rep_weight_a },
	stats{ stat_a },
	check_bootstrap_weights{ true },
	any_impl{ std::make_unique<ledger_set_any> (*this) },
	confirmed_impl{ std::make_unique<ledger_set_confirmed> (*this) },
	unconfirmed_impl{ std::make_unique<ledger_set_unconfirmed> (*this) },
	any{ *any_impl },
	confirmed{ *confirmed_impl },
	unconfirmed{ *unconfirmed_impl }
{
	if (!store.init_error ())
	{
		initialize (generate_cache_flags_a);
	}
}

nano::ledger::~ledger ()
{
}

void nano::ledger::initialize (nano::generate_cache_flags const & generate_cache_flags_a)
{
	if (generate_cache_flags_a.reps || generate_cache_flags_a.account_count || generate_cache_flags_a.block_count || generate_cache_flags_a.cemented_count)
	{
		store.account.for_each_par (
		[this] (store::read_transaction const & /*unused*/, store::iterator<nano::account, nano::account_info> i, store::iterator<nano::account, nano::account_info> n) {
			uint64_t block_count_l{ 0 };
			uint64_t account_count_l{ 0 };
			for (; i != n; ++i)
			{
				nano::account_info const & info (i->second);
				block_count_l += info.block_count;
				++account_count_l;
			}
			this->cache.block_count += block_count_l;
			this->cache.cemented_count += block_count_l;
			this->cache.account_count += account_count_l;
		});

		store.rep_weight.for_each_par (
		[this] (store::read_transaction const & /*unused*/, store::iterator<nano::account, nano::uint128_union> i, store::iterator<nano::account, nano::uint128_union> n) {
			nano::rep_weights rep_weights_l{ this->store.rep_weight };
			for (; i != n; ++i)
			{
				rep_weights_l.representation_put (i->first, i->second.number ());
			}
			this->cache.rep_weights.copy_from (rep_weights_l);
		});
	}

	auto transaction (store.tx_begin_read ());
	cache.pruned_count = store.pruned.count (transaction);
}

nano::uint128_t nano::ledger::account_receivable (store::transaction const & transaction_a, nano::account const & account_a, bool only_confirmed_a)
{
	nano::uint128_t result{ 0 };
	for (auto i = any.receivable_upper_bound (transaction_a, account_a, 0), n = any.receivable_end (); i != n; ++i)
	{
		auto const & [key, info] = *i;
		if (!only_confirmed_a || confirmed.exists_or_pruned (transaction_a, key.hash))
		{
			result += info.amount.number ();
		}
	}
	return result;
}

std::deque<std::shared_ptr<nano::block>> nano::ledger::confirm (nano::store::write_transaction const & transaction, nano::block_hash const & hash)
{
	std::lock_guard lock{ unconfirmed.mutex };
	std::deque<std::shared_ptr<nano::block>> result;
	std::stack<nano::block_hash> stack;
	stack.push (hash);
	while (!stack.empty ())
	{
		auto hash = stack.top ();
		auto block = any.get (transaction, hash);
		release_assert (block);
		auto dependents = dependent_blocks (transaction, *block);
		for (auto const & dependent : dependents)
		{
			if (!dependent.is_zero () && !confirmed.exists_or_pruned (transaction, dependent))
			{
				stack.push (dependent);
			}
		}
		if (stack.top () == hash)
		{
			stack.pop ();
			if (!confirmed.exists_or_pruned (transaction, hash))
			{
				result.push_back (block);
				auto existing = unconfirmed.block.find (hash);
				release_assert (existing != unconfirmed.block.end ());
				auto delta = std::move (existing->second);
				confirm (transaction, delta);
			}
		}
		else
		{
			// unconfirmed dependencies were added
		}
	}
	return result;
}

nano::block_status nano::ledger::process (store::write_transaction const & transaction_a, std::shared_ptr<nano::block> block_a)
{
	std::lock_guard lock{ unconfirmed.mutex };
	debug_assert (!constants.work.validate_entry (*block_a) || constants.genesis == nano::dev::genesis);
	nano::block_check_context ctx{ *this, block_a };
	auto code = ctx.check (transaction_a);
	if (code == nano::block_status::progress)
	{
		debug_assert (block_a->has_sideband ());
		track (transaction_a, ctx.delta.value ());
	}
	return code;
}

nano::block_hash nano::ledger::representative (store::transaction const & transaction_a, nano::block_hash const & hash_a)
{
	auto result (representative_calculated (transaction_a, hash_a));
	debug_assert (result.is_zero () || any.exists (transaction_a, result));
	return result;
}

nano::block_hash nano::ledger::representative_calculated (store::transaction const & transaction_a, nano::block_hash const & hash_a)
{
	representative_visitor visitor (transaction_a, *this);
	visitor.compute (hash_a);
	return visitor.result;
}

std::string nano::ledger::block_text (char const * hash_a)
{
	return block_text (nano::block_hash (hash_a));
}

std::string nano::ledger::block_text (nano::block_hash const & hash_a)
{
	std::string result;
	auto block_l = any.get (store.tx_begin_read (), hash_a);
	if (block_l != nullptr)
	{
		block_l->serialize_json (result);
	}
	return result;
}

std::pair<nano::block_hash, nano::block_hash> nano::ledger::hash_root_random (store::transaction const & transaction_a) const
{
	nano::block_hash hash (0);
	nano::root root (0);
	if (!pruning)
	{
		auto block (store.block.random (transaction_a));
		hash = block->hash ();
		root = block->root ();
	}
	else
	{
		uint64_t count (cache.block_count);
		auto region = nano::random_pool::generate_word64 (0, count - 1);
		// Pruned cache cannot guarantee that pruned blocks are already commited
		if (region < cache.pruned_count)
		{
			hash = store.pruned.random (transaction_a);
		}
		if (hash.is_zero ())
		{
			auto block (store.block.random (transaction_a));
			hash = block->hash ();
			root = block->root ();
		}
	}
	return std::make_pair (hash, root.as_block_hash ());
}

// Vote weight of an account
nano::uint128_t nano::ledger::weight (nano::account const & account_a) const
{
	if (check_bootstrap_weights.load ())
	{
		if (cache.block_count < bootstrap_weight_max_blocks)
		{
			auto weight = bootstrap_weights.find (account_a);
			if (weight != bootstrap_weights.end ())
			{
				return weight->second;
			}
		}
		else
		{
			check_bootstrap_weights = false;
		}
	}
	{
		std::lock_guard lock{ unconfirmed.mutex };
		if (auto it = unconfirmed.weight.find (account_a); it != unconfirmed.weight.end ())
		{
			return it->second.number ();
		}
	}
	return cache.rep_weights.representation_get (account_a);
}

nano::uint128_t nano::ledger::weight_exact (store::transaction const & txn_a, nano::account const & representative_a) const
{
	return store.rep_weight.get (txn_a, representative_a);
}

// Rollback blocks until `block_a' doesn't exist or it tries to penetrate the confirmation height
bool nano::ledger::rollback (store::write_transaction const & transaction, nano::block_hash const & target, std::vector<std::shared_ptr<nano::block>> & list_a)
{
	std::stack<nano::block_hash> stack;
	stack.push (target);
	while (!stack.empty ())
	{
		auto hash = stack.top ();
		auto block = any.get (transaction, hash);
		if (block != nullptr)
		{
			if (confirmed.exists_or_pruned (transaction, hash))
			{
				return true;
			}
			if (unconfirmed.successor.count (hash) != 0)
			{
				auto head = unconfirmed.account.at (block->account ()).head;
				debug_assert (head != block->hash ());
				stack.push (head);
			}
			if (block->is_send ())
			{
				auto destination = block->destination ();
				if (unconfirmed.received.count ({ destination, hash }) != 0)
				{
					auto head = unconfirmed.account.at (destination).head;
					stack.push (head);
				}
			}
			if (hash == stack.top ())
			{
				list_a.push_back (block);
				stack.pop ();
				rollback (transaction, *block);
			}
			else
			{
				// Rollback dependencies were added
			}
		}
		else
		{
			stack.pop ();
		}
	}
	return false;
}

void nano::ledger::rollback (store::write_transaction const & transaction, nano::block const & block)
{
	debug_assert (unconfirmed.account.at (block.account ()).head == block.hash ());
	auto existing = unconfirmed.block.find (unconfirmed.account.at (block.account ()).head);
	auto delta = std::move (existing->second);
	unconfirmed.block.erase (existing);
	if (delta.block->previous ().is_zero ())
	{
		stats.inc (nano::stat::type::rollback, nano::stat::detail::open);
		[[maybe_unused]] auto erased = unconfirmed.account.erase (block.account ());
		debug_assert (erased == 1);
	}
	else
	{
		[[maybe_unused]] auto erased = unconfirmed.successor.erase (delta.block->previous ());
		debug_assert (erased == 1);
		if (auto existing = unconfirmed.block.find (block.previous ()); existing != unconfirmed.block.end ())
		{
			unconfirmed.account[block.account ()] = unconfirmed.block.at (block.previous ()).head;
		}
		else
		{
			[[maybe_unused]] auto erased = unconfirmed.account.erase (block.account ());
			debug_assert (erased == 1);
			debug_assert (unconfirmed.accounts_updated > 0);
			--unconfirmed.accounts_updated;
		}
	}
	auto const & [receivable_key, receivable_info] = delta.receivable;
	if (receivable_key)
	{
		if (receivable_info)
		{
			// key + info = send
			[[maybe_unused]] auto erased = unconfirmed.receivable.erase (receivable_key.value ());
			debug_assert (erased == 1);
			stats.inc (nano::stat::type::rollback, nano::stat::detail::send);
		}
		else
		{
			// key + null = receive
			[[maybe_unused]] auto erased = unconfirmed.received.erase (receivable_key.value ());
			debug_assert (erased == 1);
			stats.inc (nano::stat::type::rollback, nano::stat::detail::receive);
		}
	}
	else
	{
		stats.inc (nano::stat::type::rollback, nano::stat::detail::change);
	}
	unconfirmed.weight_add (delta.head.representative, 0 - block.balance ().number (), weight_exact (transaction, delta.head.representative));
	if (delta.weight.first)
	{
		unconfirmed.weight_add (delta.weight.first.value (), delta.weight.second.value (), weight_exact (transaction, delta.weight.first.value ()));
	}
}

bool nano::ledger::rollback (store::write_transaction const & transaction, nano::block_hash const & hash)
{
	std::vector<std::shared_ptr<nano::block>> rollback_list;
	return rollback (transaction, hash, rollback_list);
}

// Return latest root for account, account number if there are no blocks for this account.
nano::root nano::ledger::latest_root (store::transaction const & transaction_a, nano::account const & account_a)
{
	auto info = any.get (transaction_a, account_a);
	if (!info)
	{
		return account_a;
	}
	else
	{
		return info->head;
	}
}

void nano::ledger::dump_account_chain (nano::account const & account_a, std::ostream & stream)
{
	auto transaction = store.tx_begin_read ();
	auto hash (any.head (transaction, account_a));
	while (!hash.is_zero ())
	{
		auto block_l = any.get (transaction, hash);
		debug_assert (block_l != nullptr);
		stream << hash.to_string () << std::endl;
		hash = block_l->previous ();
	}
}

bool nano::ledger::dependents_confirmed (store::transaction const & transaction_a, nano::block const & block_a) const
{
	auto dependencies (dependent_blocks (transaction_a, block_a));
	return std::all_of (dependencies.begin (), dependencies.end (), [this, &transaction_a] (nano::block_hash const & hash_a) {
		auto result (hash_a.is_zero ());
		if (!result)
		{
			result = confirmed.exists_or_pruned (transaction_a, hash_a);
		}
		return result;
	});
}

bool nano::ledger::is_epoch_link (nano::link const & link_a) const
{
	return constants.epochs.is_epoch_link (link_a);
}

class dependent_block_visitor : public nano::block_visitor
{
public:
	dependent_block_visitor (nano::ledger const & ledger_a, nano::store::transaction const & transaction_a) :
		ledger (ledger_a),
		transaction (transaction_a),
		result ({ 0, 0 })
	{
	}
	void send_block (nano::send_block const & block_a) override
	{
		result[0] = block_a.previous ();
	}
	void receive_block (nano::receive_block const & block_a) override
	{
		result[0] = block_a.previous ();
		result[1] = block_a.source_field ().value ();
	}
	void open_block (nano::open_block const & block_a) override
	{
		if (block_a.source_field ().value () != ledger.constants.genesis->account ())
		{
			result[0] = block_a.source_field ().value ();
		}
	}
	void change_block (nano::change_block const & block_a) override
	{
		result[0] = block_a.previous ();
	}
	void state_block (nano::state_block const & block_a) override
	{
		result[0] = block_a.hashables.previous;
		result[1] = block_a.hashables.link.as_block_hash ();
		// ledger.is_send will check the sideband first, if block_a has a loaded sideband the check that previous block exists can be skipped
		if (ledger.is_epoch_link (block_a.hashables.link) || is_send (transaction, block_a))
		{
			result[1].clear ();
		}
	}
	// This function is used in place of block->is_send () as it is tolerant to the block not having the sideband information loaded
	// This is needed for instance in vote generation on forks which have not yet had sideband information attached
	bool is_send (nano::store::transaction const & transaction, nano::state_block const & block) const
	{
		if (block.previous ().is_zero ())
		{
			return false;
		}
		if (block.has_sideband ())
		{
			return block.sideband ().details.is_send;
		}
		return block.balance_field ().value () < ledger.any.balance (transaction, block.previous ());
	}
	nano::ledger const & ledger;
	nano::store::transaction const & transaction;
	std::array<nano::block_hash, 2> result;
};

std::array<nano::block_hash, 2> nano::ledger::dependent_blocks (store::transaction const & transaction_a, nano::block const & block_a) const
{
	dependent_block_visitor visitor (*this, transaction_a);
	block_a.visit (visitor);
	return visitor.result;
}

/** Given the block hash of a send block, find the associated receive block that receives that send.
 *  The send block hash is not checked in any way, it is assumed to be correct.
 * @return Return the receive block on success and null on failure
 */
std::shared_ptr<nano::block> nano::ledger::find_receive_block_by_send_hash (store::transaction const & transaction, nano::account const & destination, nano::block_hash const & send_block_hash)
{
	std::shared_ptr<nano::block> result;
	debug_assert (send_block_hash != 0);

	// get the cemented frontier
	std::optional<nano::account_info> info;
	if (info = confirmed.get (transaction, destination); !info)
	{
		return nullptr;
	}
	auto possible_receive_block = any.get (transaction, info.value ().head);

	// walk down the chain until the source field of a receive block matches the send block hash
	while (possible_receive_block != nullptr)
	{
		if (possible_receive_block->is_receive () && send_block_hash == possible_receive_block->source ())
		{
			// we have a match
			result = possible_receive_block;
			break;
		}

		possible_receive_block = any.get (transaction, possible_receive_block->previous ());
	}

	return result;
}

nano::account const & nano::ledger::epoch_signer (nano::link const & link_a) const
{
	return constants.epochs.signer (constants.epochs.epoch (link_a));
}

nano::link const & nano::ledger::epoch_link (nano::epoch epoch_a) const
{
	return constants.epochs.link (epoch_a);
}

void nano::ledger::update_account (store::write_transaction const & transaction_a, nano::account const & account_a, nano::account_info const & old_a, nano::account_info const & new_a)
{
	if (!new_a.head.is_zero ())
	{
		if (old_a.head.is_zero () && new_a.open_block == new_a.head)
		{
			++cache.account_count;
		}
		if (!old_a.head.is_zero () && old_a.epoch () != new_a.epoch ())
		{
			// store.account.put won't erase existing entries if they're in different tables
			store.account.del (transaction_a, account_a);
		}
		store.account.put (transaction_a, account_a, new_a);
	}
	else
	{
		debug_assert (!store.confirmation_height.exists (transaction_a, account_a));
		store.account.del (transaction_a, account_a);
		debug_assert (cache.account_count > 0);
		--cache.account_count;
	}
}

void nano::ledger::track (store::write_transaction const & transaction, nano::block_delta const & delta)
{
	auto & block = *delta.block;
	unconfirmed.block[block.hash ()] = delta;
	if (!block.previous ().is_zero ())
	{
		debug_assert (unconfirmed.successor.count (block.previous ()) == 0);
		unconfirmed.successor[block.previous ()] = block.hash ();
		if (unconfirmed.account.count (block.account ()) == 0)
		{
			++unconfirmed.accounts_updated;
		}
	}
	unconfirmed.account[block.account ()] = delta.head;
	auto const & [receivable_key, receivable_info] = delta.receivable;
	if (receivable_key)
	{
		if (receivable_info)
		{
			unconfirmed.receivable[receivable_key.value ()] = receivable_info.value ();
		}
		else
		{
			unconfirmed.received.insert (receivable_key.value ());
		}
	}
	if (delta.weight.first)
	{
		cache.rep_weights.representation_add (transaction, delta.weight.first.value (), 0 - delta.weight.second.value ().number ());
	}
	cache.rep_weights.representation_add (transaction, delta.head.representative, delta.head.balance.number ());
}

std::shared_ptr<nano::block> nano::ledger::forked_block (store::transaction const & transaction_a, nano::block const & block_a)
{
	debug_assert (!any.exists (transaction_a, block_a.hash ()));
	auto root (block_a.root ());
	debug_assert (any.exists (transaction_a, root.as_block_hash ()) || store.account.exists (transaction_a, root.as_account ()));
	std::shared_ptr<nano::block> result;
	auto successor_l = any.successor (transaction_a, root.as_block_hash ());
	if (successor_l)
	{
		result = any.get (transaction_a, successor_l.value ());
	}
	if (result == nullptr)
	{
		auto info = any.get (transaction_a, root.as_account ());
		debug_assert (info);
		result = any.get (transaction_a, info->open_block);
		debug_assert (result != nullptr);
	}
	return result;
}

uint64_t nano::ledger::pruning_action (store::write_transaction & transaction_a, nano::block_hash const & hash_a, uint64_t const batch_size_a)
{
	uint64_t pruned_count (0);
	nano::block_hash hash (hash_a);
	while (!hash.is_zero () && hash != constants.genesis->hash ())
	{
		auto block_l = any.get (transaction_a, hash);
		if (block_l != nullptr)
		{
			release_assert (confirmed.exists (transaction_a, hash));
			store.block.del (transaction_a, hash);
			store.pruned.put (transaction_a, hash);
			hash = block_l->previous ();
			++pruned_count;
			++cache.pruned_count;
			if (pruned_count % batch_size_a == 0)
			{
				transaction_a.commit ();
				transaction_a.renew ();
			}
		}
		else if (store.pruned.exists (transaction_a, hash))
		{
			hash = 0;
		}
		else
		{
			hash = 0;
			release_assert (false && "Error finding block for pruning");
		}
	}
	return pruned_count;
}

// A precondition is that the store is an LMDB store
bool nano::ledger::migrate_lmdb_to_rocksdb (std::filesystem::path const & data_path_a) const
{
	boost::system::error_code error_chmod;
	nano::set_secure_perm_directory (data_path_a, error_chmod);
	auto rockdb_data_path = data_path_a / "rocksdb";
	std::filesystem::remove_all (rockdb_data_path);

	nano::logger logger;
	auto error (false);

	// Open rocksdb database
	nano::rocksdb_config rocksdb_config;
	rocksdb_config.enable = true;
	auto rocksdb_store = nano::make_store (logger, data_path_a, nano::dev::constants, false, true, rocksdb_config);

	if (!rocksdb_store->init_error ())
	{
		store.block.for_each_par (
		[&rocksdb_store] (store::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::blocks }));

				std::vector<uint8_t> vector;
				{
					nano::vectorstream stream (vector);
					nano::serialize_block (stream, *i->second.block);
					i->second.sideband.serialize (stream, i->second.block->type ());
				}
				rocksdb_store->block.raw_put (rocksdb_transaction, vector, i->first);
			}
		});

		store.pending.for_each_par (
		[&rocksdb_store] (store::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::pending }));
				rocksdb_store->pending.put (rocksdb_transaction, i->first, i->second);
			}
		});

		store.confirmation_height.for_each_par (
		[&rocksdb_store] (store::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::confirmation_height }));
				rocksdb_store->confirmation_height.put (rocksdb_transaction, i->first, i->second);
			}
		});

		store.account.for_each_par (
		[&rocksdb_store] (store::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::accounts }));
				rocksdb_store->account.put (rocksdb_transaction, i->first, i->second);
			}
		});

		store.rep_weight.for_each_par (
		[&rocksdb_store] (store::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::rep_weights }));
				rocksdb_store->rep_weight.put (rocksdb_transaction, i->first, i->second.number ());
			}
		});

		store.pruned.for_each_par (
		[&rocksdb_store] (store::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::pruned }));
				rocksdb_store->pruned.put (rocksdb_transaction, i->first);
			}
		});

		store.final_vote.for_each_par (
		[&rocksdb_store] (store::read_transaction const & /*unused*/, auto i, auto n) {
			for (; i != n; ++i)
			{
				auto rocksdb_transaction (rocksdb_store->tx_begin_write ({}, { nano::tables::final_votes }));
				rocksdb_store->final_vote.put (rocksdb_transaction, i->first, i->second);
			}
		});

		auto lmdb_transaction (store.tx_begin_read ());
		auto version = store.version.get (lmdb_transaction);
		auto rocksdb_transaction (rocksdb_store->tx_begin_write ());
		rocksdb_store->version.put (rocksdb_transaction, version);

		for (auto i (store.online_weight.begin (lmdb_transaction)), n (store.online_weight.end ()); i != n; ++i)
		{
			rocksdb_store->online_weight.put (rocksdb_transaction, i->first, i->second);
		}

		for (auto i (store.peer.begin (lmdb_transaction)), n (store.peer.end ()); i != n; ++i)
		{
			rocksdb_store->peer.put (rocksdb_transaction, i->first);
		}

		// Compare counts
		error |= store.peer.count (lmdb_transaction) != rocksdb_store->peer.count (rocksdb_transaction);
		error |= store.pruned.count (lmdb_transaction) != rocksdb_store->pruned.count (rocksdb_transaction);
		error |= store.final_vote.count (lmdb_transaction) != rocksdb_store->final_vote.count (rocksdb_transaction);
		error |= store.online_weight.count (lmdb_transaction) != rocksdb_store->online_weight.count (rocksdb_transaction);
		error |= store.version.get (lmdb_transaction) != rocksdb_store->version.get (rocksdb_transaction);

		// For large tables a random key is used instead and makes sure it exists
		auto random_block (store.block.random (lmdb_transaction));
		error |= rocksdb_store->block.get (rocksdb_transaction, random_block->hash ()) == nullptr;

		auto account = random_block->account ();
		nano::account_info account_info;
		error |= rocksdb_store->account.get (rocksdb_transaction, account, account_info);

		// If confirmation height exists in the lmdb ledger for this account it should exist in the rocksdb ledger
		nano::confirmation_height_info confirmation_height_info{};
		if (!store.confirmation_height.get (lmdb_transaction, account, confirmation_height_info))
		{
			error |= rocksdb_store->confirmation_height.get (rocksdb_transaction, account, confirmation_height_info);
		}
	}
	else
	{
		error = true;
	}
	return error;
}

bool nano::ledger::bootstrap_weight_reached () const
{
	return cache.block_count >= bootstrap_weight_max_blocks;
}

nano::epoch nano::ledger::version (nano::block const & block)
{
	if (block.type () == nano::block_type::state)
	{
		return block.sideband ().details.epoch;
	}

	return nano::epoch::epoch_0;
}

nano::epoch nano::ledger::version (store::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block_l = any.get (transaction, hash);
	if (block_l == nullptr)
	{
		return nano::epoch::epoch_0;
	}
	return version (*block_l);
}

void nano::ledger::confirm (nano::store::write_transaction const & transaction, nano::block_delta const & delta)
{
	auto & block = *delta.block;
	auto account_l = block.account ();
	auto hash = block.hash ();
	store.block.put (transaction, hash, block);
	stats.inc (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed);
	++cache.cemented_count;
	++cache.block_count;
	[[maybe_unused]] auto erased = unconfirmed.block.erase (hash);
	debug_assert (erased == 1);
	if (!block.previous ().is_zero ())
	{
		[[maybe_unused]] auto erased = unconfirmed.successor.erase (block.previous ());
		debug_assert (erased == 1);
	}
	auto info = store.account.get (transaction, account_l);
	auto representative = block.representative_field () ? block.representative_field ().value () : info.value ().representative;
	if (unconfirmed.account[account_l].head == hash)
	{
		[[maybe_unused]] auto erased = unconfirmed.account.erase (account_l);
		debug_assert (erased == 1);
		if (!block.previous ().is_zero ())
		{
			debug_assert (unconfirmed.accounts_updated > 0);
			--unconfirmed.accounts_updated;
		}
	}
	else
	{
		if (block.previous ().is_zero ())
		{
			++unconfirmed.accounts_updated;
		}
	}
	if (block.sideband ().details.is_send)
	{
		auto destination_l = block.destination ();
		nano::pending_key key{ destination_l, hash };
		auto amount = delta.receivable.second.value ().amount;
		nano::pending_info value{ account_l, amount, block.sideband ().details.epoch };
		store.pending.put (transaction, key, value);
		[[maybe_unused]] auto erased = unconfirmed.receivable.erase (key);
		debug_assert (erased);
		debug_assert (info.has_value ());
	}
	else if (block.sideband ().details.is_receive)
	{
		auto source_l = block.source ();
		nano::pending_key key{ account_l, source_l };
		store.pending.del (transaction, key);
		[[maybe_unused]] auto erased = unconfirmed.received.erase (key);
		debug_assert (erased);
	}
	store.account.put (transaction, account_l, delta.head);
	if (block.previous ().is_zero ())
	{
		++cache.account_count;
	}
}

uint64_t nano::ledger::cemented_count () const
{
	return cache.cemented_count;
}

uint64_t nano::ledger::block_count () const
{
	return cache.block_count + unconfirmed.block_size ();
}

uint64_t nano::ledger::account_count () const
{
	return cache.account_count + unconfirmed.account_size ();
}

uint64_t nano::ledger::pruned_count () const
{
	return cache.pruned_count;
}

std::unique_ptr<nano::container_info_component> nano::ledger::collect_container_info (std::string const & name) const
{
	auto count = bootstrap_weights.size ();
	auto sizeof_element = sizeof (decltype (bootstrap_weights)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "bootstrap_weights", count, sizeof_element }));
	composite->add_component (cache.rep_weights.collect_container_info ("rep_weights"));
	return composite;
}
