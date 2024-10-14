#include <nano/store/pruned.hpp>
#include <nano/store/typed_iterator_templates.hpp>

template class nano::store::typed_iterator<nano::block_hash, std::nullptr_t>;
