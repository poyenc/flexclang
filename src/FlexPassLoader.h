#ifndef FLEXCLANG_FLEXPASSLOADER_H
#define FLEXCLANG_FLEXPASSLOADER_H

#include "llvm/ADT/StringRef.h"

namespace llvm {
class Pass;
} // namespace llvm

namespace flexclang {

/// Load MIR pass plugin. If configPath is non-empty, reads the file and
/// tries flexclangCreatePassWithConfig(contents) first.
/// Falls back to flexclangCreatePass(). Returns nullptr on failure.
llvm::Pass *loadMIRPassPlugin(llvm::StringRef soPath,
                               llvm::StringRef configPath = "");

} // namespace flexclang

#endif
