#include "FlexPassConfigCallback.h"
#include "FlexPassLoader.h"
#include "FlexPassNameRegistry.h"
#include "FlexConfig.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/LegacyPassManagers.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/PassInfo.h"
#include "llvm/PassRegistry.h"
#include "llvm/Target/RegisterTargetPassConfigCallback.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

namespace {

/// A MachineFunctionPass inserted at the start of the MIR pipeline that
/// introspects its own FPPassManager to collect all MIR pass names.
/// Collects once on the first MachineFunction, then does nothing.
class MIRPassListPrinter : public MachineFunctionPass {
  bool Collected = false;
  std::shared_ptr<std::vector<std::string>> Names;

public:
  static char ID;
  MIRPassListPrinter(std::shared_ptr<std::vector<std::string>> N)
      : MachineFunctionPass(ID), Names(std::move(N)) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    if (Collected || !Names)
      return false;
    Collected = true;

    AnalysisResolver *R = getResolver();
    if (!R)
      return false;

    PMDataManager &PMD = R->getPMDataManager();
    if (PMD.getPassManagerType() != PMT_FunctionPassManager)
      return false;
    auto &FPM = static_cast<FPPassManager &>(PMD);

    unsigned N = FPM.getNumContainedPasses();
    for (unsigned i = 0; i < N; ++i) {
      Pass *P = FPM.getContainedPass(i);
      if (P == this)
        continue;
      const PassInfo *PI =
          PassRegistry::getPassRegistry()->getPassInfo(P->getPassID());
      if (PI && !PI->getPassArgument().empty())
        Names->push_back(PI->getPassArgument().str());
      else
        Names->push_back(P->getPassName().str());
    }
    return false;
  }

  StringRef getPassName() const override { return "flexclang-pass-lister"; }
};

char MIRPassListPrinter::ID = 0;

} // anonymous namespace

static std::unique_ptr<RegisterTargetPassConfigCallback> CallbackReg;

void registerFlexPassConfigCallback(
    const FlexConfig &config,
    std::shared_ptr<std::vector<std::string>> mirPassNames) {
  CallbackReg = std::make_unique<RegisterTargetPassConfigCallback>(
      [&config, mirPassNames](TargetMachine &TM, PassManagerBase &PM,
                              TargetPassConfig *PassConfig) {
        if (TM.getTargetTriple().getArch() != Triple::amdgcn)
          return;

        // Insert MIR pass lister after FinalizeISel when we need to know
        // which passes actually ran: for --flex-list-passes output, and for
        // runtime detection of disable/replace failures (passes added via
        // insertPass() bypass disablePass()/substitutePass()).
        bool hasMIRDisableOrReplace = false;
        for (const auto &rule : config.mirRules) {
          if (rule.action == MIRPassRule::Disable ||
              rule.action == MIRPassRule::Replace) {
            hasMIRDisableOrReplace = true;
            break;
          }
        }
        if ((config.listPasses || hasMIRDisableOrReplace) && mirPassNames)
          PassConfig->insertPass(&FinalizeISelID,
                                 new MIRPassListPrinter(mirPassNames));

        for (const auto &rule : config.mirRules) {
          switch (rule.action) {
          case MIRPassRule::Disable: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            PassConfig->disablePass(ID);
            if (config.verbose)
              errs() << "flexclang: requesting disable of MIR pass '" << rule.target << "'\n";
            break;
          }
          case MIRPassRule::Replace: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            Pass *Replacement = loadMIRPassPlugin(rule.plugin, rule.config,
                                                   config.verbose);
            if (!Replacement) break;
            PassConfig->substitutePass(ID, Replacement);
            if (config.verifyPlugins)
              PassConfig->insertPass(Replacement->getPassID(),
                                     createMachineVerifierPass(
                                         "After flex plugin: " + rule.plugin));
            if (config.verbose)
              errs() << "flexclang: requesting replace of '" << rule.target
                     << "' with " << rule.plugin << "\n";
            break;
          }
          case MIRPassRule::InsertAfter: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            Pass *NewPass = loadMIRPassPlugin(rule.plugin, rule.config,
                                                      config.verbose);
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
