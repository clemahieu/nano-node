#include <nano/node/bootstrap/bootstrap_bulk_push.hpp>
#include <nano/node/bootstrap/bootstrap_frontier.hpp>
#include <nano/node/messages.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/message_deserializer.hpp>
#include <nano/node/transport/tcp.hpp>
#include <nano/node/transport/tcp_server.hpp>

#include <boost/format.hpp>

#include <memory>

namespace
{
bool is_temporary_error (boost::system::error_code const & ec_a)
{
	switch (ec_a.value ())
	{
#if EAGAIN != EWOULDBLOCK
		case EAGAIN:
#endif

		case EWOULDBLOCK:
		case EINTR:
			return true;
		default:
			return false;
	}
}
}

/*
 * tcp_listener
 */

nano::transport::tcp_listener::tcp_listener (uint16_t port_a, nano::node & node_a, std::size_t max_inbound_connections) :
	node (node_a),
	strand{ node_a.io_ctx.get_executor () },
	acceptor{ node_a.io_ctx },
	local{ boost::asio::ip::tcp::endpoint{ boost::asio::ip::address_v6::any (), port_a } },
	max_inbound_connections{ max_inbound_connections }
{
}

void nano::transport::tcp_listener::start (std::function<bool (std::shared_ptr<nano::transport::socket> const &, boost::system::error_code const &)> callback_a)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	on = true;
	acceptor.open (local.protocol ());
	acceptor.set_option (boost::asio::ip::tcp::acceptor::reuse_address (true));
	boost::system::error_code ec;
	acceptor.bind (local, ec);
	if (!ec)
	{
		acceptor.listen (boost::asio::socket_base::max_listen_connections, ec);
	}
	if (ec)
	{
		node.logger.always_log (boost::str (boost::format ("Network: Error while binding for incoming TCP/bootstrap on port %1%: %2%") % acceptor.local_endpoint ().port () % ec.message ()));
		throw std::runtime_error (ec.message ());
	}

	on_connection (callback_a);
}

void nano::transport::tcp_listener::stop ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	on = false;
	for (auto & i: connections)
	{
		if (auto connection = i.second.lock ())
		{
			connection->stop ();
		}
	}
	connections.clear ();
	boost::asio::dispatch (strand, boost::asio::bind_executor (strand, [this_l = shared_from_this ()] () {
		this_l->acceptor.close ();
		for (auto & address_connection_pair : this_l->connections_per_address)
		{
			if (auto connection_l = address_connection_pair.second.lock ())
			{
				connection_l->close ();
			}
		}
		this_l->connections_per_address.clear ();
	}));
}

std::size_t nano::transport::tcp_listener::connection_count ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return connections.size ();
}

bool nano::transport::tcp_listener::limit_reached_for_incoming_subnetwork_connections (std::shared_ptr<nano::transport::socket> const & new_connection)
{
	debug_assert (strand.running_in_this_thread ());
	if (node.flags.disable_max_peers_per_subnetwork || nano::transport::is_ipv4_or_v4_mapped_address (new_connection->remote.address ()))
	{
		// If the limit is disabled, then it is unreachable.
		// If the address is IPv4 we don't check for a network limit, since its address space isn't big as IPv6 /64.
		return false;
	}
	auto const counted_connections = socket_functions::count_subnetwork_connections (
	connections_per_address,
	new_connection->remote.address ().to_v6 (),
	node.network_params.network.ipv6_subnetwork_prefix_for_limiting);
	return counted_connections >= node.network_params.network.max_peers_per_subnetwork;
}

bool nano::transport::tcp_listener::limit_reached_for_incoming_ip_connections (std::shared_ptr<nano::transport::socket> const & new_connection)
{
	debug_assert (strand.running_in_this_thread ());
	if (node.flags.disable_max_peers_per_ip)
	{
		// If the limit is disabled, then it is unreachable.
		return false;
	}
	auto const address_connections_range = connections_per_address.equal_range (new_connection->remote.address ());
	auto const counted_connections = static_cast<std::size_t> (std::abs (std::distance (address_connections_range.first, address_connections_range.second)));
	return counted_connections >= node.network_params.network.max_peers_per_ip;
}

