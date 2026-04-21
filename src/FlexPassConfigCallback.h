#ifndef FLEXCLANG_FLEXPASSCONFIGCALLBACK_H
#define FLEXCLANG_FLEXPASSCONFIGCALLBACK_H

#include "FlexConfig.h"

namespace flexclang {

/// Register the TargetPassConfig callback. Must be called before
/// ExecuteCompilerInvocation(). The config reference must outlive compilation.
void registerFlexPassConfigCallback(const FlexConfig &config);

} // namespace flexclang

#endif
