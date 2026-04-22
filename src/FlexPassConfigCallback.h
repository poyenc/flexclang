#ifndef FLEXCLANG_FLEXPASSCONFIGCALLBACK_H
#define FLEXCLANG_FLEXPASSCONFIGCALLBACK_H

#include "FlexConfig.h"
#include <memory>
#include <string>
#include <vector>

namespace flexclang {

/// Register the TargetPassConfig callback. Must be called before
/// ExecuteCompilerInvocation(). The config reference must outlive compilation.
/// If mirPassNames is non-null, --flex-list-passes collects MIR pass names
/// into it instead of printing inline.
void registerFlexPassConfigCallback(
    const FlexConfig &config,
    std::shared_ptr<std::vector<std::string>> mirPassNames = nullptr);

} // namespace flexclang

#endif
