#include "FlexPassNameRegistry.h"
#include "llvm/PassInfo.h"
#include "llvm/PassRegistry.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

const void *resolvePassID(StringRef passArg) {
  const PassInfo *PI = PassRegistry::getPassRegistry()->getPassInfo(passArg);
  if (!PI) {
    errs() << "flexclang: error: unknown pass '" << passArg << "'\n";
    return nullptr;
  }
  return PI->getTypeInfo();
}

bool isCriticalPass(StringRef passArg) {
  static const StringSet<> Critical = {
      "si-lower-control-flow", "si-insert-waitcnts",
      "prologepilog",          "phi-node-elimination",
      "virtregrewriter",       "si-fix-sgpr-copies",
  };
  return Critical.contains(passArg);
}

bool isNonSubstitutablePass(StringRef passArg) {
  // AMDGPU passes added via insertPass() in AMDGPUTargetMachine.cpp.
  // disablePass()/substitutePass() only work for passes added via
  // addPass(AnalysisID). Passes inserted via insertPass() bypass the
  // substitution lookup and always run.
  static const StringSet<> Inserted = {
      "si-form-memory-clauses",
      "si-lower-control-flow",
      "si-optimize-exec-masking-pre-ra",
      "si-opt-vgpr-liverange",
      "si-wqm",
      "amdgpu-pre-ra-optimizations",
      "rewrite-partial-reg-uses",
  };
  return Inserted.contains(passArg);
}

} // namespace flexclang
