#include <nano/lib/jsonconfig.hpp>
#include <nano/node/election.hpp>
#include <nano/node/testing.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <numeric>

using namespace std::chrono_literals;

namespace nano
{
TEST (active_transactions, confirm_active)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node1 = *system.add_node (node_flags);
	nano::genesis genesis;
	auto send = nano::send_block_builder ()
				.previous (genesis.hash ())
				.destination (nano::public_key ())
				.balance (nano::genesis_amount - 100)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (genesis.hash ()))
				.build_shared ();
	ASSERT_EQ (nano::process_result::progress, node1.process (*send).code);
	nano::node_config node_config2 (nano::get_available_port (), system.logging);
	node_config2.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	nano::node_flags node_flags2;
	// The rep crawler would otherwise request confirmations in order to find representatives
	node_flags2.disable_rep_crawler = true;
	auto & node2 = *system.add_node (node_config2, node_flags2);
	system.deadline_set (5s);
	// Let node2 know about the block
	while (node2.active.empty ())
	{
		node1.network.flood_block (send, nano::buffer_drop_policy::no_limiter_drop);
		ASSERT_NO_ERROR (system.poll ());
	}
	// Save election to check request count afterwards
	auto election = node2.active.election (send->qualified_root ());
	ASSERT_NE (nullptr, election);
	// Add key to node1
	system.wallet (0)->insert_adhoc (nano::dev_genesis_key.prv);
	// Add representative to disabled rep crawler
	auto peers (node2.network.random_set (1));
	ASSERT_FALSE (peers.empty ());
	{
		nano::lock_guard<nano::mutex> guard (node2.rep_crawler.probable_reps_mutex);
		node2.rep_crawler.probable_reps.emplace (nano::dev_genesis_key.pub, nano::genesis_amount, *peers.begin ());
	}
	ASSERT_TIMELY (5s, election->votes ().size () != 1); // Votes were inserted (except for not_an_account)
	auto confirm_req_count (election->confirmation_request_count.load ());
	// At least one confirmation request
	ASSERT_GT (confirm_req_count, 0u);
	ASSERT_TRUE (election->confirmed ());
	// Waiting for final confirmation
	ASSERT_TIMELY (10s, node2.ledger.cache.cemented_count == 2 && node2.active.empty ());
	// Blocks were cleared (except for not_an_account)
	ASSERT_EQ (1, election->blocks ().size ());
}
}

namespace nano
{
TEST (active_transactions, confirm_frontier)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_request_loop = true;
	// Voting node
	auto & node1 = *system.add_node (node_flags);
	nano::node_flags node_flags2;
	// The rep crawler would otherwise request confirmations in order to find representatives
	node_flags2.disable_rep_crawler = true;
	auto & node2 = *system.add_node (node_flags2);

	// Add key to node1
	system.wallet (0)->insert_adhoc (nano::dev_genesis_key.prv);
	// Add representative to disabled rep crawler
	auto peers (node2.network.random_set (1));
	ASSERT_FALSE (peers.empty ());
	{
		nano::lock_guard<nano::mutex> guard (node2.rep_crawler.probable_reps_mutex);
		node2.rep_crawler.probable_reps.emplace (nano::dev_genesis_key.pub, nano::genesis_amount, *peers.begin ());
	}

	nano::genesis genesis;
	nano::state_block_builder builder;
	auto send = builder
				.account (nano::dev_genesis_key.pub)
				.previous (genesis.hash ())
				.representative (nano::dev_genesis_key.pub)
				.balance (nano::genesis_amount - 100)
				.link (nano::public_key ())
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (genesis.hash ()))
				.build_shared ();
	auto send_copy = builder.make_block ().from (*send).build_shared ();
	ASSERT_EQ (nano::process_result::progress, node1.process (*send).code);
	node1.confirmation_height_processor.add (send);
	ASSERT_TIMELY (5s, node1.ledger.block_confirmed (node1.store.tx_begin_read (), send->hash ()));
	ASSERT_EQ (nano::process_result::progress, node2.process (*send_copy).code);
	ASSERT_TIMELY (5s, !node2.active.empty ());
	// Save election to check request count afterwards
	auto election2 = node2.active.election (send->qualified_root ());
	ASSERT_NE (nullptr, election2);
	ASSERT_TIMELY (5s, node2.ledger.cache.cemented_count == 2 && node2.active.empty ());
	ASSERT_GT (election2->confirmation_request_count, 0u);
}
}

TEST (active_transactions, keep_local)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.enable_voting = false;
	node_config.active_elections_size = 2; //bound to 2, wont drop wallet created transactions, but good to test dropping remote
	// Disable frontier confirmation to allow the test to finish before
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	auto & wallet (*system.wallet (0));
	nano::genesis genesis;
	//key 1/2 will be managed by the wallet
	nano::keypair key1, key2, key3, key4, key5, key6;
	wallet.insert_adhoc (nano::dev_genesis_key.prv);
	auto send1 (wallet.send_action (nano::dev_genesis_key.pub, key1.pub, node.config.receive_minimum.number ()));
	auto send2 (wallet.send_action (nano::dev_genesis_key.pub, key2.pub, node.config.receive_minimum.number ()));
	auto send3 (wallet.send_action (nano::dev_genesis_key.pub, key3.pub, node.config.receive_minimum.number ()));
	auto send4 (wallet.send_action (nano::dev_genesis_key.pub, key4.pub, node.config.receive_minimum.number ()));
	auto send5 (wallet.send_action (nano::dev_genesis_key.pub, key5.pub, node.config.receive_minimum.number ()));
	auto send6 (wallet.send_action (nano::dev_genesis_key.pub, key6.pub, node.config.receive_minimum.number ()));
	// should not drop wallet created transactions
	ASSERT_TIMELY (5s, node.active.size () == 1);
	for (auto const & block : { send1, send2, send3, send4, send5, send6 })
	{
		ASSERT_TIMELY (1s, node.active.election (block->qualified_root ()));
		auto election = node.active.election (block->qualified_root ());
		ASSERT_NE (nullptr, election);
		election->force_confirm ();
	}
	ASSERT_TIMELY (5s, node.active.empty ());
	nano::state_block_builder builder;
	auto open1 = builder.make_block ()
				 .account (key1.pub)
				 .previous (0)
				 .representative (key1.pub)
				 .balance (node.config.receive_minimum.number ())
				 .link (send1->hash ())
				 .sign (key1.prv, key1.pub)
				 .work (*system.work.generate (key1.pub))
				 .build_shared ();
	auto open2 = builder.make_block ()
				 .account (key2.pub)
				 .previous (0)
				 .representative (key2.pub)
				 .balance (node.config.receive_minimum.number ())
				 .link (send2->hash ())
				 .sign (key2.prv, key2.pub)
				 .work (*system.work.generate (key2.pub))
				 .build_shared ();
	auto open3 = builder.make_block ()
				 .account (key3.pub)
				 .previous (0)
				 .representative (key3.pub)
				 .balance (node.config.receive_minimum.number ())
				 .link (send3->hash ())
				 .sign (key3.prv, key3.pub)
				 .work (*system.work.generate (key3.pub))
				 .build_shared ();
	node.process_active (open1);
	node.process_active (open2);
	node.process_active (open3);
	node.block_processor.flush ();
	// bound elections, should drop after one loop
	ASSERT_TIMELY (1s, node.active.size () == node_config.active_elections_size);
	ASSERT_EQ (1, node.scheduler.size ());
}

