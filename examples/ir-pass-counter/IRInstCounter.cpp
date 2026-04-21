// examples/ir-pass-counter/IRInstCounter.cpp
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

class IRInstCounter : public PassInfoMixin<IRInstCounter> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    unsigned Count = 0;
    for (auto &BB : F)
      Count += BB.size();
    errs() << "[IRInstCounter] " << F.getName() << ": " << Count
           << " instructions\n";
    return PreservedAnalyses::all();
  }
};

extern "C" ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "IRInstCounter", "0.1",
    [](PassBuilder &PB) {
      PB.registerOptimizerLastEPCallback(
        [](ModulePassManager &MPM, OptimizationLevel, ThinOrFullLTOPhase) {
          MPM.addPass(createModuleToFunctionPassAdaptor(IRInstCounter()));
        });
    }};
}
