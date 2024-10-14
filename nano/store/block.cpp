#include <nano/store/block.hpp>
#include <nano/store/typed_iterator_templates.hpp>

template class nano::store::typed_iterator<nano::block_hash, nano::store::block_w_sideband>;