TEST (active_transactions, inactive_votes_cache)
{
	nano::system system (1);
	auto & node = *system.nodes[0];
	nano::block_hash latest (node.latest (nano::dev_genesis_key.pub));
	nano::keypair key;
	auto send = nano::send_block_builder ()
				.previous (latest)
				.destination (key.pub)
				.balance (nano::genesis_amount - 100)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (latest))
				.build_shared ();
	auto vote (std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, std::numeric_limits<uint64_t>::max (), std::vector<nano::block_hash> (1, send->hash ())));
	node.vote_processor.vote (vote, std::make_shared<nano::transport::channel_loopback> (node));
	ASSERT_TIMELY (5s, node.active.inactive_votes_cache_size () == 1);
	node.process_active (send);
	node.block_processor.flush ();
	ASSERT_TIMELY (5s, node.ledger.block_confirmed (node.store.tx_begin_read (), send->hash ()));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::vote_cached));
}

TEST (active_transactions, inactive_votes_cache_fork)
{
	nano::system system (1);
	auto & node = *system.nodes[0];
	nano::block_hash latest (node.latest (nano::dev_genesis_key.pub));
	nano::keypair key;
	nano::send_block_builder builder;
	auto send1 = builder.make_block ()
				 .previous (latest)
				 .destination (key.pub)
				 .balance (nano::genesis_amount - 100)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (latest))
				 .build_shared ();
	auto send2 = builder.make_block ()
				 .previous (latest)
				 .destination (key.pub)
				 .balance (nano::genesis_amount - 200)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (latest))
				 .build_shared ();
	auto vote (std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, std::numeric_limits<uint64_t>::max (), std::vector<nano::block_hash> (1, send1->hash ())));
	node.vote_processor.vote (vote, std::make_shared<nano::transport::channel_loopback> (node));
	auto channel1 (node.network.udp_channels.create (node.network.endpoint ()));
	ASSERT_TIMELY (5s, node.active.inactive_votes_cache_size () == 1);
	node.network.process_message (nano::publish (send2), channel1);
	node.block_processor.flush ();
	ASSERT_NE (nullptr, node.block (send2->hash ()));
	node.network.process_message (nano::publish (send1), channel1);
	node.block_processor.flush ();
	bool confirmed (false);
	system.deadline_set (5s);
	while (!confirmed)
	{
		auto transaction (node.store.tx_begin_read ());
		confirmed = node.block (send1->hash ()) != nullptr && node.ledger.block_confirmed (transaction, send1->hash ()) && node.active.empty ();
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::vote_cached));
}

TEST (active_transactions, inactive_votes_cache_existing_vote)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	nano::block_hash latest (node.latest (nano::dev_genesis_key.pub));
	nano::keypair key;
	nano::block_builder builder;
	auto send = builder.send ()
				.previous (latest)
				.destination (key.pub)
				.balance (nano::genesis_amount - 100 * nano::Gxrb_ratio)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (latest))
				.build_shared ();
	auto open = builder.state ()
				.account (key.pub)
				.previous (0)
				.representative (key.pub)
				.balance (100 * nano::Gxrb_ratio)
				.link (send->hash ())
				.sign (key.prv, key.pub)
				.work (*system.work.generate (key.pub))
				.build_shared ();
	node.process_active (send);
	node.block_processor.add (open);
	node.block_processor.flush ();
	ASSERT_TIMELY (5s, node.active.size () == 1);
	auto election (node.active.election (send->qualified_root ()));
	ASSERT_NE (nullptr, election);
	ASSERT_GT (node.weight (key.pub), node.minimum_principal_weight ());
	// Insert vote
	auto vote1 (std::make_shared<nano::vote> (key.pub, key.prv, 1, std::vector<nano::block_hash> (1, send->hash ())));
	node.vote_processor.vote (vote1, std::make_shared<nano::transport::channel_loopback> (node));
	ASSERT_TIMELY (5s, election->votes ().size () == 2)
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::vote_new));
	auto last_vote1 (election->votes ()[key.pub]);
	ASSERT_EQ (send->hash (), last_vote1.hash);
	ASSERT_EQ (1, last_vote1.timestamp);
	// Attempt to change vote with inactive_votes_cache
	nano::unique_lock<nano::mutex> active_lock (node.active.mutex);
	node.active.add_inactive_votes_cache (active_lock, send->hash (), key.pub, 0);
	active_lock.unlock ();
	auto cache (node.active.find_inactive_votes_cache (send->hash ()));
	active_lock.lock ();
	ASSERT_EQ (1, cache.voters.size ());
	election->insert_inactive_votes_cache (cache);
	// Check that election data is not changed
	ASSERT_EQ (2, election->votes ().size ());
	auto last_vote2 (election->votes ()[key.pub]);
	ASSERT_EQ (last_vote1.hash, last_vote2.hash);
	ASSERT_EQ (last_vote1.timestamp, last_vote2.timestamp);
	ASSERT_EQ (last_vote1.time, last_vote2.time);
	ASSERT_EQ (0, node.stats.count (nano::stat::type::election, nano::stat::detail::vote_cached));
}

TEST (active_transactions, inactive_votes_cache_multiple_votes)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	nano::block_hash latest (node.latest (nano::dev_genesis_key.pub));
	nano::keypair key1;
	nano::block_builder builder;
	auto send1 = builder.send ()
				 .previous (latest)
				 .destination (key1.pub)
				 .balance (nano::genesis_amount - 100 * nano::Gxrb_ratio)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (latest))
				 .build_shared ();
	auto send2 = builder.send ()
				 .previous (send1->hash ())
				 .destination (key1.pub)
				 .balance (100 * nano::Gxrb_ratio)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send1->hash ()))
				 .build_shared ();
	auto open = builder.state ()
				.account (key1.pub)
				.previous (0)
				.representative (key1.pub)
				.balance (100 * nano::Gxrb_ratio)
				.link (send1->hash ())
				.sign (key1.prv, key1.pub)
				.work (*system.work.generate (key1.pub))
				.build_shared ();
	node.block_processor.add (send1);
	node.block_processor.add (send2);
	node.block_processor.add (open);
	node.block_processor.flush ();
	// Process votes
	auto vote1 (std::make_shared<nano::vote> (key1.pub, key1.prv, 0, std::vector<nano::block_hash> (1, send1->hash ())));
	node.vote_processor.vote (vote1, std::make_shared<nano::transport::channel_loopback> (node));
	auto vote2 (std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, 0, std::vector<nano::block_hash> (1, send1->hash ())));
	node.vote_processor.vote (vote2, std::make_shared<nano::transport::channel_loopback> (node));
	ASSERT_TIMELY (5s, node.active.find_inactive_votes_cache (send1->hash ()).voters.size () == 2);
	ASSERT_EQ (1, node.active.inactive_votes_cache_size ());
	node.scheduler.activate (nano::dev_genesis_key.pub, node.store.tx_begin_read ());
	node.scheduler.flush ();
	auto election = node.active.election (send1->qualified_root ());
	ASSERT_NE (nullptr, election);
	ASSERT_EQ (3, election->votes ().size ()); // 2 votes and 1 default not_an_acount
	ASSERT_EQ (2, node.stats.count (nano::stat::type::election, nano::stat::detail::vote_cached));
}

