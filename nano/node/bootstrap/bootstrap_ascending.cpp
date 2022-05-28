#include <nano/node/bootstrap/bootstrap_ascending.hpp>
#include <nano/node/bootstrap/block_deserializer.hpp>
#include <nano/secure/common.hpp>

#include <nano/node/node.hpp>

#include <boost/format.hpp>

using namespace std::chrono_literals;

nano::bootstrap::bootstrap_ascending::async_tag::async_tag (std::shared_ptr<nano::bootstrap::bootstrap_ascending> bootstrap) :
	bootstrap{ bootstrap }
{
	std::lock_guard<nano::mutex> lock{ bootstrap->mutex };
	++bootstrap->requests;
	bootstrap->condition.notify_all ();
	//std::cerr << boost::str (boost::format ("Request started\n"));
}

nano::bootstrap::bootstrap_ascending::async_tag::~async_tag ()
{
	std::lock_guard<nano::mutex> lock{ bootstrap->mutex };
	--bootstrap->requests;
	bootstrap->condition.notify_all ();
	//std::cerr << boost::str (boost::format ("Request completed\n"));
}

void nano::bootstrap::bootstrap_ascending::send (std::shared_ptr<async_tag> tag, socket_channel ctx, nano::hash_or_account const & start)
{
	nano::bulk_pull message{ node->network_params.network };
	message.header.flag_set (nano::message_header::bulk_pull_ascending_flag);
	message.header.flag_set (nano::message_header::bulk_pull_count_present_flag);
	message.start = start;
	message.end = 0;
	message.count = request_message_count;
	//std::cerr << boost::str (boost::format ("Request sent for: %1% to: %2%\n") % message.start.to_string () % ctx.first->remote_endpoint ());
	auto channel = ctx.second;
	channel->send (message, [this_l = shared (), tag, ctx] (boost::system::error_code const & ec, std::size_t size) {
		this_l->read_block (tag, ctx);
	});
}

void nano::bootstrap::bootstrap_ascending::read_block (std::shared_ptr<async_tag> tag, socket_channel ctx)
{
	auto deserializer = std::make_shared<nano::bootstrap::block_deserializer>();
	auto socket = ctx.first;
	deserializer->read (*socket, [this_l = shared (), tag, ctx] (boost::system::error_code ec, std::shared_ptr<nano::block> block) {
		if (block == nullptr)
		{
			//std::cerr << "stream end\n";
			std::lock_guard<nano::mutex> lock{ this_l->mutex };
			this_l->sockets.push_back (ctx);
			return;
		}
		//std::cerr << boost::str (boost::format ("block: %1%\n") % block->hash ().to_string ());
		this_l->node->block_processor.add (block);
		this_l->read_block (tag, ctx);
		++tag->blocks;
	});
}

void nano::bootstrap::bootstrap_ascending::dump_backoff_hist ()
{
	std::vector<size_t> weight_counts;
	std::lock_guard<nano::mutex> lock{ mutex };
	for (auto &[account, count]: backoff)
	{
		auto log = std::log2 (std::max<decltype(count)> (count, 1));
		//std::cerr << "log: " << log << ' ';
		auto index = static_cast<size_t> (log);
		if (weight_counts.size () <= index)
		{
			weight_counts.resize (index + 1);
		}
		++weight_counts[index];
	}
	std::string output;
	output += "Backoff hist (" + std::to_string (backoff.size ()) + "): ";
	for (size_t i = 0, n = weight_counts.size (); i < n; ++i)
	{
		output += std::to_string (weight_counts[i]) + ' ';
	}
	output += '\n';
	std::cerr << output;
}

nano::account nano::bootstrap::bootstrap_ascending::random_account_entry (nano::account const & search)
{
	auto tx = node->store.tx_begin_read ();
	auto existing_account = node->store.account.begin (tx, search);
	if (existing_account == node->store.account.end ())
	{
		existing_account = node->store.account.begin (tx);
		debug_assert (existing_account != node->store.account.end ());
	}
	//std::cerr << boost::str (boost::format ("Found: %1%\n") % result.to_string ());
	return existing_account->first;
}

std::optional<nano::account> nano::bootstrap::bootstrap_ascending::random_pending_entry (nano::account const & search)
{
	auto tx = node->store.tx_begin_read ();
	auto existing_pending = node->store.pending.begin (tx, nano::pending_key{ search, 0 });
	std::optional<nano::account> result;
	if (existing_pending != node->store.pending.end ())
	{
		result = existing_pending->first.key ();
	}
	return result;
}

std::optional<nano::account> nano::bootstrap::bootstrap_ascending::random_ledger_account ()
{
	nano::account search;
	nano::random_pool::generate_block (search.bytes.data (), search.bytes.size ());
	//std::cerr << boost::str (boost::format ("Search: %1% ") % search.to_string ());
	auto rand = nano::random_pool::generate_byte ();
	if (rand & 0x1)
	{
		//std::cerr << boost::str (boost::format ("account "));
		return random_account_entry (search);
	}
	else
	{
		//std::cerr << boost::str (boost::format ("pending "));
		return random_pending_entry (search);
	}
}

