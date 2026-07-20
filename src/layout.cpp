#include "mim/plug/layout/layout.h"

#include <mim/phase.h>
#include <mim/plugin.h>

using namespace mim;

/// Registers normalizers as well as Phase%s and Pass%es for the Axm%s of this Plugin.
extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"layout", MIM_VERSION, plug::layout::register_normalizers, nullptr};
}
