#include <nano/store/iterator.hpp>

namespace nano::store
{
void iterator::update ()
{
	std::visit ([&] (auto && arg) {
		using T = std::decay_t<decltype(arg)>;
		if constexpr (std::is_same_v<T, lmdb::iterator>)
		{
			if (!arg.is_end ())
			{
				auto & current = *arg;
				std::span<uint8_t const> key{ reinterpret_cast<uint8_t const *> (current.first.mv_data), current.first.mv_size };
				std::span<uint8_t const> value{ reinterpret_cast<uint8_t const *> (current.second.mv_data), current.second.mv_size };
				this->current = std::make_pair (key, value);
			}
			else
			{
				current = std::monostate{};
			}
		}
		else if constexpr (std::is_same_v<T, rocksdb::iterator>)
			if (!arg.is_end ())
			{
				auto & current = *arg;
				std::span<uint8_t const> key{ reinterpret_cast<uint8_t const *> (current.first.data ()), current.first.size () };
				std::span<uint8_t const> value{ reinterpret_cast<uint8_t const *> (current.second.data ()), current.second.size () };
				this->current = std::make_pair (key, value);
			}
			else
			{
				current = std::monostate{};
			}
		else
			static_assert (false, "Missing variant handler");
	}, internals);
}

iterator::iterator (std::variant<lmdb::iterator, rocksdb::iterator> && internals) noexcept :
internals{ std::move (internals) }
{
}
	
iterator::iterator (iterator && other) noexcept
{
	*this = std::move (other);
}

auto iterator::operator= (iterator && other) noexcept -> iterator &
{
	internals = std::move (other.internals);
}

auto iterator::operator++ () -> iterator &
{
	std::visit ([] (auto && arg) {
		++arg;
	}, internals);
	update ();
	return *this;
}

auto iterator::operator-- () -> iterator &
{
	std::visit ([] (auto && arg) {
		--arg;
	}, internals);
	update ();
	return *this;
}

auto iterator::operator-> () const -> const_pointer
{
	return std::get_if<value_type> (&current);
}

auto iterator::operator* () const -> const_reference
{
	return std::get<value_type> (current);
}

auto iterator::operator== (iterator const & other) const -> bool
{
	return internals == other.internals;
}
}
