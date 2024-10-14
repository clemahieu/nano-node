#pragma once

#include <lmdb/libraries/liblmdb/lmdb.h>

#include <iterator>
#include <utility>
#include <variant>

namespace nano::store::lmdb
{
class iterator
{
	MDB_cursor * cursor{ nullptr };
	std::variant<std::monostate, std::pair<MDB_val, MDB_val>> current;
	void update (std::pair<MDB_val, MDB_val> const & current);

public:
	using iterator_category = std::bidirectional_iterator_tag;
	using value_type = std::pair<MDB_val, MDB_val>;
	using pointer = value_type *;
	using const_pointer = value_type const *;
	using reference = value_type &;
	using const_reference = value_type const &;

	iterator (MDB_txn * tx, MDB_dbi dbi, MDB_val const & lower_bound = MDB_val{ 0, nullptr }) noexcept;
	iterator () noexcept = default;

	~iterator ();

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
