#include <nano/store/rocksdb/iterator.hpp>

#include <nano/lib/utility.hpp>

namespace nano::store::rocksdb
{
auto iterator::is_end () const -> bool
{
	return std::holds_alternative<std::monostate>(current);
}

void iterator::update ()
{
	if (iter->Valid ())
	{
		current = std::make_pair (iter->key (), iter->value ());
	}
	else
	{
		current = std::monostate{};
	}
}

iterator::iterator (::rocksdb::DB * db, std::variant<::rocksdb::Transaction *, ::rocksdb::ReadOptions *> snapshot, ::rocksdb::ColumnFamilyHandle * table, ::rocksdb::Slice const & lower_bound) noexcept
{
	auto iterator = std::visit ([&] (auto && ptr) {
		using V = std::decay_t<decltype(ptr)>;
		if constexpr (std::is_same_v<V, ::rocksdb::Transaction *>)
		{
			::rocksdb::ReadOptions ropts;
			ropts.fill_cache = false;
			return ptr->GetIterator (ropts, table);
		}
		else if constexpr (std::is_same_v<V, ::rocksdb::ReadOptions *>)
		{
			ptr->fill_cache = false;
			return db->NewIterator (*ptr, table);
		}
		else
		{
			static_assert (false, "Unsupported variant");
		}
	}, snapshot);
	iter.reset (iterator);
	if (lower_bound.size () != 0)
	{
		iter->Seek (lower_bound);
		update ();
	}
	else
	{
		iter->SeekToFirst ();
		update ();
	}
}

iterator::iterator (iterator && other) noexcept
{
	*this = std::move (other);
}

auto iterator::operator= (iterator && other) noexcept -> iterator &
{
	iter = std::move (other.iter);
	current = other.current;
	other.current = std::monostate{};
	return *this;
}

auto iterator::operator++ () -> iterator &
{
	release_assert (!is_end ());
	iter->Next ();
	update ();
}

auto iterator::operator-- () -> iterator &
{
	release_assert (!is_end ());
	iter->Prev ();
	update ();
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
	return is_end () == other.is_end () && (is_end () || std::get<value_type> (current) == std::get<value_type> (other.current));
}
} // namespace nano::store::lmdb