TEST (active_transactions, inactive_votes_cache_election_start)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	nano::block_hash latest (node.latest (nano::dev_genesis_key.pub));
	nano::keypair key1, key2, key3, key4, key5;
	nano::send_block_builder send_block_builder;
	nano::state_block_builder state_block_builder;
	auto send1 = send_block_builder.make_block ()
				 .previous (latest)
				 .destination (key1.pub)
				 .balance (nano::genesis_amount - 2000 * nano::Gxrb_ratio)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (latest))
				 .build_shared ();
	auto send2 = send_block_builder.make_block ()
				 .previous (send1->hash ())
				 .destination (key2.pub)
				 .balance (nano::genesis_amount - 4000 * nano::Gxrb_ratio)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send1->hash ()))
				 .build_shared ();
	auto send3 = send_block_builder.make_block ()
				 .previous (send2->hash ())
				 .destination (key3.pub)
				 .balance (nano::genesis_amount - 6000 * nano::Gxrb_ratio)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send2->hash ()))
				 .build_shared ();
	auto send4 = send_block_builder.make_block ()
				 .previous (send3->hash ())
				 .destination (key4.pub)
				 .balance (nano::genesis_amount - 8000 * nano::Gxrb_ratio)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send3->hash ()))
				 .build_shared ();
	auto send5 = send_block_builder.make_block ()
				 .previous (send4->hash ())
				 .destination (key5.pub)
				 .balance (nano::genesis_amount - 10000 * nano::Gxrb_ratio)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send4->hash ()))
				 .build_shared ();
	auto open1 = state_block_builder.make_block ()
				 .account (key1.pub)
				 .previous (0)
				 .representative (key1.pub)
				 .balance (2000 * nano::Gxrb_ratio)
				 .link (send1->hash ())
				 .sign (key1.prv, key1.pub)
				 .work (*system.work.generate (key1.pub))
				 .build_shared ();
	auto open2 = state_block_builder.make_block ()
				 .account (key2.pub)
				 .previous (0)
				 .representative (key2.pub)
				 .balance (2000 * nano::Gxrb_ratio)
				 .link (send2->hash ())
				 .sign (key2.prv, key2.pub)
				 .work (*system.work.generate (key2.pub))
				 .build_shared ();
	auto open3 = state_block_builder.make_block ()
				 .account (key3.pub)
				 .previous (0)
				 .representative (key3.pub)
				 .balance (2000 * nano::Gxrb_ratio)
				 .link (send3->hash ())
				 .sign (key3.prv, key3.pub)
				 .work (*system.work.generate (key3.pub))
				 .build_shared ();
	auto open4 = state_block_builder.make_block ()
				 .account (key4.pub)
				 .previous (0)
				 .representative (key4.pub)
				 .balance (2000 * nano::Gxrb_ratio)
				 .link (send4->hash ())
				 .sign (key4.prv, key4.pub)
				 .work (*system.work.generate (key4.pub))
				 .build_shared ();
	auto open5 = state_block_builder.make_block ()
				 .account (key5.pub)
				 .previous (0)
				 .representative (key5.pub)
				 .balance (2000 * nano::Gxrb_ratio)
				 .link (send5->hash ())
				 .sign (key5.prv, key5.pub)
				 .work (*system.work.generate (key5.pub))
				 .build_shared ();
	node.block_processor.add (send1);
	node.block_processor.add (send2);
	node.block_processor.add (send3);
	node.block_processor.add (send4);
	node.block_processor.add (send5);
	node.block_processor.add (open1);
	node.block_processor.add (open2);
	node.block_processor.add (open3);
	node.block_processor.add (open4);
	node.block_processor.add (open5);
	node.block_processor.flush ();
	ASSERT_TIMELY (5s, 11 == node.ledger.cache.block_count);
	ASSERT_TRUE (node.active.empty ());
	ASSERT_EQ (1, node.ledger.cache.cemented_count);
	// These blocks will be processed later
	auto send6 = send_block_builder.make_block ()
				 .previous (send5->hash ())
				 .destination (nano::keypair ().pub)
				 .balance (send5->balance ().number () - 1)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send5->hash ()))
				 .build_shared ();
	auto send7 = send_block_builder.make_block ()
				 .previous (send6->hash ())
				 .destination (nano::keypair ().pub)
				 .balance (send6->balance ().number () - 1)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send6->hash ()))
				 .build_shared ();
	// Inactive votes
	std::vector<nano::block_hash> hashes{ open1->hash (), open2->hash (), open3->hash (), open4->hash (), open5->hash (), send7->hash () };
	auto vote1 (std::make_shared<nano::vote> (key1.pub, key1.prv, 0, hashes));
	node.vote_processor.vote (vote1, std::make_shared<nano::transport::channel_loopback> (node));
	auto vote2 (std::make_shared<nano::vote> (key2.pub, key2.prv, 0, hashes));
	node.vote_processor.vote (vote2, std::make_shared<nano::transport::channel_loopback> (node));
	auto vote3 (std::make_shared<nano::vote> (key3.pub, key3.prv, 0, hashes));
	node.vote_processor.vote (vote3, std::make_shared<nano::transport::channel_loopback> (node));
	auto vote4 (std::make_shared<nano::vote> (key4.pub, key4.prv, 0, hashes));
	node.vote_processor.vote (vote4, std::make_shared<nano::transport::channel_loopback> (node));
	ASSERT_TIMELY (5s, node.active.inactive_votes_cache_size () == 6);
	ASSERT_TRUE (node.active.empty ());
	ASSERT_EQ (1, node.ledger.cache.cemented_count);
	// 5 votes are required to start election
	auto vote5 (std::make_shared<nano::vote> (key5.pub, key5.prv, 0, hashes));
	node.vote_processor.vote (vote5, std::make_shared<nano::transport::channel_loopback> (node));
	ASSERT_TIMELY (5s, 5 == node.active.size ());
	// Confirm elections with weight quorum
	auto vote0 (std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, std::numeric_limits<uint64_t>::max (), hashes));
	node.vote_processor.vote (vote0, std::make_shared<nano::transport::channel_loopback> (node));
	ASSERT_TIMELY (5s, node.active.empty ());
	ASSERT_TIMELY (5s, 11 == node.ledger.cache.cemented_count);
	// A late block arrival also checks the inactive votes cache
	ASSERT_TRUE (node.active.empty ());
	auto send7_cache (node.active.find_inactive_votes_cache (send7->hash ()));
	ASSERT_EQ (6, send7_cache.voters.size ());
	ASSERT_TRUE (send7_cache.status.bootstrap_started);
	ASSERT_TRUE (send7_cache.status.confirmed);
	ASSERT_TRUE (send7_cache.status.election_started); // already marked even though the block does not exist
	node.process_active (send6);
	node.block_processor.flush ();
	// An election is started for send6 but does not confirm
	ASSERT_TIMELY (5s, 1 == node.active.size ());
	node.vote_processor.flush ();
	ASSERT_FALSE (node.block_confirmed_or_being_confirmed (node.store.tx_begin_read (), send6->hash ()));
	// send7 cannot be voted on but an election should be started from inactive votes
	ASSERT_FALSE (node.ledger.dependents_confirmed (node.store.tx_begin_read (), *send7));
	node.process_active (send7);
	node.block_processor.flush ();
	ASSERT_TIMELY (5s, 13 == node.ledger.cache.cemented_count);
}

