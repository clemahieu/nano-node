#include <nano/lib/blockbuilders.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/test_common/chains.hpp>
#include <nano/test_common/ledger.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <format>

using namespace std::chrono_literals;

namespace
{
std::deque<nano::keypair> rep_set (size_t count)
{
	std::deque<nano::keypair> result;
	for (auto i = 0; i < count; ++i)
	{
		result.emplace_back (nano::keypair{});
	}
	return result;
}
}

TEST (flamegraph, large_direct_processing)
{
	auto reps = rep_set (4);
	auto circulating = 10 * nano::Gxrb_ratio;
	nano::test::system system;
	system.ledger_initialization_set (reps, circulating);
	auto & node = *system.add_node ();
	auto prepare = [&] () {
		nano::state_block_builder builder;
		std::deque<std::shared_ptr<nano::block>> blocks;
		std::deque<nano::keypair> keys;
		auto previous = *std::prev (std::prev (system.initialization_blocks.end ()));
		for (auto i = 0; i < 20000; ++i)
		{
			keys.emplace_back ();
			auto const & key = keys.back ();
			auto block = builder.make_block ()
						 .account (nano::dev::genesis_key.pub)
						 .representative (nano::dev::genesis_key.pub)
						 .previous (previous->hash ())
						 .link (key.pub)
						 .balance (previous->balance_field ().value ().number () - nano::xrb_ratio)
						 .sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
						 .work (*system.work.generate (previous->hash ()))
						 .build ();
			blocks.push_back (block);
			previous = block;
		}
		return std::make_tuple (blocks, keys);
	};
	auto const & [blocks, keys] = prepare ();
	auto execute = [&] () {
		auto count = 0;
		for (auto block : blocks)
		{
			ASSERT_EQ (nano::block_status::progress, node.process (block));
		}
	};
	execute ();
}

TEST (flamegraph, large_confirmation)
{
	auto start = std::chrono::steady_clock::now ();
	auto circulating = 10 * nano::Gxrb_ratio;
	auto rep_count = 4;
	auto block_count = 500;
	auto rep_amount = (nano::dev::constants.genesis_amount - circulating) / rep_count;
	std::deque<nano::keypair> reps;
	std::filesystem::path path;
	{
		std::cerr << "Preparing ledger...\n";
		auto ctx = nano::test::context::ledger_empty ();
		path = ctx.path;
		while (reps.size () < rep_count)
		{
			reps.emplace_back (nano::test::setup_rep (ctx.pool (), ctx.ledger (), rep_amount, nano::dev::genesis_key));
		}

		auto tx = ctx.ledger ().tx_begin_write ();
		nano::state_block_builder builder;
		auto previous = ctx.ledger ().any.block_get (tx, ctx.ledger ().any.account_head (tx, nano::dev::genesis_key.pub));
		for (auto i = 0; i < block_count; ++i)
		{
			nano::keypair key;
			auto block = builder.make_block ()
						 .account (nano::dev::genesis_key.pub)
						 .representative (nano::dev::genesis_key.pub)
						 .previous (previous->hash ())
						 .link (key.pub)
						 .balance (previous->balance_field ().value ().number () - nano::xrb_ratio)
						 .sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
						 .work (*ctx.pool ().generate (previous->hash ()))
						 .build ();
			ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (tx, block));
			previous = block;
		}
	}

	nano::test::system system;
	{
		std::cerr << "Initializing nodes...\n";
		for (auto rep : reps)
		{
			auto data_path = nano::unique_path ();
			std::filesystem::create_directory (data_path);
			std::filesystem::copy (path, data_path, std::filesystem::copy_options::recursive);
			nano::node_config config;
			nano::node_flags flags;
			system.add_node (config, flags, nano::transport::transport_type::tcp, rep, data_path);
		}
	}
	auto prep = std::chrono::steady_clock::now ();
	std::cerr << "Waiting for confirmation...\n";
	ASSERT_TIMELY (3000s, std::all_of (system.nodes.begin (), system.nodes.end (), [&] (auto const & node) {
		std::cerr << std::format ("c({})a({})i({})f({}) ", node->ledger.cemented_count (), node->stats.count (nano::stat::type::hinting, nano::stat::detail::activate), node->stats.count (nano::stat::type::hinting, nano::stat::detail::insert), node->stats.count (nano::stat::type::hinting, nano::stat::detail::insert_failed));
		return node->ledger.cemented_count () == node->ledger.block_count ();
	}));
	auto finish = std::chrono::steady_clock::now ();
	auto total = std::chrono::duration_cast<std::chrono::milliseconds> (finish - start).count ();
	auto work = std::chrono::duration_cast<std::chrono::milliseconds> (finish - prep).count ();
	std::cerr << "\nTotal: " << total << " work: " << work << " percent: " << (static_cast<double> (work) / total) << std::endl;
}
