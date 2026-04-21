#ifndef FLEXCLANG_FLEXCONFIG_H
#define FLEXCLANG_FLEXCONFIG_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace flexclang {

struct MIRPassRule {
  enum Action { Disable, Replace, InsertAfter };
  Action action;
  std::string target;
  std::string plugin;
  std::string config; // optional: plugin-specific config file path
};

struct IRPassRule {
  enum Action { Disable, LoadPlugin };
  Action action;
  std::string target;
  std::string plugin;
};

struct FlexConfig {
  std::vector<MIRPassRule> mirRules;
  std::vector<IRPassRule> irRules;
  std::string configFile;
  std::string latencyModelFile;
  bool listPasses = false;
  bool verbose = false;
  bool verifyPlugins = false;
  bool dryRun = false;
  std::vector<std::string> originalFlexArgs; // Raw --flex-* strings for driver mode injection

  bool hasModifications() const {
    return !mirRules.empty() || !irRules.empty();
  }
};

FlexConfig parseFlexArgs(llvm::SmallVectorImpl<const char *> &remainingArgs,
                         int argc, const char **argv);

bool parseFlexYAML(FlexConfig &config, llvm::StringRef path);

} // namespace flexclang

#endif