namespace nano
{
TEST (active_transactions, vote_replays)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.enable_voting = false;
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	nano::genesis genesis;
	nano::keypair key;
	nano::state_block_builder builder;
	auto send1 = builder.make_block ()
				 .account (nano::dev_genesis_key.pub)
				 .previous (genesis.hash ())
				 .representative (nano::dev_genesis_key.pub)
				 .balance (nano::genesis_amount - nano::Gxrb_ratio)
				 .link (key.pub)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (genesis.hash ()))
				 .build_shared ();
	ASSERT_NE (nullptr, send1);
	auto open1 = builder.make_block ()
				 .account (key.pub)
				 .previous (0)
				 .representative (key.pub)
				 .balance (nano::Gxrb_ratio)
				 .link (send1->hash ())
				 .sign (key.prv, key.pub)
				 .work (*system.work.generate (key.pub))
				 .build_shared ();
	ASSERT_NE (nullptr, open1);
	node.process_active (send1);
	node.process_active (open1);
	nano::blocks_confirm (node, { send1, open1 });
	ASSERT_EQ (2, node.active.size ());
	// First vote is not a replay and confirms the election, second vote should be a replay since the election has confirmed but not yet removed
	auto vote_send1 (std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, std::numeric_limits<uint64_t>::max (), send1));
	ASSERT_EQ (nano::vote_code::vote, node.active.vote (vote_send1));
	ASSERT_EQ (2, node.active.size ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote_send1));
	// Wait until the election is removed, at which point the vote is still a replay since it's been recently confirmed
	ASSERT_TIMELY (3s, node.active.size () == 1);
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote_send1));
	// Open new account
	auto vote_open1 (std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, std::numeric_limits<uint64_t>::max (), open1));
	ASSERT_EQ (nano::vote_code::vote, node.active.vote (vote_open1));
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote_open1));
	ASSERT_TIMELY (3s, node.active.empty ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote_open1));
	ASSERT_EQ (nano::Gxrb_ratio, node.ledger.weight (key.pub));

	auto send2 = builder.make_block ()
				 .account (key.pub)
				 .previous (open1->hash ())
				 .representative (key.pub)
				 .balance (nano::Gxrb_ratio - 1)
				 .link (key.pub)
				 .sign (key.prv, key.pub)
				 .work (*system.work.generate (open1->hash ()))
				 .build_shared ();
	ASSERT_NE (nullptr, send2);
	node.process_active (send2);
	nano::blocks_confirm (node, { send2 });
	ASSERT_EQ (1, node.active.size ());
	auto vote1_send2 (std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, std::numeric_limits<uint64_t>::max (), send2));
	auto vote2_send2 (std::make_shared<nano::vote> (key.pub, key.prv, 0, send2));
	ASSERT_EQ (nano::vote_code::vote, node.active.vote (vote2_send2));
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote2_send2));
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (nano::vote_code::vote, node.active.vote (vote1_send2));
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote1_send2));
	ASSERT_TIMELY (3s, node.active.empty ());
	ASSERT_EQ (0, node.active.size ());
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote1_send2));
	ASSERT_EQ (nano::vote_code::replay, node.active.vote (vote2_send2));

	// Removing blocks as recently confirmed makes every vote indeterminate
	{
		nano::lock_guard<nano::mutex> guard (node.active.mutex);
		node.active.recently_confirmed.clear ();
	}
	ASSERT_EQ (nano::vote_code::indeterminate, node.active.vote (vote_send1));
	ASSERT_EQ (nano::vote_code::indeterminate, node.active.vote (vote_open1));
	ASSERT_EQ (nano::vote_code::indeterminate, node.active.vote (vote1_send2));
	ASSERT_EQ (nano::vote_code::indeterminate, node.active.vote (vote2_send2));
}
}

// Tests that blocks are correctly cleared from the duplicate filter for unconfirmed elections
TEST (active_transactions, dropped_cleanup)
{
	nano::system system;
	nano::node_flags flags;
	flags.disable_request_loop = true;
	auto & node (*system.add_node (flags));

	nano::genesis genesis;
	auto block = genesis.open;
	block->sideband_set (nano::block_sideband (nano::genesis_account, 0, nano::genesis_amount, 1, nano::seconds_since_epoch (), nano::epoch::epoch_0, false, false, false, nano::epoch::epoch_0));

	// Add to network filter to ensure proper cleanup after the election is dropped
	std::vector<uint8_t> block_bytes;
	{
		nano::vectorstream stream (block_bytes);
		block->serialize (stream);
	}
	ASSERT_FALSE (node.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));
	ASSERT_TRUE (node.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));

	node.block_confirm (block);
	node.scheduler.flush ();
	auto election = node.active.election (block->qualified_root ());
	ASSERT_NE (nullptr, election);

	// Not yet removed
	ASSERT_TRUE (node.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));
	ASSERT_EQ (1, node.active.blocks.count (block->hash ()));

	// Now simulate dropping the election
	ASSERT_FALSE (election->confirmed ());
	node.active.erase (*block);

	// The filter must have been cleared
	ASSERT_FALSE (node.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));

	// An election was recently dropped
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::election_drop));

	// Block cleared from active
	ASSERT_EQ (0, node.active.blocks.count (block->hash ()));

	// Repeat test for a confirmed election
	ASSERT_TRUE (node.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));
	node.block_confirm (block);
	node.scheduler.flush ();
	election = node.active.election (block->qualified_root ());
	ASSERT_NE (nullptr, election);
	election->force_confirm ();
	ASSERT_TRUE (election->confirmed ());
	node.active.erase (*block);

	// The filter should not have been cleared
	ASSERT_TRUE (node.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));

	// Not dropped
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::election_drop));

	// Block cleared from active
	ASSERT_EQ (0, node.active.blocks.count (block->hash ()));
}

TEST (active_transactions, republish_winner)
{
	nano::system system;
	nano::node_config node_config{ nano::get_available_port (), system.logging };
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node1 = *system.add_node (node_config);
	node_config.peering_port = nano::get_available_port ();
	auto & node2 = *system.add_node (node_config);

	nano::genesis genesis;
	nano::keypair key;
	nano::state_block_builder builder;
	auto send1 = builder.make_block ()
				 .account (nano::dev_genesis_key.pub)
				 .previous (genesis.hash ())
				 .representative (nano::dev_genesis_key.pub)
				 .balance (nano::genesis_amount - nano::Gxrb_ratio)
				 .link (key.pub)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (genesis.hash ()))
				 .build_shared ();

	node1.process_active (send1);
	node1.block_processor.flush ();
	ASSERT_TIMELY (3s, node2.stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in) == 1);

	// Several forks
	for (auto i (0); i < 5; i++)
	{
		auto fork = builder.make_block ()
					.account (nano::dev_genesis_key.pub)
					.previous (genesis.hash ())
					.representative (nano::dev_genesis_key.pub)
					.balance (nano::genesis_amount - 1 - i)
					.link (key.pub)
					.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
					.work (*system.work.generate (genesis.hash ()))
					.build_shared ();
		node1.process_active (fork);
	}
	node1.block_processor.flush ();
	ASSERT_TIMELY (3s, !node1.active.empty ());
	ASSERT_EQ (1, node2.stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in));

	// Process new fork with vote to change winner
	auto fork = builder.make_block ()
				.account (nano::dev_genesis_key.pub)
				.previous (genesis.hash ())
				.representative (nano::dev_genesis_key.pub)
				.balance (nano::genesis_amount - 2 * nano::Gxrb_ratio)
				.link (key.pub)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (genesis.hash ()))
				.build_shared ();

	node1.process_active (fork);
	node1.block_processor.flush ();
	auto election = node1.active.election (fork->qualified_root ());
	ASSERT_NE (nullptr, election);
	auto vote = std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, 0, std::vector<nano::block_hash>{ fork->hash () });
	node1.vote_processor.vote (vote, std::make_shared<nano::transport::channel_loopback> (node1));
	node1.vote_processor.flush ();
	node1.block_processor.flush ();
	ASSERT_TIMELY (3s, election->confirmed ());
	ASSERT_EQ (fork->hash (), election->status.winner->hash ());
	ASSERT_TIMELY (3s, node2.block_confirmed (fork->hash ()));
}