void nano::transport::tcp_listener::on_connection (std::function<bool (std::shared_ptr<nano::transport::socket> const &, boost::system::error_code const &)> callback_a)
{
	boost::asio::post (strand, boost::asio::bind_executor (strand, [this_l = shared_from_this (), callback = std::move (callback_a)] () mutable {
		if (!this_l->acceptor.is_open ())
		{
			this_l->node.logger.always_log ("Network: Acceptor is not open");
			return;
		}

		// Prepare new connection
		auto new_connection = std::make_shared<nano::transport::socket> (this_l->node, socket::endpoint_type_t::server);
		this_l->acceptor.async_accept (new_connection->tcp_socket, new_connection->remote,
		boost::asio::bind_executor (this_l->strand,
		[this_l, new_connection, cbk = std::move (callback)] (boost::system::error_code const & ec_a) mutable {
			this_l->evict_dead_connections ();

			if (this_l->connections_per_address.size () >= this_l->max_inbound_connections)
			{
				this_l->node.logger.try_log ("Network: max_inbound_connections reached, unable to open new connection");
				this_l->node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_accept_failure, nano::stat::dir::in);
				this_l->on_connection_requeue_delayed (std::move (cbk));
				return;
			}

			if (this_l->limit_reached_for_incoming_ip_connections (new_connection))
			{
				auto const remote_ip_address = new_connection->remote_endpoint ().address ();
				auto const log_message = boost::str (
				boost::format ("Network: max connections per IP (max_peers_per_ip) was reached for %1%, unable to open new connection")
				% remote_ip_address.to_string ());
				this_l->node.logger.try_log (log_message);
				this_l->node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_max_per_ip, nano::stat::dir::in);
				this_l->on_connection_requeue_delayed (std::move (cbk));
				return;
			}

			if (this_l->limit_reached_for_incoming_subnetwork_connections (new_connection))
			{
				auto const remote_ip_address = new_connection->remote_endpoint ().address ();
				debug_assert (remote_ip_address.is_v6 ());
				auto const remote_subnet = socket_functions::get_ipv6_subnet_address (remote_ip_address.to_v6 (), this_l->node.network_params.network.max_peers_per_subnetwork);
				auto const log_message = boost::str (
				boost::format ("Network: max connections per subnetwork (max_peers_per_subnetwork) was reached for subnetwork %1% (remote IP: %2%), unable to open new connection")
				% remote_subnet.canonical ().to_string ()
				% remote_ip_address.to_string ());
				this_l->node.logger.try_log (log_message);
				this_l->node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_max_per_subnetwork, nano::stat::dir::in);
				this_l->on_connection_requeue_delayed (std::move (cbk));
				return;
			}

			if (!ec_a)
			{
				// Make sure the new connection doesn't idle. Note that in most cases, the callback is going to start
				// an IO operation immediately, which will start a timer.
				new_connection->start ();
				new_connection->set_timeout (this_l->node.network_params.network.idle_timeout);
				this_l->node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_accept_success, nano::stat::dir::in);
				this_l->connections_per_address.emplace (new_connection->remote.address (), new_connection);
				this_l->node.observers.socket_accepted.notify (*new_connection);
				if (cbk (new_connection, ec_a))
				{
					this_l->on_connection (std::move (cbk));
					return;
				}
				this_l->node.logger.always_log ("Network: Stopping to accept connections");
				return;
			}

			// accept error
			this_l->node.logger.try_log ("Network: Unable to accept connection: ", ec_a.message ());
			this_l->node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_accept_failure, nano::stat::dir::in);

			if (is_temporary_error (ec_a))
			{
				// if it is a temporary error, just retry it
				this_l->on_connection_requeue_delayed (std::move (cbk));
				return;
			}

			// if it is not a temporary error, check how the listener wants to handle this error
			if (cbk (new_connection, ec_a))
			{
				this_l->on_connection_requeue_delayed (std::move (cbk));
				return;
			}

			// No requeue if we reach here, no incoming socket connections will be handled
			this_l->node.logger.always_log ("Network: Stopping to accept connections");
		}));
	}));
}

