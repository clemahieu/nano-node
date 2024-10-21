#pragma once

#include <nano/store/pruned.hpp>

namespace nano::store::rocksdb
{
class component;
}
namespace nano::store::rocksdb
{
class pruned : public nano::store::pruned
{
private:
	nano::store::rocksdb::component & store;

public:
	explicit pruned (nano::store::rocksdb::component & store_a);
	void put (store::write_transaction const & transaction_a, nano::block_hash const & hash_a) override;
	void del (store::write_transaction const & transaction_a, nano::block_hash const & hash_a) override;
	bool exists (store::transaction const & transaction_a, nano::block_hash const & hash_a) const override;
	nano::block_hash random (store::transaction const & transaction_a) override;
	size_t count (store::transaction const & transaction_a) const override;
	void clear (store::write_transaction const & transaction_a) override;
	iterator begin (store::transaction const & transaction_a, nano::block_hash const & hash_a) const override;
	iterator begin (store::transaction const & transaction_a) const override;
	iterator end (store::transaction const & transaction_a) const override;
	void for_each_par (std::function<void (store::read_transaction const &, iterator, iterator)> const & action_a) const override;
};
} // namespace nano::store::rocksdb
