#include "FlexPassConfigCallback.h"
#include "FlexPassLoader.h"
#include "FlexPassNameRegistry.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Target/RegisterTargetPassConfigCallback.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

static std::unique_ptr<RegisterTargetPassConfigCallback> CallbackReg;

void registerFlexPassConfigCallback(const FlexConfig &config) {
  CallbackReg = std::make_unique<RegisterTargetPassConfigCallback>(
      [&config](TargetMachine &TM, PassManagerBase &PM,
                TargetPassConfig *PassConfig) {
        if (TM.getTargetTriple().getArch() != Triple::amdgcn)
          return;

        for (const auto &rule : config.mirRules) {
          switch (rule.action) {
          case MIRPassRule::Disable: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            if (isCriticalPass(rule.target))
              errs() << "flexclang: warning: disabling '" << rule.target
                     << "' may cause incorrect code generation\n";
            PassConfig->disablePass(ID);
            if (config.verbose)
              errs() << "flexclang: disabled MIR pass '" << rule.target << "'\n";
            break;
          }
          case MIRPassRule::Replace: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            Pass *Replacement = loadMIRPassPlugin(rule.plugin, rule.config);
            if (!Replacement) break;
            PassConfig->substitutePass(ID, Replacement);
            if (config.verbose)
              errs() << "flexclang: replaced '" << rule.target
                     << "' with " << rule.plugin << "\n";
            break;
          }
          case MIRPassRule::InsertAfter: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            Pass *NewPass = loadMIRPassPlugin(rule.plugin, rule.config);
            if (!NewPass) break;
            PassConfig->insertPass(ID, NewPass);
            if (config.verifyPlugins)
              PassConfig->insertPass(NewPass->getPassID(),
                                     createMachineVerifierPass(
                                         "After flex plugin: " + rule.plugin));
            if (config.verbose)
              errs() << "flexclang: inserted after '" << rule.target
                     << "' from " << rule.plugin << "\n";
            break;
          }
          }
        }
      });
}

} // namespace flexclang
