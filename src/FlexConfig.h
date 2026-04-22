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
  bool listPasses = false;
  bool verbose = false;
  bool verifyPlugins = false;
  bool dryRun = false;
  std::vector<std::string> originalFlexArgs; // Raw --flex-* strings for driver mode injection

  bool hasModifications() const {
    return !mirRules.empty() || !irRules.empty();
  }

  /// Print dry-run summary of all configured modifications to stderr.
  /// Returns true if there were modifications to print.
  bool printDryRun() const;
};

/// Parse --flex-* flags from argv, placing non-flex args into remainingArgs.
/// Starts at argv[1], skipping argv[0] (expected to be program name or sentinel
/// like "-cc1"). Callers must account for this: pass the full argc/argv with
/// the program name at argv[0].
FlexConfig parseFlexArgs(llvm::SmallVectorImpl<const char *> &remainingArgs,
                         int argc, const char **argv);

bool parseFlexYAML(FlexConfig &config, llvm::StringRef path);

} // namespace flexclang

#endif
