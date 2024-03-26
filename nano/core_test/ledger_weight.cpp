#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/test_common/ledger.hpp>

#include <gtest/gtest.h>

TEST (ledger_weight, genesis)
{
	auto ctx = nano::test::context::ledger_empty ();
	ASSERT_EQ (nano::dev::genesis->balance (), ctx.ledger ().weight (nano::dev::genesis_key.pub));
}

TEST (ledger_weight, send_same)
{
	auto ctx = nano::test::context::ledger_empty ();
	nano::state_block_builder builder;
	auto block = builder.make_block ()
		.account (nano::dev::genesis_key.pub)
		.previous (nano::dev::genesis->hash ())
		.representative (nano::dev::genesis_key.pub)
		.balance (nano::dev::genesis->balance ().number () - 1)
		.link (nano::dev::genesis_key.pub)
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (ctx.pool ().generate (nano::dev::genesis->hash ()).value ())
		.build ();
	ASSERT_EQ (nano::dev::genesis->balance (), ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), block));
	ctx.ledger ().confirm (ctx.store ().tx_begin_write (), block->hash ());
	ASSERT_EQ (nano::dev::genesis->balance ().number () - 1, ctx.ledger ().weight (nano::dev::genesis_key.pub));
}

TEST (ledger_weight, send_different)
{
	auto ctx = nano::test::context::ledger_empty ();
	nano::keypair key;
	nano::state_block_builder builder;
	auto block = builder.make_block ()
		.account (nano::dev::genesis_key.pub)
		.previous (nano::dev::genesis->hash ())
		.representative (key.pub)
		.balance (nano::dev::genesis->balance ().number () - 1)
		.link (nano::dev::genesis_key.pub)
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (ctx.pool ().generate (nano::dev::genesis->hash ()).value ())
		.build ();
	ASSERT_EQ (nano::dev::genesis->balance (), ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), block));
	ctx.ledger ().confirm (ctx.store ().tx_begin_write (), block->hash ());
	ASSERT_EQ (nano::dev::genesis->balance ().number () - 1, ctx.ledger ().weight (key.pub));
}

TEST (ledger_weight, open)
{
	auto ctx = nano::test::context::ledger_empty ();
	nano::keypair key;
	nano::state_block_builder builder;
	auto send = builder.make_block ()
		.account (nano::dev::genesis_key.pub)
		.previous (nano::dev::genesis->hash ())
		.representative (nano::dev::genesis_key.pub)
		.balance (nano::dev::genesis->balance ().number () - 1)
		.link (key.pub)
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (ctx.pool ().generate (nano::dev::genesis->hash ()).value ())
		.build ();
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), send));
	auto open = builder.make_block ()
		.account (key.pub)
		.previous (0)
		.representative(key.pub)
		.balance(1)
		.link (send->hash ())
		.sign (key.prv, key.pub)
		.work (ctx.pool ().generate (key.pub).value ())
		.build ();
	ASSERT_EQ (nano::dev::genesis->balance ().number () - 1, ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (0, ctx.ledger ().weight (key.pub));
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), open));
	ctx.ledger ().confirm (ctx.store ().tx_begin_write (), open->hash ());
	ASSERT_EQ (nano::dev::genesis->balance ().number () - 1, ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (1, ctx.ledger ().weight (key.pub));
}

TEST (ledger_weight, change)
{
	auto ctx = nano::test::context::ledger_empty ();
	nano::keypair key;
	nano::state_block_builder builder;
	auto block = builder.make_block ()
		.account (nano::dev::genesis_key.pub)
		.previous (nano::dev::genesis->hash ())
		.representative (key.pub)
		.balance (nano::dev::genesis->balance ().number ())
		.link (0)
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (ctx.pool ().generate (nano::dev::genesis->hash ()).value ())
		.build ();
	ASSERT_EQ (nano::dev::genesis->balance (), ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (0, ctx.ledger ().weight (key.pub));
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), block));
	ctx.ledger ().confirm (ctx.store ().tx_begin_write (), block->hash ());
	ASSERT_EQ (0, ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (nano::dev::genesis->balance ().number (), ctx.ledger ().weight (key.pub));
}

