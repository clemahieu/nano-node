#pragma once

#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/utilities/transaction.h>

#include <iterator>
#include <memory>
#include <utility>
#include <variant>

namespace nano::store::rocksdb
{
class iterator
{
	std::unique_ptr<::rocksdb::Iterator> iter;
	std::variant<std::monostate, std::pair<::rocksdb::Slice, ::rocksdb::Slice>> current;
	void update ();

public:
	using iterator_category = std::bidirectional_iterator_tag;
	using value_type = std::pair<::rocksdb::Slice, ::rocksdb::Slice>;
	using pointer = value_type *;
	using const_pointer = value_type const *;
	using reference = value_type &;
	using const_reference = value_type const &;

	iterator (::rocksdb::DB * db, std::variant<::rocksdb::Transaction *, ::rocksdb::ReadOptions *> snapshot, ::rocksdb::ColumnFamilyHandle * table, ::rocksdb::Slice const & lower_bound = ::rocksdb::Slice{}) noexcept;
	iterator () noexcept = default;

	iterator (iterator const &) = delete;
	auto operator= (iterator const &) -> iterator & = delete;
	
	iterator (iterator && other_a) noexcept;
	auto operator= (iterator && other) noexcept -> iterator &;

	auto operator++ () -> iterator &;
	auto operator-- () -> iterator &;
	auto operator-> () const -> const_pointer;
	auto operator* () const -> const_reference;
	auto operator== (iterator const & other) const -> bool;
	auto operator!= (iterator const & other) const -> bool
	{
		return !(*this == other);
	}
	bool is_end () const;
};
}