TEST (active_transactions, fork_filter_cleanup)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node1 (*system.add_node (node_config));

	nano::genesis genesis;
	nano::keypair key;
	nano::state_block_builder builder;
	auto send1 = builder.make_block ()
				 .account (nano::dev_genesis_key.pub)
				 .previous (genesis.hash ())
				 .representative (nano::dev_genesis_key.pub)
				 .balance (nano::genesis_amount - nano::Gxrb_ratio)
				 .link (key.pub)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (genesis.hash ()))
				 .build_shared ();
	std::vector<uint8_t> block_bytes;
	{
		nano::vectorstream stream (block_bytes);
		send1->serialize (stream);
	}

	// Generate 10 forks to prevent new block insertion to election
	for (auto i (0); i < 10; i++)
	{
		auto fork = builder.make_block ()
					.account (nano::dev_genesis_key.pub)
					.previous (genesis.hash ())
					.representative (nano::dev_genesis_key.pub)
					.balance (nano::genesis_amount - 1 - i)
					.link (key.pub)
					.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
					.work (*system.work.generate (genesis.hash ()))
					.build_shared ();
		node1.process_active (fork);
		node1.block_processor.flush ();
		node1.scheduler.flush ();
	}
	ASSERT_EQ (1, node1.active.size ());

	// Process correct block
	node_config.peering_port = nano::get_available_port ();
	auto & node2 (*system.add_node (node_config));
	node2.network.flood_block (send1);
	ASSERT_TIMELY (3s, node1.stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in) > 0);
	node1.block_processor.flush ();
	std::this_thread::sleep_for (50ms);

	// Block is erased from the duplicate filter
	ASSERT_FALSE (node1.network.publish_filter.apply (block_bytes.data (), block_bytes.size ()));

	auto election (node1.active.election (send1->qualified_root ()));
	ASSERT_NE (nullptr, election);
	ASSERT_EQ (10, election->blocks ().size ());
}

TEST (active_transactions, fork_replacement_tally)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node1 (*system.add_node (node_config));

	nano::genesis genesis;
	size_t reps_count = 20;
	size_t const max_blocks = 10;
	std::vector<nano::keypair> keys (reps_count);
	auto latest (genesis.hash ());
	auto balance (nano::genesis_amount);
	auto amount (node1.minimum_principal_weight ());
	nano::state_block_builder builder;

	// Create 20 representatives & confirm blocks
	for (auto i (0); i < reps_count; i++)
	{
		balance -= amount + i;
		auto send = builder.make_block ()
					.account (nano::dev_genesis_key.pub)
					.previous (latest)
					.representative (nano::dev_genesis_key.pub)
					.balance (balance)
					.link (keys[i].pub)
					.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
					.work (*system.work.generate (latest))
					.build_shared ();
		node1.process_active (send);
		latest = send->hash ();
		auto open = builder.make_block ()
					.account (keys[i].pub)
					.previous (0)
					.representative (keys[i].pub)
					.balance (amount + i)
					.link (send->hash ())
					.sign (keys[i].prv, keys[i].pub)
					.work (*system.work.generate (keys[i].pub))
					.build_shared ();
		node1.process_active (open);
		// Confirmation
		auto vote (std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, std::numeric_limits<uint64_t>::max (), std::vector<nano::block_hash>{ send->hash (), open->hash () }));
		node1.vote_processor.vote (vote, std::make_shared<nano::transport::channel_loopback> (node1));
	}
	node1.block_processor.flush ();
	ASSERT_TIMELY (5s, node1.ledger.cache.cemented_count == 1 + 2 * reps_count);

	nano::keypair key;
	auto send_last = builder.make_block ()
					 .account (nano::dev_genesis_key.pub)
					 .previous (latest)
					 .representative (nano::dev_genesis_key.pub)
					 .balance (balance - 2 * nano::Gxrb_ratio)
					 .link (key.pub)
					 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
					 .work (*system.work.generate (latest))
					 .build_shared ();

	// Forks without votes
	for (auto i (0); i < reps_count; i++)
	{
		auto fork = builder.make_block ()
					.account (nano::dev_genesis_key.pub)
					.previous (latest)
					.representative (nano::dev_genesis_key.pub)
					.balance (balance - nano::Gxrb_ratio - i)
					.link (key.pub)
					.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
					.work (*system.work.generate (latest))
					.build_shared ();
		node1.process_active (fork);
	}
	node1.block_processor.flush ();
	ASSERT_TIMELY (3s, !node1.active.empty ());
	// Check overflow of blocks
	auto election (node1.active.election (send_last->qualified_root ()));
	ASSERT_NE (nullptr, election);
	ASSERT_EQ (max_blocks, election->blocks ().size ());

	// Generate forks with votes to prevent new block insertion to election
	for (auto i (0); i < reps_count; i++)
	{
		auto fork = builder.make_block ()
					.account (nano::dev_genesis_key.pub)
					.previous (latest)
					.representative (nano::dev_genesis_key.pub)
					.balance (balance - 1 - i)
					.link (key.pub)
					.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
					.work (*system.work.generate (latest))
					.build_shared ();
		auto vote (std::make_shared<nano::vote> (keys[i].pub, keys[i].prv, 0, std::vector<nano::block_hash>{ fork->hash () }));
		node1.vote_processor.vote (vote, std::make_shared<nano::transport::channel_loopback> (node1));
		node1.vote_processor.flush ();
		node1.process_active (fork);
	}
	node1.block_processor.flush ();
	// Check overflow of blocks
	ASSERT_EQ (max_blocks, election->blocks ().size ());
	// Check that only max weight blocks remains (and start winner)
	auto votes1 (election->votes ());
	ASSERT_EQ (max_blocks, votes1.size ());
	for (auto i (max_blocks + 1); i < reps_count; i++)
	{
		ASSERT_TRUE (votes1.find (keys[i].pub) != votes1.end ());
	}

	// Process correct block
	node_config.peering_port = nano::get_available_port ();
	auto & node2 (*system.add_node (node_config));
	node2.network.flood_block (send_last);
	ASSERT_TIMELY (3s, node1.stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in) > 0);
	node1.block_processor.flush ();
	std::this_thread::sleep_for (50ms);

	// Correct block without votes is ignored
	auto blocks1 (election->blocks ());
	ASSERT_EQ (max_blocks, blocks1.size ());
	ASSERT_FALSE (blocks1.find (send_last->hash ()) != blocks1.end ());

	// Process vote for correct block & replace existing lowest tally block
	auto vote (std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, 0, std::vector<nano::block_hash>{ send_last->hash () }));
	node1.vote_processor.vote (vote, std::make_shared<nano::transport::channel_loopback> (node1));
	node1.vote_processor.flush ();
	node2.network.flood_block (send_last);
	ASSERT_TIMELY (3s, node1.stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in) > 1);
	node1.block_processor.flush ();
	std::this_thread::sleep_for (50ms);

	auto blocks2 (election->blocks ());
	ASSERT_EQ (max_blocks, blocks2.size ());
	ASSERT_TRUE (blocks2.find (send_last->hash ()) != blocks2.end ());
	auto votes2 (election->votes ());
	ASSERT_EQ (max_blocks, votes2.size ());
	for (auto i (max_blocks + 2); i < reps_count; i++)
	{
		ASSERT_TRUE (votes2.find (keys[i].pub) != votes2.end ());
	}
	ASSERT_FALSE (votes2.find (keys[max_blocks].pub) != votes2.end ());
	ASSERT_FALSE (votes2.find (keys[max_blocks + 1].pub) != votes2.end ());
	ASSERT_TRUE (votes2.find (nano::dev_genesis_key.pub) != votes2.end ());
}