TEST (ledger_weight, change_legacy)
{
	auto ctx = nano::test::context::ledger_empty ();
	nano::keypair key;
	nano::change_block_builder builder;
	auto block = builder.make_block ()
		.previous (nano::dev::genesis->hash ())
		.representative (key.pub)
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (ctx.pool ().generate (nano::dev::genesis->hash ()).value ())
		.build ();
	ASSERT_EQ (nano::dev::genesis->balance (), ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (0, ctx.ledger ().weight (key.pub));
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), block));
	ctx.ledger ().confirm (ctx.store ().tx_begin_write (), block->hash ());
	ASSERT_EQ (0, ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (nano::dev::genesis->balance ().number (), ctx.ledger ().weight (key.pub));
}

TEST (ledger_weight, send_legacy)
{
	auto ctx = nano::test::context::ledger_empty ();
	nano::send_block_builder builder;
	auto block = builder.make_block ()
		.previous (nano::dev::genesis->hash ())
		.destination (nano::dev::genesis_key.pub)
		.balance (nano::dev::genesis->balance ().number () - 1)
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (ctx.pool ().generate (nano::dev::genesis->hash ()).value ())
		.build ();
	ASSERT_EQ (nano::dev::genesis->balance (), ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), block));
	ctx.ledger ().confirm (ctx.store ().tx_begin_write (), block->hash ());
	ASSERT_EQ (nano::dev::genesis->balance ().number () - 1, ctx.ledger ().weight (nano::dev::genesis_key.pub));
}

TEST (ledger_weight, receive_legacy)
{
	auto ctx = nano::test::context::ledger_empty ();
	nano::send_block_builder builder1;
	auto send1 = builder1.make_block ()
		.previous (nano::dev::genesis->hash ())
		.destination (nano::dev::genesis_key.pub)
		.balance (nano::dev::genesis->balance ().number () - 1)
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (ctx.pool ().generate (nano::dev::genesis->hash ()).value ())
		.build ();
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), send1));
	auto send2 = builder1.make_block ()
		.previous (send1->hash ())
		.destination (nano::dev::genesis_key.pub)
		.balance (nano::dev::genesis->balance ().number () - 2)
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (ctx.pool ().generate (send1->hash ()).value ())
		.build ();
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), send2));
	nano::receive_block_builder builder2;
	auto receive = builder2.make_block ()
		.previous (send2->hash ())
		.source (send1->hash ())
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (ctx.pool ().generate (send2->hash ()).value ())
		.build ();
	ASSERT_EQ (nano::dev::genesis->balance ().number () - 2, ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), receive));
	ctx.ledger ().confirm (ctx.store ().tx_begin_write (), receive->hash ());
	ASSERT_EQ (nano::dev::genesis->balance ().number () - 1, ctx.ledger ().weight (nano::dev::genesis_key.pub));
}

TEST (ledger_weight, open_legacy)
{
	auto ctx = nano::test::context::ledger_empty ();
	nano::keypair key;
	nano::send_block_builder builder1;
	auto send = builder1.make_block ()
		.previous (nano::dev::genesis->hash ())
		.destination (key.pub)
		.balance (nano::dev::genesis->balance ().number () - 1)
		.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
		.work (ctx.pool ().generate (nano::dev::genesis->hash ()).value ())
		.build ();
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), send));
	nano::open_block_builder builder2;
	auto open = builder2.make_block ()
		.source (send->hash ())
		.representative (key.pub)
		.account (key.pub)
		.sign (key.prv, key.pub)
		.work (ctx.pool ().generate (key.pub).value ())
		.build ();
	ASSERT_EQ (nano::dev::genesis->balance ().number () - 1, ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (0, ctx.ledger ().weight (key.pub));
	ASSERT_EQ (nano::block_status::progress, ctx.ledger ().process (ctx.store ().tx_begin_write (), open));
	ctx.ledger ().confirm (ctx.store ().tx_begin_write (), open->hash ());
	ASSERT_EQ (nano::dev::genesis->balance ().number () - 1, ctx.ledger ().weight (nano::dev::genesis_key.pub));
	ASSERT_EQ (1, ctx.ledger ().weight (key.pub));
}