// If we are unable to accept a socket, for any reason, we wait just a little (1ms) before rescheduling the next connection accept.
// The intention is to throttle back the connection requests and break up any busy loops that could possibly form and
// give the rest of the system a chance to recover.
void nano::transport::tcp_listener::on_connection_requeue_delayed (std::function<bool (std::shared_ptr<nano::transport::socket> const &, boost::system::error_code const &)> callback_a)
{
	node.workers.add_timed_task (std::chrono::steady_clock::now () + std::chrono::milliseconds (1), [this_l = shared_from_this (), callback = std::move (callback_a)] () mutable {
		this_l->on_connection (std::move (callback));
	});
}

// This must be called from a strand
void nano::transport::tcp_listener::evict_dead_connections ()
{
	debug_assert (strand.running_in_this_thread ());
	for (auto it = connections_per_address.begin (); it != connections_per_address.end ();)
	{
		if (it->second.expired ())
		{
			it = connections_per_address.erase (it);
			continue;
		}
		++it;
	}
}

void nano::transport::tcp_listener::accept_action (boost::system::error_code const & ec, std::shared_ptr<nano::transport::socket> const & socket_a)
{
	if (!node.network.excluded_peers.check (socket_a->remote_endpoint ()))
	{
		auto server = std::make_shared<nano::transport::tcp_server> (socket_a, node.shared (), true);
		nano::lock_guard<nano::mutex> lock{ mutex };
		connections[server.get ()] = server;
		server->start ();
	}
	else
	{
		node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_excluded);
		if (node.config.logging.network_rejected_logging ())
		{
			node.logger.try_log ("Rejected connection from excluded peer ", socket_a->remote_endpoint ());
		}
	}
}

boost::asio::ip::tcp::endpoint nano::transport::tcp_listener::endpoint ()
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	if (on)
	{
		return boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::loopback (), acceptor.local_endpoint ().port ());
	}
	else
	{
		return boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::loopback (), 0);
	}
}

std::unique_ptr<nano::container_info_component> nano::transport::collect_container_info (nano::transport::tcp_listener & bootstrap_listener, std::string const & name)
{
	auto sizeof_element = sizeof (decltype (bootstrap_listener.connections)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "connections", bootstrap_listener.connection_count (), sizeof_element }));
	return composite;
}

/*
 * tcp_server
 */

nano::transport::tcp_server::tcp_server (std::shared_ptr<nano::transport::socket> socket_a, std::shared_ptr<nano::node> node_a, bool allow_bootstrap_a) :
	socket{ std::move (socket_a) },
	node{ std::move (node_a) },
	allow_bootstrap{ allow_bootstrap_a },
	message_deserializer{
		std::make_shared<nano::transport::message_deserializer> (node_a->network_params.network, node_a->network.publish_filter, node_a->block_uniquer, node_a->vote_uniquer,
		[socket_l = socket] (std::shared_ptr<std::vector<uint8_t>> const & data_a, size_t size_a, std::function<void (boost::system::error_code const &, std::size_t)> callback_a) {
			debug_assert (socket_l != nullptr);
			socket_l->read_impl (data_a, size_a, callback_a);
		})
	}
{
	debug_assert (socket != nullptr);
}