namespace nano
{
// Blocks that won an election must always be seen as confirming or cemented
TEST (active_transactions, confirmation_consistency)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	system.wallet (0)->insert_adhoc (nano::dev_genesis_key.prv);
	for (unsigned i = 0; i < 10; ++i)
	{
		auto block (system.wallet (0)->send_action (nano::dev_genesis_key.pub, nano::public_key (), node.config.receive_minimum.number ()));
		ASSERT_NE (nullptr, block);
		system.deadline_set (5s);
		while (!node.ledger.block_confirmed (node.store.tx_begin_read (), block->hash ()))
		{
			node.scheduler.activate (nano::dev_genesis_key.pub, node.store.tx_begin_read ());
			ASSERT_NO_ERROR (system.poll (5ms));
		}
		ASSERT_NO_ERROR (system.poll_until_true (1s, [&node, &block, i] {
			nano::lock_guard<nano::mutex> guard (node.active.mutex);
			EXPECT_EQ (i + 1, node.active.recently_confirmed.size ());
			EXPECT_EQ (block->qualified_root (), node.active.recently_confirmed.back ().first);
			return i + 1 == node.active.recently_cemented.size (); // done after a callback
		}));
	}
}
}

TEST (active_transactions, confirm_new)
{
	nano::system system (1);
	auto & node1 = *system.nodes[0];
	nano::genesis genesis;
	auto send = nano::send_block_builder ()
				.previous (genesis.hash ())
				.destination (nano::public_key ())
				.balance (nano::genesis_amount - 100)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (genesis.hash ()))
				.build_shared ();
	node1.process_active (send);
	node1.block_processor.flush ();
	node1.scheduler.flush ();
	ASSERT_EQ (1, node1.active.size ());
	auto & node2 = *system.add_node ();
	// Add key to node2
	system.wallet (1)->insert_adhoc (nano::dev_genesis_key.prv);
	// Let node2 know about the block
	ASSERT_TIMELY (5s, node2.block (send->hash ()));
	// Wait confirmation
	ASSERT_TIMELY (5s, node1.ledger.cache.cemented_count == 2 && node2.ledger.cache.cemented_count == 2);
}

TEST (active_transactions, restart_dropped)
{
	nano::system system;
	nano::node_config node_config (nano::get_available_port (), system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (node_config);
	nano::genesis genesis;
	auto send = nano::state_block_builder ()
				.account (nano::dev_genesis_key.pub)
				.previous (genesis.hash ())
				.representative (nano::dev_genesis_key.pub)
				.balance (nano::genesis_amount - nano::xrb_ratio)
				.link (nano::dev_genesis_key.pub)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (genesis.hash ()))
				.build_shared (); // Process only in ledger and simulate dropping the election
	ASSERT_EQ (nano::process_result::progress, node.process (*send).code);
	// Generate higher difficulty work
	ASSERT_TRUE (node.work_generate_blocking (*send, send->difficulty () + 1).is_initialized ());
	// Process the same block with updated work
	ASSERT_EQ (0, node.active.size ());
	node.process_active (send);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::election_restart));
	auto ledger_block (node.store.block_get (node.store.tx_begin_read (), send->hash ()));
	ASSERT_NE (nullptr, ledger_block);
	// Exact same block, including work value must have been re-written
	ASSERT_EQ (*send, *ledger_block);
	// Drop election
	node.active.erase (*send);
	ASSERT_EQ (0, node.active.size ());
	// Try to restart election with the same difficulty
	node.process_active (send);
	node.block_processor.flush ();
	ASSERT_EQ (0, node.active.size ());
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::election_restart));
	// Generate even higher difficulty work
	ASSERT_TRUE (node.work_generate_blocking (*send, send->difficulty () + 1).is_initialized ());
	// Add voting
	system.wallet (0)->insert_adhoc (nano::dev_genesis_key.prv);
	// Process the same block with updated work
	ASSERT_EQ (0, node.active.size ());
	node.process_active (send);
	node.block_processor.flush ();
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (1, node.ledger.cache.cemented_count);
	ASSERT_EQ (2, node.stats.count (nano::stat::type::election, nano::stat::detail::election_restart));
	// Wait for the election to complete
	ASSERT_TIMELY (5s, node.ledger.cache.cemented_count == 2);
	// Verify the block is eventually updated in the ledger
	ASSERT_TIMELY (3s, node.store.block_get (node.store.tx_begin_read (), send->hash ())->block_work () == send->block_work ());
}

// Ensures votes are tallied on election::publish even if no vote is inserted through inactive_votes_cache
TEST (active_transactions, conflicting_block_vote_existing_election)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_request_loop = true;
	auto & node = *system.add_node (node_flags);
	nano::genesis genesis;
	nano::keypair key;
	nano::state_block_builder builder;
	auto send = builder.make_block ()
				.account (nano::dev_genesis_key.pub)
				.previous (genesis.hash ())
				.representative (nano::dev_genesis_key.pub)
				.balance (nano::genesis_amount - 100)
				.link (key.pub)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (genesis.hash ()))
				.build_shared ();
	auto fork = builder.make_block ()
				.account (nano::dev_genesis_key.pub)
				.previous (genesis.hash ())
				.representative (nano::dev_genesis_key.pub)
				.balance (nano::genesis_amount - 200)
				.link (key.pub)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (genesis.hash ()))
				.build_shared ();
	auto vote_fork (std::make_shared<nano::vote> (nano::dev_genesis_key.pub, nano::dev_genesis_key.prv, std::numeric_limits<uint64_t>::max (), fork));

	ASSERT_EQ (nano::process_result::progress, node.process_local (send).code);
	node.scheduler.flush ();
	ASSERT_EQ (1, node.active.size ());

	// Vote for conflicting block, but the block does not yet exist in the ledger
	node.active.vote (vote_fork);

	// Block now gets processed
	ASSERT_EQ (nano::process_result::fork, node.process_local (fork).code);

	// Election must be confirmed
	auto election (node.active.election (fork->qualified_root ()));
	ASSERT_NE (nullptr, election);
	ASSERT_TIMELY (3s, election->confirmed ());
}

