#include <nano/secure/account_iterator_impl.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_confirmed.hpp>
#include <nano/secure/ledger_set_unconfirmed.hpp>

template class nano::account_iterator<nano::ledger_set_any>;
template class nano::account_iterator<nano::ledger_set_confirmed>;
template class nano::account_iterator<nano::ledger_set_unconfirmed>;
