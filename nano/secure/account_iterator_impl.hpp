#include <nano/secure/account_info.hpp>
#include <nano/secure/account_iterator.hpp>

template <typename T>
nano::account_iterator<T>::account_iterator ()
{
}

template <typename T>
nano::account_iterator<T>::account_iterator (store::transaction const & transaction, T const & view, std::optional<std::pair<nano::account, nano::account_info>> const & item) :
	transaction{ &transaction },
	view{ &view },
	item{ item }
{
}

template <typename T>
bool nano::account_iterator<T>::operator== (account_iterator const & other) const
{
	debug_assert (view == nullptr || other.view == nullptr || view == other.view);
	return item == other.item;
}

template <typename T>
bool nano::account_iterator<T>::operator!= (account_iterator const & other) const
{
	return !(*this == other);
}

template <typename T>
auto nano::account_iterator<T>::operator++ () -> account_iterator<T> &
{
	auto next = item.value ().first.number () + 1;
	if (next != 0)
	{
		*this = view->account_lower_bound (*transaction, next);
	}
	else
	{
		*this = account_iterator<T>{};
	}
	return *this;
}

template <typename T>
std::pair<nano::account, nano::account_info> const & nano::account_iterator<T>::operator* () const
{
	return item.value ();
}

template <typename T>
std::pair<nano::account, nano::account_info> const * nano::account_iterator<T>::operator->() const
{
	return &item.value ();
}
