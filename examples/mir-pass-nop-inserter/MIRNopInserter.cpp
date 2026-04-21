// examples/mir-pass-nop-inserter/MIRNopInserter.cpp
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/MachineFunction.h"

// AMDGPU-specific headers for opcode enums (e.g., AMDGPU::S_NOP)
#include "AMDGPU.h"
#include "SIInstrInfo.h"

using namespace llvm;

class MIRNopInserter : public MachineFunctionPass {
public:
  static char ID;
  MIRNopInserter() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    bool Changed = false;

    for (auto &MBB : MF) {
      for (auto MI = MBB.begin(); MI != MBB.end(); ++MI) {
        // Check if this is an MFMA instruction (opcode name contains "MFMA")
        if (TII->getName(MI->getOpcode()).contains("MFMA")) {
          BuildMI(MBB, MI, MI->getDebugLoc(),
                  TII->get(AMDGPU::S_NOP)).addImm(0);
          Changed = true;
        }
      }
    }

    if (Changed)
      errs() << "[MIRNopInserter] Inserted NOPs in " << MF.getName() << "\n";
    return Changed;
  }

  StringRef getPassName() const override { return "mir-nop-inserter"; }
};
char MIRNopInserter::ID = 0;

extern "C" MachineFunctionPass* flexclangCreatePass() {
  return new MIRNopInserter();
}

extern "C" const char* flexclangPassName() {
  return "mir-nop-inserter";
}
