#include "FlexPassNameRegistry.h"
#include "llvm/PassInfo.h"
#include "llvm/PassRegistry.h"
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

} // namespace flexclang
