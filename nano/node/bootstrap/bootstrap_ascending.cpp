#include <nano/node/bootstrap/block_deserializer.hpp>
#include <nano/node/bootstrap/bootstrap_ascending.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/secure/common.hpp>

#include <boost/format.hpp>

using namespace std::chrono_literals;

/*
 * account_sets
 */

nano::bootstrap_ascending::account_sets::iterator_t::iterator_t (nano::store & store) :
store{ store }
{
}

nano::account nano::bootstrap_ascending::account_sets::iterator_t::operator* () const
{
	return current;
}

void nano::bootstrap_ascending::account_sets::iterator_t::next (nano::transaction & tx)
{
	switch (table)
	{
	case table_t::account:
	{
		auto i = current.number () + 1;
		auto item = store.account.begin (tx, i);
		if (item != store.account.end ())
		{
			current = item->first;
		}
		else
		{
			item = nullptr;
			table = table_t::pending;
			current = 0;
			next (tx);
		}
		break;
	}
	case table_t::pending:
	{
		auto i = current.number () + 1;
		auto item = store.pending.begin (tx, nano::pending_key{ i, 0 });
		if (item != store.pending.end ())
		{
			current = item->first.account;
		}
		else
		{
			item = nullptr;
			table = table_t::account;
			current = 0;
			next (tx);
		}
		break;
	}
	}
}

nano::bootstrap_ascending::account_sets::account_sets (nano::stat & stats_a, nano::store & store) :
	stats{ stats_a },
	store{ store },
	iter{ store }
{
}

void nano::bootstrap_ascending::account_sets::dump () const
{
	std::cerr << boost::str (boost::format ("Blocking: %1%\n") % blocking.size ());
	std::deque<size_t> weight_counts;
	float max = 0.0f;
	for (auto const & [account, priority] : priorities)
	{
		auto count = std::log2 (std::max (priority, 1.0f));
		if (weight_counts.size () <= count)
		{
			weight_counts.resize (count + 1);
		}
		++weight_counts[count];
		max = std::max (max, priority);
	}
	std::string output;
	output += "Priorities hist (max: " + std::to_string (max) + " size: " + std::to_string (priorities.size ()) + "): ";
	for (size_t i = 0, n = weight_counts.size (); i < n; ++i)
	{
		output += std::to_string (weight_counts[i]) + ' ';
	}
	output += '\n';
	std::cerr << output;
}

void nano::bootstrap_ascending::account_sets::priority_up (nano::account const & account)
{
	auto blocking_iter = blocking.find (account);
	if (blocking_iter == blocking.end ())
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::prioritize);

		auto iter = priorities.get<tag_account> ().find (account);
		if (iter != priorities.get<tag_account> ().end ())
		{
			priorities.get<tag_account> ().modify (iter, [] (auto & val) {
				val.priority += 0.4f;
			});
		}
		else
		{
			if (priorities.size () < priorities_max)
			{
				priorities.insert ({ account, 1.4f });
			}
		}
	}
	else
	{
		blocking_iter->second.second += 1.0f;
	}
}

void nano::bootstrap_ascending::account_sets::priority_down (nano::account const & account)
{
	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		auto priority_new = iter->priority / 2.0f;
		if (priority_new <= 1.0f)
		{
			priorities.get<tag_account> ().erase (iter);
		}
		else
		{
			priorities.get<tag_account> ().modify (iter, [priority_new] (auto & val) {
				val.priority = priority_new;
			});
		}
	}
	auto blocking_iter = blocking.find (account);
	if (blocking_iter != blocking.end ())
	{
		blocking_iter->second.second /= 2.0f;
	}
}

void nano::bootstrap_ascending::account_sets::priority_dec (nano::account const & account)
{
	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		auto priority_new = iter->priority - 0.5f;
		if (priority_new <= 1.0f)
		{
			priorities.get<tag_account> ().erase (iter);
		}
		else
		{
			priorities.get<tag_account> ().modify (iter, [priority_new] (auto & val) {
				val.priority = priority_new;
			});
		}
	}
	auto blocking_iter = blocking.find (account);
	if (blocking_iter != blocking.end ())
	{
		blocking_iter->second.second -= 0.5f;
	}
}