nano::transport::tcp_server::~tcp_server ()
{
	auto node = this->node.lock ();
	if (!node)
	{
		return;
	}
	if (node->config.logging.bulk_pull_logging ())
	{
		node->logger.try_log ("Exiting incoming TCP/bootstrap server");
	}

	if (socket->type () == nano::transport::socket::type_t::bootstrap)
	{
		--node->tcp_listener->bootstrap_count;
	}
	else if (socket->type () == nano::transport::socket::type_t::realtime)
	{
		--node->tcp_listener->realtime_count;

		// Clear temporary channel
		auto exisiting_response_channel (node->network.tcp_channels.find_channel (remote_endpoint));
		if (exisiting_response_channel != nullptr)
		{
			exisiting_response_channel->temporary = false;
			node->network.tcp_channels.erase (remote_endpoint);
		}
	}

	stop ();

	nano::lock_guard<nano::mutex> lock{ node->tcp_listener->mutex };
	node->tcp_listener->connections.erase (this);
}

void nano::transport::tcp_server::start ()
{
	// Set remote_endpoint
	if (remote_endpoint.port () == 0)
	{
		remote_endpoint = socket->remote_endpoint ();
		debug_assert (remote_endpoint.port () != 0);
	}
	receive_message ();
}

void nano::transport::tcp_server::stop ()
{
	if (!stopped.exchange (true))
	{
		socket->close ();
	}
}

void nano::transport::tcp_server::receive_message ()
{
	if (stopped)
	{
		return;
	}

	message_deserializer->read ([this_l = shared_from_this ()] (boost::system::error_code ec, std::unique_ptr<nano::message> message) {
		auto node = this_l->node.lock ();
		if (!node)
		{
			return;
		}
		if (ec)
		{
			// IO error or critical error when deserializing message
			node->stats.inc (nano::stat::type::error, nano::transport::message_deserializer::to_stat_detail (this_l->message_deserializer->status));
			this_l->stop ();
		}
		else
		{
			this_l->received_message (std::move (message));
		}
	});
}

void nano::transport::tcp_server::received_message (std::unique_ptr<nano::message> message)
{
	auto node = this->node.lock ();
	if (!node)
	{
		return;
	}
	bool should_continue = true;
	if (message)
	{
		should_continue = process_message (std::move (message));
	}
	else
	{
		// Error while deserializing message
		debug_assert (message_deserializer->status != transport::message_deserializer::parse_status::success);
		node->stats.inc (nano::stat::type::error, nano::transport::message_deserializer::to_stat_detail (message_deserializer->status));
		if (message_deserializer->status == transport::message_deserializer::parse_status::duplicate_publish_message)
		{
			node->stats.inc (nano::stat::type::filter, nano::stat::detail::duplicate_publish);
		}
	}

	if (should_continue)
	{
		receive_message ();
	}
}

bool nano::transport::tcp_server::process_message (std::unique_ptr<nano::message> message)
{
	auto node = this->node.lock ();
	if (!node)
	{
		return false;
	}
	node->stats.inc (nano::stat::type::tcp_server, nano::to_stat_detail (message->header.type), nano::stat::dir::in);

	debug_assert (is_undefined_connection () || is_realtime_connection () || is_bootstrap_connection ());

	/*
	 * Server initially starts in undefined state, where it waits for either a handshake or booststrap request message
	 * If the server receives a handshake (and it is successfully validated) it will switch to a realtime mode.
	 * In realtime mode messages are deserialized and queued to `tcp_message_manager` for further processing.
	 * In realtime mode any bootstrap requests are ignored.
	 *
	 * If the server receives a bootstrap request before receiving a handshake, it will switch to a bootstrap mode.
	 * In bootstrap mode once a valid bootstrap request message is received, the server will start a corresponding bootstrap server and pass control to that server.
	 * Once that server finishes its task, control is passed back to this server to read and process any subsequent messages.
	 * In bootstrap mode any realtime messages are ignored
	 */
	if (is_undefined_connection ())
	{
		handshake_message_visitor handshake_visitor{ shared_from_this () };
		message->visit (handshake_visitor);
		if (handshake_visitor.process)
		{
			queue_realtime (std::move (message));
			return true;
		}
		else if (handshake_visitor.bootstrap)
		{
			if (!to_bootstrap_connection ())
			{
				stop ();
				return false;
			}
		}
		else
		{
			// Neither handshake nor bootstrap received when in handshake mode
			return true;
		}
	}
	else if (is_realtime_connection ())
	{
		realtime_message_visitor realtime_visitor{ *this };
		message->visit (realtime_visitor);
		if (realtime_visitor.process)
		{
			queue_realtime (std::move (message));
		}
		return true;
	}
	// the server will switch to bootstrap mode immediately after processing the first bootstrap message, thus no `else if`
	if (is_bootstrap_connection ())
	{
		bootstrap_message_visitor bootstrap_visitor{ shared_from_this () };
		message->visit (bootstrap_visitor);
		return !bootstrap_visitor.processed; // Stop receiving new messages if bootstrap serving started
	}
	debug_assert (false);
	return true; // Continue receiving new messages
}

