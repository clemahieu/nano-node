#pragma once

#include <nano/store/iterator.hpp>

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
template <typename Key, typename Value>
class typed_iterator final
{
public:
	using iterator_category = std::bidirectional_iterator_tag;
	using value_type = std::pair<Key, Value>;
	using pointer = value_type *;
	using const_pointer = value_type const *;
	using reference = value_type &;
	using const_reference = value_type const &;

private:
	iterator iter;
	std::variant<std::monostate, value_type> current;
	void update ();

public:
	typed_iterator (iterator && iter) noexcept;
	typed_iterator () noexcept = default;

	typed_iterator (typed_iterator const &) = delete;
	auto operator= (typed_iterator const &) -> typed_iterator & = delete;
	
	typed_iterator (typed_iterator && other) noexcept;
	auto operator= (typed_iterator && other) noexcept -> typed_iterator &;

	auto operator++ () -> typed_iterator &;
	auto operator-- () -> typed_iterator &;
	auto operator-> () const -> const_pointer;
	auto operator* () const -> const_reference;
	auto operator== (typed_iterator const & other) const -> bool;
	auto operator!= (typed_iterator const & other) const -> bool
	{
		return !(*this == other);
	}
};
} // namespace nano::store