void nano::bootstrap_ascending::account_sets::block (nano::account const & account, nano::block_hash const & dependency)
{
	stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::block);
	auto existing = priorities.get<tag_account> ().find (account);
	auto count = existing == priorities.get<tag_account> ().end () ? 1.0f : existing->priority;
	priorities.get<tag_account> ().erase (account);
	blocking[account] = std::make_pair (dependency, count);
}

void nano::bootstrap_ascending::account_sets::unblock (nano::account const & account, std::optional<nano::block_hash> const & hash)
{
	auto existing = blocking.find (account);
	// Unblock only if the dependency is fulfilled
	if (existing != blocking.end () && (!hash || existing->second.first == *hash))
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::unblock);
		if (priorities.size () < priorities_max)
		{
			priorities.insert ({ account, existing->second.second });
		}
		blocking.erase (account);
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending_accounts, nano::stat::detail::unblock_failed);
	}
}

nano::account nano::bootstrap_ascending::account_sets::random ()
{
	if (priorities.empty ())
	{
		auto tx = store.tx_begin_read ();
		iter.next (tx);
		//std::cerr << "Disk: " << (*iter).to_account () << '\n';
		return *iter;
	}
	std::vector<float> weights;
	std::vector<nano::account> candidates;
	while (candidates.size () < account_sets::consideration_count)
	{
		debug_assert (candidates.size () == weights.size ());
		nano::account search;
		nano::random_pool::generate_block (search.bytes.data (), search.bytes.size ());
		auto iter = priorities.get<tag_account> ().lower_bound (search);
		if (iter == priorities.get<tag_account> ().end ())
		{
			iter = priorities.get<tag_account> ().begin ();
		}
		candidates.push_back (iter->account);
		weights.push_back (iter->priority);
	}
	std::discrete_distribution dist{ weights.begin (), weights.end () };
	auto selection = dist (rng);
	debug_assert (!weights.empty () && selection < weights.size ());
	auto result = candidates[selection];
	priority_dec (result);
	return result;
}

bool nano::bootstrap_ascending::account_sets::blocked (nano::account const & account) const
{
	return blocking.count (account) > 0;
}

size_t nano::bootstrap_ascending::account_sets::priority_size () const
{
	return priorities.size ();
}

size_t nano::bootstrap_ascending::account_sets::blocked_size () const
{
	return blocking.size ();
}

float nano::bootstrap_ascending::account_sets::priority (nano::account const & account) const
{
	if (blocked (account))
	{
		return 0.0f;
	}
	auto existing = priorities.find (account);
	if (existing == priorities.end ())
	{
		return 1.0f;
	}
	return existing->priority;
}

auto nano::bootstrap_ascending::account_sets::info () const -> info_t
{
	return { blocking, priorities };
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::account_sets::collect_container_info (const std::string & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "priorities", priorities.size (), sizeof (decltype (priorities)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocking", blocking.size (), sizeof (decltype (blocking)::value_type) }));
	return composite;
}

/*
 * bootstrap_ascending
 */

nano::bootstrap_ascending::bootstrap_ascending (nano::node & node_a, nano::store & store_a, nano::block_processor & block_processor_a, nano::ledger & ledger_a, nano::network & network_a, nano::stat & stat_a) :
	node{ node_a },
	store{ store_a },
	block_processor{ block_processor_a },
	ledger{ ledger_a },
	network{ network_a },
	stats{ stat_a },
	accounts{ stats, store_a }
{
	block_processor.processed.add ([this] (nano::transaction const & tx, nano::process_return const & result, nano::block const & block) {
		inspect (tx, result, block);
	});

	//	on_timeout.add ([this] (auto tag) {
	//		std::cout << "timeout: " << tag.id
	//				  << " | "
	//				  << "count: " << tags.size ()
	//				  << std::endl;
	//	});
	//
	//	on_request.add ([this] (auto tag, auto channel) {
	//		std::cout << "requesting: " << tag.id
	//				  << " | "
	//				  << "channel: " << channel->to_string ()
	//				  << std::endl;
	//	});
}

nano::bootstrap_ascending::~bootstrap_ascending ()
{
	// All threads must be stopped before destruction
	debug_assert (threads.empty ());
	debug_assert (!timeout_thread.joinable ());
}

void nano::bootstrap_ascending::start ()
{
	debug_assert (threads.empty ());
	debug_assert (!timeout_thread.joinable ());

	// TODO: Use value read from node config
	const std::size_t thread_count = 2;

	for (int n = 0; n < thread_count; ++n)
	{
		threads.emplace_back ([this] () {
			nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
			run ();
		});
	}

	timeout_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
		run_timeouts ();
	});
}

