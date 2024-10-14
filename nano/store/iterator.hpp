#pragma once

#include <nano/store/lmdb/iterator.hpp>
#include <nano/store/rocksdb/iterator.hpp>

#include <cstddef>
#include <iterator>
#include <memory>
#include <span>
#include <utility>

namespace nano::store
{
/**
 * Iterates the key/value pairs of a transaction
 */
class iterator final
{
public:
	using iterator_category = std::bidirectional_iterator_tag;
	using value_type = std::pair<std::span<uint8_t const>, std::span<uint8_t const>>;
	using pointer = value_type *;
	using const_pointer = value_type const *;
	using reference = value_type &;
	using const_reference = value_type const &;

private:
	std::variant<lmdb::iterator, rocksdb::iterator> internals;
	std::variant<std::monostate, value_type> current;
	void update ();

public:
	iterator (std::variant<lmdb::iterator, rocksdb::iterator> && internals) noexcept;
	iterator () noexcept = default;

	iterator (iterator const &) = delete;
	auto operator= (iterator const &) -> iterator & = delete;
	
	iterator (iterator && other) noexcept;
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
};
} // namespace nano::store
