#ifndef FLEXCLANG_FLEXPASSNAMEREGISTRY_H
#define FLEXCLANG_FLEXPASSNAMEREGISTRY_H

#include "llvm/ADT/StringRef.h"

namespace flexclang {

/// Resolve pass argument string to AnalysisID. Returns nullptr if not found.
const void *resolvePassID(llvm::StringRef passArg);

/// Returns true if disabling this pass is likely to cause miscompilation.
bool isCriticalPass(llvm::StringRef passArg);

/// Returns true if this AMDGPU pass is added via insertPass() and cannot
/// be disabled via disablePass()/substitutePass(). These passes bypass
/// the TargetPassConfig substitution mechanism.
bool isNonSubstitutablePass(llvm::StringRef passArg);

} // namespace flexclang

#endif