void nano::transport::tcp_server::queue_realtime (std::unique_ptr<nano::message> message)
{
	auto node = this->node.lock ();
	if (!node)
	{
		return;
	}
	node->network.tcp_message_manager.put_message (nano::tcp_message_item{ std::move (message), remote_endpoint, remote_node_id, socket });
}

/*
 * Handshake
 */

nano::transport::tcp_server::handshake_message_visitor::handshake_message_visitor (std::shared_ptr<tcp_server> server) :
	server{ std::move (server) }
{
}

void nano::transport::tcp_server::handshake_message_visitor::node_id_handshake (nano::node_id_handshake const & message)
{
	auto node = server->node.lock ();
	if (!node)
	{
		return;
	}
	if (node->flags.disable_tcp_realtime)
	{
		if (node->config.logging.network_node_id_handshake_logging ())
		{
			node->logger.try_log (boost::str (boost::format ("Disabled realtime TCP for handshake %1%") % server->remote_endpoint));
		}
		// Stop invalid handshake
		server->stop ();
		return;
	}

	if (message.query && server->handshake_query_received)
	{
		if (node->config.logging.network_node_id_handshake_logging ())
		{
			node->logger.try_log (boost::str (boost::format ("Detected multiple node_id_handshake query from %1%") % server->remote_endpoint));
		}
		// Stop invalid handshake
		server->stop ();
		return;
	}

	server->handshake_query_received = true;

	if (node->config.logging.network_node_id_handshake_logging ())
	{
		node->logger.try_log (boost::str (boost::format ("Received node_id_handshake message from %1%") % server->remote_endpoint));
	}

	if (message.query)
	{
		server->send_handshake_response (*message.query, message.is_v2 ());
	}
	if (message.response)
	{
		if (node->network.verify_handshake_response (*message.response, nano::transport::map_tcp_to_endpoint (server->remote_endpoint)))
		{
			server->to_realtime_connection (message.response->node_id);
		}
		else
		{
			// Stop invalid handshake
			server->stop ();
			return;
		}
	}

	process = true;
}

void nano::transport::tcp_server::send_handshake_response (nano::node_id_handshake::query_payload const & query, bool v2)
{
	auto node = this->node.lock ();
	if (!node)
	{
		return;
	}
	auto response = node->network.prepare_handshake_response (query, v2);
	auto own_query = node->network.prepare_handshake_query (nano::transport::map_tcp_to_endpoint (remote_endpoint));
	nano::node_id_handshake handshake_response{ node->network_params.network, own_query, response };

	// TODO: Use channel
	auto shared_const_buffer = handshake_response.to_shared_const_buffer ();
	socket->async_write (shared_const_buffer, [this_l = shared_from_this ()] (boost::system::error_code const & ec, std::size_t size_a) {
		auto node = this_l->node.lock ();
		if (!node)
		{
			return;
		}
		if (ec)
		{
			if (node->config.logging.network_node_id_handshake_logging ())
			{
				node->logger.try_log (boost::str (boost::format ("Error sending node_id_handshake to %1%: %2%") % this_l->remote_endpoint % ec.message ()));
			}
			// Stop invalid handshake
			this_l->stop ();
		}
		else
		{
			node->stats.inc (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::out);
		}
	});
}

