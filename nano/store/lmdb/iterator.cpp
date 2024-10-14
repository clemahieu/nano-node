#include <nano/store/lmdb/iterator.hpp>

#include <nano/lib/utility.hpp>

namespace nano::store::lmdb
{
auto iterator::is_end () const -> bool
{
	return std::holds_alternative<std::monostate>(current);
}

void iterator::update (std::pair<MDB_val, MDB_val> const & current)
{
	if (current.first.mv_size != 0)
	{
		this->current = current;
	}
	else
	{
		this->current = std::monostate{};
	}
}

iterator::iterator (MDB_txn * tx, MDB_dbi dbi, MDB_val const & lower_bound) noexcept
{
	auto open_status = mdb_cursor_open (tx, dbi, &cursor);
	release_assert (open_status == MDB_SUCCESS);
	value_type init;
	init.first = lower_bound;
	auto operation = lower_bound.mv_size != 0 ? MDB_SET_RANGE : MDB_FIRST;
	auto status = mdb_cursor_get (cursor, &init.first, &init.second, operation);
	release_assert (status == MDB_SUCCESS || status == MDB_NOTFOUND);
	update (init);
}

iterator::iterator (iterator && other) noexcept
{
	*this = std::move (other);
}

iterator::~iterator ()
{
	if (cursor)
	{
		mdb_cursor_close (cursor);
	}
}

auto iterator::operator= (iterator && other) noexcept -> iterator &
{
	cursor = other.cursor;
	other.cursor = nullptr;
	current = other.current;
	other.current = std::monostate{};
	return *this;
}

auto iterator::operator++ () -> iterator &
{
	release_assert (!is_end ());
	value_type init;
	auto status = mdb_cursor_get (cursor, &init.first, &init.second, MDB_NEXT);
	release_assert (status == MDB_SUCCESS || status == MDB_NOTFOUND);
	update (init);
}

auto iterator::operator-- () -> iterator &
{
	auto operation = is_end () ? MDB_LAST : MDB_PREV;
	value_type init;
	auto status = mdb_cursor_get (cursor, &init.first, &init.second, operation);
	release_assert (status == MDB_SUCCESS || status == MDB_NOTFOUND);
	update (init);
}

auto iterator::operator-> () const -> const_pointer
{
	release_assert (!is_end ());
	return std::get_if<value_type> (&current);
}

auto iterator::operator* () const -> const_reference
{
	release_assert (!is_end ());
	return std::get<value_type> (current);
}

auto iterator::operator== (iterator const & other) const -> bool
{
	if (is_end () != other.is_end ())
	{
		return false;
	}
	if (is_end ())
	{
		return true;
	}
	auto & lhs = std::get<value_type> (current);
	auto & rhs = std::get<value_type> (other.current);
	auto result = lhs.first.mv_data == rhs.first.mv_data;
	if (!result)
	{
		return result;
	}
	debug_assert (std::make_pair (lhs.first.mv_data, lhs.first.mv_size) == std::make_pair (rhs.first.mv_data, rhs.first.mv_size) && std::make_pair (lhs.second.mv_data, lhs.second.mv_size) == std::make_pair (rhs.second.mv_data, rhs.second.mv_size));
	return result;
}
} // namespace nano::store::lmdb