std::optional<nano::account> nano::bootstrap::bootstrap_ascending::pick_account ()
{
	static_assert (backoff_exclusion > 0);
	std::unordered_set<nano::account> accounts;
	{
		std::lock_guard<nano::mutex> lock{ mutex };
		if (!forwarding.empty ())
		{
			auto first = forwarding.begin ();;
			accounts.insert (*first);
			forwarding.erase (first);
		}
	}
	while (accounts.size () < backoff_exclusion)
	{
		auto account = random_ledger_account ();
		if (account)
		{
			if (accounts.count (*account) > 0)
			{
				break;
			}
			accounts.insert (*account);
		}
	}
	std::lock_guard<nano::mutex> lock{ mutex };
	return *std::min_element (accounts.begin (), accounts.end (), [this] (nano::account const & lhs, nano::account const & rhs) { return backoff[lhs] < backoff[rhs]; });
}

bool nano::bootstrap::bootstrap_ascending::wait_available_request ()
{
	std::unique_lock<nano::mutex> lock{ mutex };
	condition.wait (lock, [this] () { return stopped || requests < requests_max; } );
	return stopped;
}

nano::bootstrap::bootstrap_ascending::bootstrap_ascending (std::shared_ptr<nano::node> const & node_a, uint64_t incremental_id_a, std::string id_a) :
	bootstrap_attempt{ node_a, nano::bootstrap_mode::ascending, incremental_id_a, id_a }
{
	std::cerr << '\0';
}

void nano::bootstrap::bootstrap_ascending::request_one ()
{
	wait_available_request ();
	if (stopped)
	{
		return;
	}
	auto tag = std::make_shared<async_tag> (shared ());
	auto account = pick_account ();
	if (!account)
	{
		return;
	}
	auto existing = backoff [*account];
	auto updated = existing + 1;
	if (updated < existing)
	{
		updated = std::numeric_limits<decltype(updated)>::max ();
	}
	backoff[*account] = updated;
	nano::account_info info;
	nano::hash_or_account start = *account;
	if (!node->store.account.get (node->store.tx_begin_read (), *account, info))
	{
		start = info.head;
	}
	std::unique_lock<nano::mutex> lock{ mutex };
	if (!sockets.empty ())
	{
		auto socket = sockets.front ();
		sockets.pop_front ();
		send (tag, socket, start);
		return;
	}
	lock.unlock ();
	auto endpoint = node->network.bootstrap_peer (true);
	if (endpoint != nano::tcp_endpoint (boost::asio::ip::address_v6::any (), 0))
	{
		auto socket = std::make_shared<nano::client_socket> (*node);
		auto channel = std::make_shared<nano::transport::channel_tcp> (*node, socket);
		std::cerr << boost::str (boost::format ("Connecting: %1%\n") % endpoint);
		socket->async_connect (endpoint,
		[this_l = shared (), endpoint, tag, ctx = std::make_pair (socket, channel), start] (boost::system::error_code const & ec) {
			if (ec)
			{
				std::cerr << boost::str (boost::format ("connect failed to: %1%\n") % endpoint);
				return;
			}
			std::cerr << boost::str (boost::format ("connected to: %1%\n") % endpoint);
			this_l->send (tag, ctx, start);
		});
	}
	else
	{
		stop ();
	}
}

static int pass_number = 0;

void nano::bootstrap::bootstrap_ascending::run ()
{
	std::cerr << "!! Starting with:" << std::to_string (pass_number++) << "\n";
	node->block_processor.processed.add ([this_w = std::weak_ptr<nano::bootstrap::bootstrap_ascending>{ shared () }] (nano::transaction const & tx, nano::process_return const & result, nano::block const & block) {
		auto this_l = this_w.lock ();
		if (this_l == nullptr)
		{
			return;
		}
		std::lock_guard<nano::mutex> lock{ this_l->mutex };
		switch (result.code)
		{
			case nano::process_result::progress:
			{
				auto account = this_l->node->ledger.account (tx, block.hash ());
				this_l->backoff [account] = 0;
				this_l->forwarding.insert (account);
				if (this_l->node->ledger.is_send (tx, block))
				{
					switch (block.type ())
					{
						case nano::block_type::send:
							account = block.destination ();
							break;
						case nano::block_type::state:
							account = block.link ().as_account ();
							break;
						default:
							debug_assert (false);
							account = account; // Fail in a predictable way, insert the block's account again.
							break;
					}
					this_l->forwarding.insert (account);
				}
				break;
			}
			case nano::process_result::gap_source:
			{
				auto account = block.previous ().is_zero () ? block.account () : this_l->node->ledger.account (tx, block.previous ());
				auto existing = this_l->backoff [account];
				auto updated = existing << 1;
				if (updated < existing)
				{
					updated = std::numeric_limits<decltype(updated)>::max ();
				}
				this_l->backoff [account] = updated;
				break;
			}
			default:
				break;
		}
	});
	//for (auto i = 0; !stopped && i < 5'000; ++i)
	int counter = 0;
	while (!stopped)
	{
		request_one ();
		if ((++counter % 10'000) == 0)
		{
			node->block_processor.flush ();
			node->block_processor.dump_result_hist ();
			dump_backoff_hist ();
			std::lock_guard<nano::mutex> lock{ mutex };
			std::cerr << boost::str (boost::format ("Forwarding: %1%\n") % forwarding.size ());
		}
	}
	
	std::cerr << "!! stopping" << std::endl;
}

void nano::bootstrap::bootstrap_ascending::get_information (boost::property_tree::ptree &)
{
}

std::shared_ptr<nano::bootstrap::bootstrap_ascending> nano::bootstrap::bootstrap_ascending::shared ()
{
	return std::static_pointer_cast<nano::bootstrap::bootstrap_ascending> (shared_from_this ());
}