void nano::transport::tcp_server::handshake_message_visitor::bulk_pull (const nano::bulk_pull & message)
{
	bootstrap = true;
}

void nano::transport::tcp_server::handshake_message_visitor::bulk_pull_account (const nano::bulk_pull_account & message)
{
	bootstrap = true;
}

void nano::transport::tcp_server::handshake_message_visitor::bulk_push (const nano::bulk_push & message)
{
	bootstrap = true;
}

void nano::transport::tcp_server::handshake_message_visitor::frontier_req (const nano::frontier_req & message)
{
	bootstrap = true;
}

/*
 * Realtime
 */

nano::transport::tcp_server::realtime_message_visitor::realtime_message_visitor (nano::transport::tcp_server & server_a) :
	server{ server_a }
{
}

void nano::transport::tcp_server::realtime_message_visitor::keepalive (const nano::keepalive & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::publish (const nano::publish & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::confirm_req (const nano::confirm_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::confirm_ack (const nano::confirm_ack & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::frontier_req (const nano::frontier_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::telemetry_req (const nano::telemetry_req & message)
{
	auto node = server.node.lock ();
	if (!node)
	{
		return;
	}
	// Only handle telemetry requests if they are outside the cooldown period
	if (server.last_telemetry_req + node->network_params.network.telemetry_request_cooldown < std::chrono::steady_clock::now ())
	{
		server.last_telemetry_req = std::chrono::steady_clock::now ();
		process = true;
	}
	else
	{
		node->stats.inc (nano::stat::type::telemetry, nano::stat::detail::request_within_protection_cache_zone);
	}
}

void nano::transport::tcp_server::realtime_message_visitor::telemetry_ack (const nano::telemetry_ack & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::asc_pull_req (const nano::asc_pull_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::asc_pull_ack (const nano::asc_pull_ack & message)
{
	process = true;
}

/*
 * Bootstrap
 */

nano::transport::tcp_server::bootstrap_message_visitor::bootstrap_message_visitor (std::shared_ptr<tcp_server> server) :
	server{ std::move (server) }
{
}

void nano::transport::tcp_server::bootstrap_message_visitor::bulk_pull (const nano::bulk_pull & message)
{
	auto node = server->node.lock ();
	if (!node)
	{
		return;
	}
	if (node->flags.disable_bootstrap_bulk_pull_server)
	{
		return;
	}

	if (node->config.logging.bulk_pull_logging ())
	{
		node->logger.try_log (boost::str (boost::format ("Received bulk pull for %1% down to %2%, maximum of %3% from %4%") % message.start.to_string () % message.end.to_string () % message.count % server->remote_endpoint));
	}

	node->bootstrap_workers.push_task ([server = server, message = message] () {
		// TODO: Add completion callback to bulk pull server
		// TODO: There should be no need to re-copy message as unique pointer, refactor those bulk/frontier pull/push servers
		auto bulk_pull_server = std::make_shared<nano::bulk_pull_server> (server, std::make_unique<nano::bulk_pull> (message));
		bulk_pull_server->send_next ();
	});

	processed = true;
}

void nano::transport::tcp_server::bootstrap_message_visitor::bulk_pull_account (const nano::bulk_pull_account & message)
{
	auto node = server->node.lock ();
	if (!node)
	{
		return;
	}
	if (node->flags.disable_bootstrap_bulk_pull_server)
	{
		return;
	}

	if (node->config.logging.bulk_pull_logging ())
	{
		node->logger.try_log (boost::str (boost::format ("Received bulk pull account for %1% with a minimum amount of %2%") % message.account.to_account () % nano::amount (message.minimum_amount).format_balance (nano::Mxrb_ratio, 10, true)));
	}

	node->bootstrap_workers.push_task ([server = server, message = message] () {
		// TODO: Add completion callback to bulk pull server
		// TODO: There should be no need to re-copy message as unique pointer, refactor those bulk/frontier pull/push servers
		auto bulk_pull_account_server = std::make_shared<nano::bulk_pull_account_server> (server, std::make_unique<nano::bulk_pull_account> (message));
		bulk_pull_account_server->send_frontier ();
	});

	processed = true;
}

void nano::transport::tcp_server::bootstrap_message_visitor::bulk_push (const nano::bulk_push &)
{
	auto node = server->node.lock ();
	if (!node)
	{
		return;
	}
	node->bootstrap_workers.push_task ([server = server] () {
		// TODO: Add completion callback to bulk pull server
		auto bulk_push_server = std::make_shared<nano::bulk_push_server> (server);
		bulk_push_server->throttled_receive ();
	});

	processed = true;
}

void nano::transport::tcp_server::bootstrap_message_visitor::frontier_req (const nano::frontier_req & message)
{
	auto node = server->node.lock ();
	if (!node)
	{
		return;
	}
	if (node->config.logging.bulk_pull_logging ())
	{
		node->logger.try_log (boost::str (boost::format ("Received frontier request for %1% with age %2%") % message.start.to_string () % message.age));
	}

	node->bootstrap_workers.push_task ([server = server, message = message] () {
		// TODO: There should be no need to re-copy message as unique pointer, refactor those bulk/frontier pull/push servers
		auto response = std::make_shared<nano::frontier_req_server> (server, std::make_unique<nano::frontier_req> (message));
		response->send_next ();
	});

	processed = true;
}

// TODO: We could periodically call this (from a dedicated timeout thread for eg.) but socket already handles timeouts,
//  and since we only ever store tcp_server as weak_ptr, socket timeout will automatically trigger tcp_server cleanup
void nano::transport::tcp_server::timeout ()
{
	auto node = this->node.lock ();
	if (!node)
	{
		return;
	}
	if (socket->has_timed_out ())
	{
		if (node->config.logging.bulk_pull_logging ())
		{
			node->logger.try_log ("Closing incoming tcp / bootstrap server by timeout");
		}
		{
			nano::lock_guard<nano::mutex> lock{ node->tcp_listener->mutex };
			node->tcp_listener->connections.erase (this);
		}
		socket->close ();
	}
}

bool nano::transport::tcp_server::to_bootstrap_connection ()
{
	auto node = this->node.lock ();
	if (!node)
	{
		return false;
	}
	if (!allow_bootstrap)
	{
		return false;
	}
	if (node->flags.disable_bootstrap_listener)
	{
		return false;
	}
	if (node->tcp_listener->bootstrap_count >= node->config.bootstrap_connections_max)
	{
		return false;
	}
	if (socket->type () != nano::transport::socket::type_t::undefined)
	{
		return false;
	}

	++node->tcp_listener->bootstrap_count;
	socket->type_set (nano::transport::socket::type_t::bootstrap);
	return true;
}

bool nano::transport::tcp_server::to_realtime_connection (nano::account const & node_id)
{
	auto node = this->node.lock ();
	if (!node)
	{
		return false;
	}
	if (socket->type () == nano::transport::socket::type_t::undefined && !node->flags.disable_tcp_realtime)
	{
		remote_node_id = node_id;
		++node->tcp_listener->realtime_count;
		socket->type_set (nano::transport::socket::type_t::realtime);
		return true;
	}
	return false;
}

bool nano::transport::tcp_server::is_undefined_connection () const
{
	return socket->type () == nano::transport::socket::type_t::undefined;
}

bool nano::transport::tcp_server::is_bootstrap_connection () const
{
	return socket->is_bootstrap_connection ();
}

bool nano::transport::tcp_server::is_realtime_connection () const
{
	return socket->is_realtime_connection ();
}