void nano::bootstrap_ascending::stop ()
{
	stopped = true;

	for (auto & thread : threads)
	{
		debug_assert (thread.joinable ());
		thread.join ();
	}
	threads.clear ();

	nano::join_or_pass (timeout_thread);
}

void nano::bootstrap_ascending::priority_up (nano::account const & account_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	accounts.priority_up (account_a);
}

void nano::bootstrap_ascending::priority_down (nano::account const & account_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	accounts.priority_down (account_a);
}

uint64_t nano::bootstrap_ascending::generate_id () const
{
	id_t id;
	nano::random_pool::generate_block (reinterpret_cast<uint8_t *> (&id), sizeof (id));
	return id;
}

void nano::bootstrap_ascending::send (std::shared_ptr<nano::transport::channel> channel, async_tag tag)
{
	nano::asc_pull_req request{ node.network_params.network };
	request.id = tag.id;
	request.type = nano::asc_pull_type::blocks;

	nano::asc_pull_req::blocks_payload request_payload;
	request_payload.start = tag.start;
	request_payload.count = nano::bootstrap_server::max_blocks;

	request.payload = request_payload;
	request.update_header ();

	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::request, nano::stat::dir::out);

	//		std::cout << "requesting: " << std::setw (28) << tag.id
	//			  << " | "
	//			  << "channel: " << channel->to_string ()
	//			  << std::endl;

	channel->send (
	request, [this, tag] (boost::system::error_code const & ec, std::size_t size) {
		if (ec)
		{
			std::cerr << "send error: " << ec.message () << std::endl;
		}
	},
	nano::buffer_drop_policy::no_limiter_drop, nano::bandwidth_limit_type::bootstrap);
}

size_t nano::bootstrap_ascending::priority_size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.priority_size ();
}

size_t nano::bootstrap_ascending::blocked_size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.blocked_size ();
}

/** Inspects a block that has been processed by the block processor
- Marks an account as blocked if the result code is gap source as there is no reason request additional blocks for this account until the dependency is resolved
- Marks an account as forwarded if it has been recently referenced by a block that has been inserted.
 */
void nano::bootstrap_ascending::inspect (nano::transaction const & tx, nano::process_return const & result, nano::block const & block)
{
	auto const hash = block.hash ();

	switch (result.code)
	{
		case nano::process_result::progress:
		{
			const auto account = ledger.account (tx, hash);
			const auto is_send = ledger.is_send (tx, block);

			nano::lock_guard<nano::mutex> lock{ mutex };

			// If we've inserted any block in to an account, unmark it as blocked
			accounts.unblock (account, std::nullopt);
			// Forward and initialize backoff value with 0.0 for the current account
			// 0.0 has the highest priority
			accounts.priority_up (account);

			if (is_send)
			{
				// Initialize with value of 1.0 a value of lower priority than an account itselt
				// This is the same priority as if it had already made 1 attempt.
				auto const send_factor = 1.0f;

				switch (block.type ())
				{
					// Forward and initialize backoff for the referenced account
					case nano::block_type::send:
						accounts.unblock (block.destination (), hash);
						accounts.priority_up (block.destination ());
						break;
					case nano::block_type::state:
						accounts.unblock (block.link ().as_account (), hash);
						accounts.priority_up (block.link ().as_account ());
						break;
					default:
						debug_assert (false);
						break;
				}
			}
			break;
		}
		case nano::process_result::gap_source:
		{
			const auto account = block.previous ().is_zero () ? block.account () : ledger.account (tx, block.previous ());
			const auto source = block.source ().is_zero () ? block.link ().as_block_hash () : block.source ();

			nano::lock_guard<nano::mutex> lock{ mutex };
			// Mark account as blocked because it is missing the source block
			accounts.block (account, source);
			break;
		}
		case nano::process_result::old:
		{
			auto account = ledger.account (tx, hash);
			//std::cerr << boost::str (boost::format ("old account: %1%\n") % account.to_account ());
			nano::lock_guard<nano::mutex> lock{ mutex };
			accounts.priority_dec (account);
			auto existing = account_stats.get<tag_account> ().find (account);
			if (existing == account_stats.end ())
			{
				account_stats.insert ({ account, 1, 0 });
			}
			else
			{
				account_stats.modify (existing, [] (auto & item) {
					item.old += 1;
				});
			}
			break;
		}
		case nano::process_result::gap_previous:
			break;
		default:
		{
			std::clog << '\0';
			break;
		}
	}
}

