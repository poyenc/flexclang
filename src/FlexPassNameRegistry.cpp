#include "FlexPassNameRegistry.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/PassInfo.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

const void *resolvePassID(StringRef passArg, StringRef programName) {
  const PassInfo *PI = PassRegistry::getPassRegistry()->getPassInfo(passArg);
  if (!PI) {
    errs() << programName << ": error: unknown MIR pass '" << passArg
           << "' (use --flex-list-passes to see available passes)\n";
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

} // namespace flexclang
