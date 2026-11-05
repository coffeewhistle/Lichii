// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"
#include "common/env.h"

namespace VideoCommon::RenderTrace {

inline bool Enabled() {
    static const bool enabled = Common::Env::IsSet("EDEN_NX2_RENDER_TRACE");
    return enabled;
}

inline bool SampleFrame(u64 frame) {
    return Enabled() && frame % 600 == 0;
}

} // namespace VideoCommon::RenderTrace