TEST (active_transactions, activate_account_chain)
{
	nano::system system;
	nano::node_flags flags;
	nano::node_config config (nano::get_available_port (), system.logging);
	config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (config, flags);

	nano::keypair key;
	nano::state_block_builder builder;
	auto send = builder.make_block ()
				.account (nano::dev_genesis_key.pub)
				.previous (nano::genesis_hash)
				.representative (nano::dev_genesis_key.pub)
				.link (nano::dev_genesis_key.pub)
				.balance (nano::genesis_amount - 1)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (nano::genesis_hash))
				.build ();
	auto send2 = builder.make_block ()
				 .account (nano::dev_genesis_key.pub)
				 .previous (send->hash ())
				 .representative (nano::dev_genesis_key.pub)
				 .link (key.pub)
				 .balance (nano::genesis_amount - 2)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send->hash ()))
				 .build ();
	auto send3 = builder.make_block ()
				 .account (nano::dev_genesis_key.pub)
				 .previous (send2->hash ())
				 .representative (nano::dev_genesis_key.pub)
				 .link (key.pub)
				 .balance (nano::genesis_amount - 3)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send2->hash ()))
				 .build ();
	auto open = builder.make_block ()
				.account (key.pub)
				.previous (0)
				.representative (key.pub)
				.link (send2->hash ())
				.balance (1)
				.sign (key.prv, key.pub)
				.work (*system.work.generate (key.pub))
				.build ();
	auto receive = builder.make_block ()
				   .account (key.pub)
				   .previous (open->hash ())
				   .representative (key.pub)
				   .link (send3->hash ())
				   .balance (2)
				   .sign (key.prv, key.pub)
				   .work (*system.work.generate (open->hash ()))
				   .build ();
	ASSERT_EQ (nano::process_result::progress, node.process (*send).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*send2).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*send3).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*open).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*receive).code);

	node.scheduler.activate (nano::dev_genesis_key.pub, node.store.tx_begin_read ());
	node.scheduler.flush ();
	auto election1 = node.active.election (send->qualified_root ());
	ASSERT_EQ (1, node.active.size ());
	ASSERT_EQ (1, election1->blocks ().count (send->hash ()));
	node.scheduler.activate (nano::dev_genesis_key.pub, node.store.tx_begin_read ());
	auto election2 = node.active.election (send->qualified_root ());
	ASSERT_EQ (election2, election1);
	election1->force_confirm ();
	ASSERT_TIMELY (3s, node.block_confirmed (send->hash ()));
	// On cementing, the next election is started
	ASSERT_TIMELY (3s, node.active.active (send2->qualified_root ()));
	node.scheduler.activate (nano::dev_genesis_key.pub, node.store.tx_begin_read ());
	auto election3 = node.active.election (send2->qualified_root ());
	ASSERT_NE (nullptr, election3);
	ASSERT_EQ (1, election3->blocks ().count (send2->hash ()));
	election3->force_confirm ();
	ASSERT_TIMELY (3s, node.block_confirmed (send2->hash ()));
	// On cementing, the next election is started
	ASSERT_TIMELY (3s, node.active.active (open->qualified_root ()));
	ASSERT_TIMELY (3s, node.active.active (send3->qualified_root ()));
	node.scheduler.activate (nano::dev_genesis_key.pub, node.store.tx_begin_read ());
	auto election4 = node.active.election (send3->qualified_root ());
	ASSERT_NE (nullptr, election4);
	ASSERT_EQ (1, election4->blocks ().count (send3->hash ()));
	node.scheduler.activate (key.pub, node.store.tx_begin_read ());
	auto election5 = node.active.election (open->qualified_root ());
	ASSERT_NE (nullptr, election5);
	ASSERT_EQ (1, election5->blocks ().count (open->hash ()));
	election5->force_confirm ();
	ASSERT_TIMELY (3s, node.block_confirmed (open->hash ()));
	// Until send3 is also confirmed, the receive block should not activate
	std::this_thread::sleep_for (200ms);
	node.scheduler.activate (key.pub, node.store.tx_begin_read ());
	election4->force_confirm ();
	ASSERT_TIMELY (3s, node.block_confirmed (send3->hash ()));
	ASSERT_TIMELY (3s, node.active.active (receive->qualified_root ()));
}

TEST (active_transactions, activate_inactive)
{
	nano::system system;
	nano::node_flags flags;
	nano::node_config config (nano::get_available_port (), system.logging);
	config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (config, flags);

	nano::keypair key;
	nano::state_block_builder builder;
	auto send = builder.make_block ()
				.account (nano::dev_genesis_key.pub)
				.previous (nano::genesis_hash)
				.representative (nano::dev_genesis_key.pub)
				.link (key.pub)
				.balance (nano::genesis_amount - 1)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (nano::genesis_hash))
				.build_shared ();
	auto send2 = builder.make_block ()
				 .account (nano::dev_genesis_key.pub)
				 .previous (send->hash ())
				 .representative (nano::dev_genesis_key.pub)
				 .link (nano::keypair ().pub)
				 .balance (nano::genesis_amount - 2)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send->hash ()))
				 .build_shared ();
	auto open = builder.make_block ()
				.account (key.pub)
				.previous (0)
				.representative (key.pub)
				.link (send->hash ())
				.balance (1)
				.sign (key.prv, key.pub)
				.work (*system.work.generate (key.pub))
				.build_shared ();

	ASSERT_EQ (nano::process_result::progress, node.process (*send).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*send2).code);
	ASSERT_EQ (nano::process_result::progress, node.process (*open).code);

	node.block_confirm (send2);
	auto election = node.active.election (send2->qualified_root ());
	ASSERT_NE (nullptr, election);
	election->force_confirm ();

	ASSERT_TIMELY (3s, !node.confirmation_height_processor.is_processing_added_block (send2->hash ()));
	ASSERT_TRUE (node.block_confirmed (send2->hash ()));
	ASSERT_TRUE (node.block_confirmed (send->hash ()));

	ASSERT_EQ (1, node.stats.count (nano::stat::type::confirmation_observer, nano::stat::detail::inactive_conf_height, nano::stat::dir::out));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::confirmation_observer, nano::stat::detail::active_quorum, nano::stat::dir::out));
	ASSERT_EQ (0, node.stats.count (nano::stat::type::confirmation_observer, nano::stat::detail::active_conf_height, nano::stat::dir::out));

	// The first block was not active so no activation takes place
	ASSERT_FALSE (node.active.active (open->qualified_root ()) || node.block_confirmed_or_being_confirmed (node.store.tx_begin_read (), open->hash ()));
}

TEST (active_transactions, difficulty_update_observer)
{
	nano::system system (1);
	auto & node (*system.nodes[0]);
	std::atomic<bool> update_received (false);
	node.observers.difficulty.add ([&mutex = node.active.mutex, &update_received] (uint64_t difficulty_a) {
		nano::unique_lock<nano::mutex> lock (mutex, std::defer_lock);
		EXPECT_TRUE (lock.try_lock ());
		update_received = true;
	});
	ASSERT_TIMELY (3s, update_received);
}

namespace nano
{
TEST (active_transactions, pessimistic_elections)
{
	nano::system system;
	nano::node_flags flags;
	nano::node_config config (nano::get_available_port (), system.logging);
	config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto & node = *system.add_node (config, flags);

	nano::keypair key;
	nano::state_block_builder builder;
	auto send = builder.make_block ()
				.account (nano::dev_genesis_key.pub)
				.previous (nano::genesis_hash)
				.representative (nano::dev_genesis_key.pub)
				.link (nano::dev_genesis_key.pub)
				.balance (nano::genesis_amount - 1)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (nano::genesis_hash))
				.build_shared ();

	ASSERT_EQ (nano::process_result::progress, node.process (*send).code);

