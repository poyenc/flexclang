#include "FlexConfig.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>

using namespace llvm;

namespace flexclang {

static std::pair<std::string, std::string> splitNamePlugin(StringRef arg) {
  auto pos = arg.find(':');
  if (pos == StringRef::npos)
    return {arg.str(), ""};
  return {arg.substr(0, pos).str(), arg.substr(pos + 1).str()};
}

FlexConfig parseFlexArgs(SmallVectorImpl<const char *> &remainingArgs,
                         int argc, const char **argv) {
  FlexConfig config;

  for (int i = 1; i < argc; ++i) {
    StringRef arg(argv[i]);

    if (arg.starts_with("--flex-")) {
      config.originalFlexArgs.push_back(argv[i]);
    }

    if (arg.consume_front("--flex-disable-pass=")) {
      config.mirRules.push_back({MIRPassRule::Disable, arg.str(), ""});
    } else if (arg.consume_front("--flex-replace-pass=")) {
      auto [name, plugin] = splitNamePlugin(arg);
      if (plugin.empty()) {
        errs() << "flexclang: error: --flex-replace-pass requires <name>:<plugin.so> format\n";
        continue;
      }
      config.mirRules.push_back({MIRPassRule::Replace, name, plugin});
    } else if (arg.consume_front("--flex-insert-after=")) {
      auto [name, plugin] = splitNamePlugin(arg);
      if (plugin.empty()) {
        errs() << "flexclang: error: --flex-insert-after requires <name>:<plugin.so> format\n";
        continue;
      }
      config.mirRules.push_back({MIRPassRule::InsertAfter, name, plugin});
    } else if (arg.consume_front("--flex-disable-ir-pass=")) {
      config.irRules.push_back({IRPassRule::Disable, arg.str(), ""});
    } else if (arg.consume_front("--flex-config=")) {
      config.configFile = arg.str();
    } else if (arg == "--flex-list-passes") {
      config.listPasses = true;
    } else if (arg == "--flex-verbose") {
      config.verbose = true;
    } else if (arg == "--flex-verify-plugins") {
      config.verifyPlugins = true;
    } else if (arg == "--flex-dry-run") {
      config.dryRun = true;
    } else {
      remainingArgs.push_back(argv[i]);
    }
  }

  if (config.configFile.empty()) {
    if (const char *env = std::getenv("FLEXCLANG_CONFIG"))
      config.configFile = env;
  }

  return config;
}

bool FlexConfig::printDryRun() const {
  if (!hasModifications()) {
    errs() << "flexclang: [dry-run] no modifications configured\n";
    return false;
  }
  for (const auto &r : mirRules) {
    const char *acts[] = {"disable", "replace", "insert-after"};
    errs() << "flexclang: [dry-run] MIR " << acts[r.action]
           << " target='" << r.target << "'";
    if (!r.plugin.empty()) errs() << " plugin=" << r.plugin;
    errs() << "\n";
  }
  for (const auto &r : irRules) {
    const char *acts[] = {"disable", "load-plugin"};
    errs() << "flexclang: [dry-run] IR " << acts[r.action]
           << " target='" << r.target << "'";
    if (!r.plugin.empty()) errs() << " plugin=" << r.plugin;
    errs() << "\n";
  }
  return true;
}

bool parseFlexYAML(FlexConfig &config, StringRef path) {
  auto BufOrErr = MemoryBuffer::getFile(path);
  if (!BufOrErr) {
    errs() << "flexclang: error: cannot open config '" << path
           << "': " << BufOrErr.getError().message() << "\n";
    return false;
  }

  SourceMgr SM;
  yaml::Stream Stream(BufOrErr.get()->getBuffer(), SM);

  for (auto &Doc : Stream) {
    auto *Root = dyn_cast<yaml::MappingNode>(Doc.getRoot());
    if (!Root) continue;

    for (auto &KV : *Root) {
      auto *Key = dyn_cast<yaml::ScalarNode>(KV.getKey());
      if (!Key) continue;
      SmallString<32> KeyBuf;
      StringRef KeyRef = Key->getValue(KeyBuf);

      if (KeyRef == "mir-passes") {
        auto *Seq = dyn_cast<yaml::SequenceNode>(KV.getValue());
        if (!Seq) continue;
        for (auto &Item : *Seq) {
          auto *Map = dyn_cast<yaml::MappingNode>(&Item);
          if (!Map) continue;
          MIRPassRule rule{};
          std::string action;
          for (auto &F : *Map) {
            auto *FK = dyn_cast<yaml::ScalarNode>(F.getKey());
            auto *FV = dyn_cast<yaml::ScalarNode>(F.getValue());
            if (!FK || !FV) continue;
            SmallString<32> FKB, FVB;
            StringRef fk = FK->getValue(FKB), fv = FV->getValue(FVB);
            if (fk == "action") action = fv.str();
            else if (fk == "target") rule.target = fv.str();
            else if (fk == "plugin") rule.plugin = fv.str();
            else if (fk == "config") rule.config = fv.str();
          }
          if (action == "disable") rule.action = MIRPassRule::Disable;
          else if (action == "replace") rule.action = MIRPassRule::Replace;
          else if (action == "insert-after") rule.action = MIRPassRule::InsertAfter;
          else { errs() << "flexclang: warning: unknown mir action: " << action << "\n"; continue; }
          // CLI takes precedence: skip YAML rule if CLI already has one for same target
          bool dup = false;
          for (const auto &existing : config.mirRules) {
            if (existing.target == rule.target) { dup = true; break; }
          }
          if (!dup)
            config.mirRules.push_back(rule);
        }
      } else if (KeyRef == "ir-passes") {
        auto *Seq = dyn_cast<yaml::SequenceNode>(KV.getValue());
        if (!Seq) continue;
        for (auto &Item : *Seq) {
          auto *Map = dyn_cast<yaml::MappingNode>(&Item);
          if (!Map) continue;
          IRPassRule rule{};
          std::string action;
          for (auto &F : *Map) {
            auto *FK = dyn_cast<yaml::ScalarNode>(F.getKey());
            auto *FV = dyn_cast<yaml::ScalarNode>(F.getValue());
            if (!FK || !FV) continue;
            SmallString<32> FKB, FVB;
            StringRef fk = FK->getValue(FKB), fv = FV->getValue(FVB);
            if (fk == "action") action = fv.str();
            else if (fk == "target") rule.target = fv.str();
            else if (fk == "plugin") rule.plugin = fv.str();
          }
          if (action == "disable") rule.action = IRPassRule::Disable;
          else if (action == "load-plugin") rule.action = IRPassRule::LoadPlugin;
          else { errs() << "flexclang: warning: unknown ir action: " << action << "\n"; continue; }
          // CLI takes precedence: skip YAML rule if CLI already has one for same target
          bool dup = false;
          for (const auto &existing : config.irRules) {
            if (existing.target == rule.target) { dup = true; break; }
          }
          if (!dup)
            config.irRules.push_back(rule);
        }
      }
    }
  }
  return !Stream.failed();
}

} // namespace flexclang
