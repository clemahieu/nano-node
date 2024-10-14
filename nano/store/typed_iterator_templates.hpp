#include <nano/store/typed_iterator.hpp>
#include <nano/store/lmdb/db_val.hpp>
#include <nano/store/db_val_impl.hpp>

namespace nano::store
{
template <typename Key, typename Value>
void typed_iterator<Key, Value>::update ()
{
	// FIXME Don't convert via lmdb::db_val, this is just a placeholder
	auto const & data = *iter;
	lmdb::db_val key_val{ MDB_val{ data.first.size (), const_cast<void *> (reinterpret_cast<void const *> (data.first.data ())) } };
	lmdb::db_val value_val{ MDB_val{ data.second.size (), const_cast<void *> (reinterpret_cast<void const *> (data.second.data ())) } };
	current = std::make_pair (static_cast<Key> (key_val), static_cast<Value> (value_val));
}

template <typename Key, typename Value>
typed_iterator<Key, Value>::typed_iterator (iterator && iter) noexcept :
iter{ std::move (iter) }
{
}

template <typename Key, typename Value>
typed_iterator<Key, Value>::typed_iterator (typed_iterator && other) noexcept
{
	*this = std::move (other);
}

template <typename Key, typename Value>
auto typed_iterator<Key, Value>::operator= (typed_iterator && other) noexcept -> typed_iterator &
{
	iter = std::move (other.iter);
}

template <typename Key, typename Value>
auto typed_iterator<Key, Value>::operator++ () -> typed_iterator<Key, Value> &
{
	++iter;
	update ();
	return *this;
}

template <typename Key, typename Value>
auto typed_iterator<Key, Value>::operator-- () -> typed_iterator<Key, Value> &
{
	--iter;
	update ();
	return *this;
}

template <typename Key, typename Value>
auto typed_iterator<Key, Value>::operator-> () const -> const_pointer
{
	return std::get_if<value_type> (&current);
}

template <typename Key, typename Value>
auto typed_iterator<Key, Value>::operator* () const -> const_reference
{
	return std::get<value_type> (current);
}

template <typename Key, typename Value>
auto typed_iterator<Key, Value>::operator== (typed_iterator<Key, Value> const & other) const -> bool
{
	return iter == other.iter;
}
}