void nano::bootstrap_ascending::wait_blockprocessor () const
{
	while (!stopped && block_processor.half_full ())
	{
		std::this_thread::sleep_for (500ms); // Blockprocessor is relatively slow, sleeping here instead of using conditions
	}
}

void nano::bootstrap_ascending::wait_available_request () const
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	condition.wait (lock, [this] () { return stopped || tags.size () < requests_max; });
}

std::shared_ptr<nano::transport::channel> nano::bootstrap_ascending::available_channel ()
{
	auto channels = network.random_set (32, node.network_params.network.bootstrap_protocol_version_min, /* include temporary channels */ true);
	for (auto & channel : channels)
	{
		if (!channel->max ())
		{
			return channel;
		}
	}
	return nullptr;
}

std::shared_ptr<nano::transport::channel> nano::bootstrap_ascending::wait_available_channel ()
{
	std::shared_ptr<nano::transport::channel> channel;
	// Wait until a channel is available
	while (!stopped && !(channel = available_channel ()))
	{
		std::this_thread::sleep_for (100ms);
	}
	return channel;
}

nano::account nano::bootstrap_ascending::wait_available_account ()
{
	while (!stopped)
	{
		nano::unique_lock<nano::mutex> lock{ mutex };

		auto account = accounts.random ();
		auto existing = account_stats.get<tag_account> ().find (account);
		if (existing == account_stats.end ())
		{
			account_stats.insert ({ account, 0, 1 });
		}
		else
		{
			account_stats.modify (existing, [] (auto & item) {
				item.request += 1;
			});
		}
		static int count = 0;
		if (count++ % 100'000 == 0)
		{
			accounts.dump ();
			auto count = 0;
			for (auto i = account_stats.get <tag_old_count> ().rbegin (), n = account_stats.get<tag_old_count> ().rend (); i != n && count < 100; ++i, ++count)
			{
				std::cerr << boost::str (boost::format ("%1% : %2% : %3%\n") % i->account.to_account () % std::to_string (i->old) % std::to_string (i->request));
			}
		}
		return account;

		condition.wait_for (lock, 100ms);
	}
	return {};
}

bool nano::bootstrap_ascending::request (nano::account & account, std::shared_ptr<nano::transport::channel> & channel)
{
	nano::account_info info;
	nano::hash_or_account start = account;

	//std::cerr << boost::str (boost::format ("req account: %1%\n") % account.to_account ());
	// check if the account picked has blocks, if it does, start the pull from the highest block
	if (!store.account.get (store.tx_begin_read (), account, info))
	{
		start = info.head;
	}

	const async_tag tag{ generate_id (), start, nano::milliseconds_since_epoch (), account };

	on_request.notify (tag, channel);

	track (tag);
	send (channel, tag);

	return true; // Request sent
}

bool nano::bootstrap_ascending::request_one ()
{
	// Ensure there is enough space in blockprocessor for queuing new blocks
	wait_blockprocessor ();

	// Do not do too many requests in parallel, impose throttling
	wait_available_request ();

	auto channel = wait_available_channel ();
	if (!channel)
	{
		return false;
	}

	auto account = wait_available_account ();
	if (account.is_zero ())
	{
		return false;
	}

	bool success = request (account, channel);
	return success;
}

void nano::bootstrap_ascending::run ()
{
	while (!stopped)
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::loop);

		request_one ();
	}
}

