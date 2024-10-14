#include <nano/store/account.hpp>
#include <nano/store/typed_iterator_templates.hpp>

template class nano::store::typed_iterator<nano::account, nano::account_info>;

std::optional<nano::account_info> nano::store::account::get (store::transaction const & transaction, nano::account const & account)
{
	nano::account_info info;
	bool error = get (transaction, account, info);
	if (!error)
	{
		return info;
	}
	else
	{
		return std::nullopt;
	}
}
