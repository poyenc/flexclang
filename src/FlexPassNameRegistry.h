#ifndef FLEXCLANG_FLEXPASSNAMEREGISTRY_H
#define FLEXCLANG_FLEXPASSNAMEREGISTRY_H

#include "llvm/ADT/StringRef.h"

namespace flexclang {

/// Resolve pass argument string to AnalysisID. Returns nullptr if not found.
const void *resolvePassID(llvm::StringRef passArg);

} // namespace flexclang

#endif
