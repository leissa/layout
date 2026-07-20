#include "mim/world.h"

#include "mim/plug/layout/layout.h"

namespace mim::plug::layout {

const Def* normalize_const(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    return world.lit(world.type_idx(arg), 42);
}

MIM_layout_NORMALIZER_IMPL

} // namespace mim::plug::layout