	auto send2 = builder.make_block ()
				 .account (nano::dev_genesis_key.pub)
				 .previous (send->hash ())
				 .representative (nano::dev_genesis_key.pub)
				 .link (key.pub)
				 .balance (nano::genesis_amount - 2)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send->hash ()))
				 .build ();

	ASSERT_EQ (nano::process_result::progress, node.process (*send2).code);

	auto open = builder.make_block ()
				.account (key.pub)
				.previous (0)
				.representative (key.pub)
				.link (send2->hash ())
				.balance (1)
				.sign (key.prv, key.pub)
				.work (*system.work.generate (key.pub))
				.build_shared ();

	ASSERT_EQ (nano::process_result::progress, node.process (*open).code);

	// This should only cement the first block in genesis account
	uint64_t election_count = 0;
	// Make dummy election with winner.
	{
		nano::election election1 (
		node, send, [] (auto const &) {}, [] (auto const &) {}, nano::election_behavior::normal);
		nano::election election2 (
		node, open, [] (auto const &) {}, [] (auto const &) {}, nano::election_behavior::normal);
		node.active.add_expired_optimistic_election (election1);
		node.active.add_expired_optimistic_election (election2);
	}
	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);
	ASSERT_EQ (1, election_count);
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());
	auto election_started_it = node.active.expired_optimistic_election_infos.get<nano::active_transactions::tag_election_started> ().begin ();
	ASSERT_EQ (election_started_it->account, nano::genesis_account);
	ASSERT_EQ (election_started_it->election_started, true);
	ASSERT_EQ ((++election_started_it)->election_started, false);

	// No new elections should get started yet
	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);
	ASSERT_EQ (1, election_count);
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());
	ASSERT_EQ (node.active.expired_optimistic_election_infos_size, node.active.expired_optimistic_election_infos.size ());

	ASSERT_EQ (1, node.active.size ());
	auto election = node.active.election (send->qualified_root ());
	ASSERT_NE (nullptr, election);
	election->force_confirm ();

	ASSERT_TIMELY (3s, node.block_confirmed (send->hash ()) && !node.confirmation_height_processor.is_processing_added_block (send->hash ()));

	nano::confirmation_height_info genesis_confirmation_height_info;
	nano::confirmation_height_info key1_confirmation_height_info;
	{
		auto transaction = node.store.tx_begin_read ();
		node.store.confirmation_height_get (transaction, nano::genesis_account, genesis_confirmation_height_info);
		ASSERT_EQ (2, genesis_confirmation_height_info.height);
		node.store.confirmation_height_get (transaction, key.pub, key1_confirmation_height_info);
		ASSERT_EQ (0, key1_confirmation_height_info.height);
	}

	// Activation of cemented frontier successor should get started after the first pessimistic block is confirmed
	ASSERT_TIMELY (10s, node.active.active (send->qualified_root ()));

	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);
	ASSERT_EQ (1, election_count);
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());

	// Confirm it
	election = node.active.election (send2->qualified_root ());
	ASSERT_NE (nullptr, election);
	election->force_confirm ();

	ASSERT_TIMELY (3s, node.block_confirmed (send2->hash ()));

	{
		auto transaction = node.store.tx_begin_read ();
		node.store.confirmation_height_get (transaction, nano::genesis_account, genesis_confirmation_height_info);
		ASSERT_EQ (3, genesis_confirmation_height_info.height);
		node.store.confirmation_height_get (transaction, key.pub, key1_confirmation_height_info);
		ASSERT_EQ (0, key1_confirmation_height_info.height);
	}

	// Wait until activation of destination account is done.
	ASSERT_TIMELY (10s, node.active.active (send2->qualified_root ()));

	// Election count should not increase, but the elections should be marked as started for that account afterwards
	ASSERT_EQ (election_started_it->election_started, false);
	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);
	ASSERT_EQ (1, election_count);
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());
	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);

	election = node.active.election (open->qualified_root ());
	ASSERT_NE (nullptr, election);
	election->force_confirm ();

	ASSERT_TIMELY (3s, node.block_confirmed (open->hash ()));

	{
		auto transaction = node.store.tx_begin_read ();
		node.store.confirmation_height_get (transaction, nano::genesis_account, genesis_confirmation_height_info);
		ASSERT_EQ (3, genesis_confirmation_height_info.height);
		node.store.confirmation_height_get (transaction, key.pub, key1_confirmation_height_info);
		ASSERT_EQ (1, key1_confirmation_height_info.height);
	}

	// Sanity check that calling it again on a fully cemented chain has no adverse effects.
	node.active.confirm_expired_frontiers_pessimistically (node.store.tx_begin_read (), 100, election_count);
	ASSERT_EQ (1, election_count);
	ASSERT_EQ (2, node.active.expired_optimistic_election_infos.size ());
}
}

TEST (active_transactions, list_active)
{
	nano::system system (1);
	auto & node = *system.nodes[0];

	nano::keypair key;
	nano::state_block_builder builder;
	auto send = builder.make_block ()
				.account (nano::dev_genesis_key.pub)
				.previous (nano::genesis_hash)
				.representative (nano::dev_genesis_key.pub)
				.link (nano::dev_genesis_key.pub)
				.balance (nano::genesis_amount - 1)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (nano::genesis_hash))
				.build_shared ();

	ASSERT_EQ (nano::process_result::progress, node.process (*send).code);

	auto send2 = builder.make_block ()
				 .account (nano::dev_genesis_key.pub)
				 .previous (send->hash ())
				 .representative (nano::dev_genesis_key.pub)
				 .link (key.pub)
				 .balance (nano::genesis_amount - 2)
				 .sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				 .work (*system.work.generate (send->hash ()))
				 .build_shared ();

	ASSERT_EQ (nano::process_result::progress, node.process (*send2).code);

	auto open = builder.make_block ()
				.account (key.pub)
				.previous (0)
				.representative (key.pub)
				.link (send2->hash ())
				.balance (1)
				.sign (key.prv, key.pub)
				.work (*system.work.generate (key.pub))
				.build_shared ();

	ASSERT_EQ (nano::process_result::progress, node.process (*open).code);

	nano::blocks_confirm (node, { send, send2, open });
	ASSERT_EQ (3, node.active.size ());
	ASSERT_EQ (1, node.active.list_active (1).size ());
	ASSERT_EQ (2, node.active.list_active (2).size ());
	ASSERT_EQ (3, node.active.list_active (3).size ());
	ASSERT_EQ (3, node.active.list_active (4).size ());
	ASSERT_EQ (3, node.active.list_active (99999).size ());
	ASSERT_EQ (3, node.active.list_active ().size ());

	auto active = node.active.list_active ();

	auto difficulty_cmp = [] (std::shared_ptr<nano::election> const & election_l, std::shared_ptr<nano::election> const & election_r) {
		return election_l->winner ()->difficulty () >= election_r->winner ()->difficulty ();
	};
	ASSERT_TRUE (std::is_sorted (active.cbegin (), active.cend (), difficulty_cmp));
}

TEST (active_transactions, vacancy)
{
	nano::system system;
	nano::node_config config{ nano::get_available_port (), system.logging };
	config.active_elections_size = 1;
	auto & node = *system.add_node (config);
	nano::state_block_builder builder;
	auto send = builder.make_block ()
				.account (nano::dev_genesis_key.pub)
				.previous (nano::genesis_hash)
				.representative (nano::dev_genesis_key.pub)
				.link (nano::dev_genesis_key.pub)
				.balance (nano::genesis_amount - nano::Gxrb_ratio)
				.sign (nano::dev_genesis_key.prv, nano::dev_genesis_key.pub)
				.work (*system.work.generate (nano::genesis_hash))
				.build_shared ();
	std::atomic<bool> updated = false;
	node.active.vacancy_update = [&updated] () { updated = true; };
	ASSERT_EQ (nano::process_result::progress, node.process (*send).code);
	ASSERT_EQ (1, node.active.vacancy ());
	ASSERT_EQ (0, node.active.size ());
	node.scheduler.activate (nano::dev_genesis_key.pub, node.store.tx_begin_read ());
	ASSERT_TIMELY (1s, updated);
	updated = false;
	ASSERT_EQ (0, node.active.vacancy ());
	ASSERT_EQ (1, node.active.size ());
	auto election1 = node.active.election (send->qualified_root ());
	ASSERT_NE (nullptr, election1);
	election1->force_confirm ();
	ASSERT_TIMELY (1s, updated);
	ASSERT_EQ (1, node.active.vacancy ());
	ASSERT_EQ (0, node.active.size ());
}