void nano::bootstrap_ascending::run_timeouts ()
{
	while (!stopped)
	{
		std::this_thread::sleep_for (1s);

		{
			nano::lock_guard<nano::mutex> lock{ mutex };

			const nano::millis_t threshold = 5 * 1000;

			auto & tags_by_order = tags.get<tag_sequenced> ();
			while (!tags_by_order.empty () && nano::time_difference (tags_by_order.front ().time, nano::milliseconds_since_epoch ()) > threshold)
			{
				auto tag = tags_by_order.front ();
				tags_by_order.pop_front ();
				on_timeout.notify (tag);
				stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::timeout);
			}
		}

		condition.notify_all ();
	}
}

void nano::bootstrap_ascending::process (const nano::asc_pull_ack & message)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	// Only process messages that have a known tag
	auto & tags_by_id = tags.get<tag_id> ();
	if (tags_by_id.count (message.id) > 0)
	{
		auto iterator = tags_by_id.find (message.id);
		auto tag = *iterator;
		tags_by_id.erase (iterator);

		lock.unlock ();
		condition.notify_all ();

		on_reply.notify (tag);

		return std::visit ([this, &tag] (auto && request) { return process (request, tag); }, message.payload);
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::missing_tag);
	}
}

void nano::bootstrap_ascending::process (const nano::asc_pull_ack::blocks_payload & response, const nano::bootstrap_ascending::async_tag & tag)
{
	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::reply);

	// Continue only if there are any blocks to process
	if (response.blocks.empty ())
	{
		priority_down (tag.account);
		return;
	}

	if (verify (response, tag))
	//	if (true)
	{
		stats.add (nano::stat::type::bootstrap_ascending, nano::stat::detail::blocks, nano::stat::dir::in, response.blocks.size ());

		for (auto & block : response.blocks)
		{
			block_processor.add (block);
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::bad_sender);
	}
}

void nano::bootstrap_ascending::process (const nano::asc_pull_ack::account_info_payload & response, const nano::bootstrap_ascending::async_tag & tag)
{
	// TODO: Make use of account info
}

void nano::bootstrap_ascending::process (const nano::empty_payload & response, const nano::bootstrap_ascending::async_tag & tag)
{
	// Should not happen
	debug_assert (false, "empty payload");
}

bool nano::bootstrap_ascending::verify (const nano::asc_pull_ack::blocks_payload & response, const nano::bootstrap_ascending::async_tag & tag) const
{
	debug_assert (!response.blocks.empty ());

	auto first = response.blocks.front ();
	// The `start` field should correspond to either previous block or account
	if (first->hash () == tag.start)
	{
		// Pass
	}
	// Open & state blocks always contain account field
	else if (first->account () == tag.start)
	{
		// Pass
	}
	else
	{
		// TODO: Stat & log
		//		std::cerr << "bad head" << std::endl;
		return false; // Bad head block
	}

	// Verify blocks make a valid chain
	nano::block_hash previous_hash = response.blocks.front ()->hash ();
	for (int n = 1; n < response.blocks.size (); ++n)
	{
		auto & block = response.blocks[n];
		if (block->previous () != previous_hash)
		{
			// TODO: Stat & log
			//			std::cerr << "bad previous" << std::endl;
			return false; // Blocks do not make a chain
		}
		previous_hash = block->hash ();
	}

	return true; // Pass verification
}

void nano::bootstrap_ascending::track (async_tag const & tag)
{
	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::track);

	nano::lock_guard<nano::mutex> lock{ mutex };
	tags.get<tag_id> ().insert (tag);
}

void nano::bootstrap_ascending::debug_log (const std::string & s) const
{
	std::cerr << s << std::endl;
}

auto nano::bootstrap_ascending::info () const -> account_sets::info_t
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return accounts.info ();
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::collect_container_info (std::string const & name)
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (accounts.collect_container_info ("accounts"));
	return composite;
}
