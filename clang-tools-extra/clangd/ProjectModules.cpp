//===------------------ ProjectModules.cpp ---------  ------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ProjectModules.h"
#include "Compiler.h"
#include "support/Logger.h"
#include "clang/DependencyScanning/DependencyScanningService.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/DependencyScanningTool.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/TargetParser/Host.h"

namespace clang::clangd {
namespace {

llvm::SmallString<128> normalizePath(PathRef Path) {
  llvm::SmallString<128> Result(Path);
  llvm::sys::path::remove_dots(Result, /*remove_dot_dot=*/true);
  llvm::sys::path::native(Result, llvm::sys::path::Style::posix);
  return Result;
}

std::string normalizePath(PathRef Path, PathRef WorkingDir) {
  if (Path.empty())
    return {};

  llvm::SmallString<128> Result;
  if (llvm::sys::path::is_absolute(Path) || WorkingDir.empty())
    Result = Path;
  else {
    Result = WorkingDir;
    llvm::sys::path::append(Result, Path);
  }

  return normalizePath(Result).str().str();
}

/// The information related to modules parsed from compile commands.
/// Including the source file, the module file it produces (if it is a
/// producer), and the module and the corresponding module files it
/// requires (if it is a consumer)
struct ParsedCompileCommandInfo {
  std::string SourceFile;
  std::optional<std::string> OutputModuleFile;
  // Map from required module name to the module file path.
  llvm::StringMap<std::string> RequiredModuleFiles;
};

/// Get ParsedCompileCommandInfo by looking at the '--precompile',
/// '-fmodule-file=' and '-fmodule-file=' commands in the compile command.
std::optional<ParsedCompileCommandInfo>
parseCompileCommandInfo(tooling::CompileCommand Cmd, const ThreadsafeFS &TFS) {
  auto FS = TFS.view(std::nullopt);
  auto Tokenizer = llvm::Triple(llvm::sys::getProcessTriple()).isOSWindows()
                       ? llvm::cl::TokenizeWindowsCommandLine
                       : llvm::cl::TokenizeGNUCommandLine;
  tooling::addExpandedResponseFiles(Cmd.CommandLine, Cmd.Directory, Tokenizer,
                                    *FS);

  ParsedCompileCommandInfo Result;
  Result.SourceFile = normalizePath(Cmd.Filename, Cmd.Directory);

  bool SawPrecompile = false;
  for (size_t I = 1; I < Cmd.CommandLine.size(); ++I) {
    llvm::StringRef Arg = Cmd.CommandLine[I];
    if (Arg == "--precompile") {
      SawPrecompile = true;
      continue;
    }

    if (Arg.consume_front("-fmodule-output=")) {
      Result.OutputModuleFile = normalizePath(Arg, Cmd.Directory);
      continue;
    }
    if (Arg == "-fmodule-output" && I + 1 < Cmd.CommandLine.size()) {
      Result.OutputModuleFile =
          normalizePath(Cmd.CommandLine[++I], Cmd.Directory);
      continue;
    }
    if (SawPrecompile && Arg == "-o" && I + 1 < Cmd.CommandLine.size()) {
      Result.OutputModuleFile =
          normalizePath(Cmd.CommandLine[++I], Cmd.Directory);
      continue;
    }
    if (SawPrecompile && Arg.starts_with("-o") && Arg.size() > 2) {
      Result.OutputModuleFile = normalizePath(Arg.drop_front(2), Cmd.Directory);
      continue;
    }

    if (!Arg.consume_front("-fmodule-file="))
      continue;

    auto Sep = Arg.find('=');
    if (Sep == llvm::StringRef::npos || Sep == 0 || Sep + 1 == Arg.size())
      continue;

    Result.RequiredModuleFiles[Arg.take_front(Sep)] =
        normalizePath(Arg.drop_front(Sep + 1), Cmd.Directory);
  }

  return Result;
}

std::optional<tooling::CompileCommand>
getCompileCommandForFile(const clang::tooling::CompilationDatabase &CDB,
                         PathRef FilePath,
                         const ProjectModules::CommandMangler &Mangler) {
  auto Candidates = CDB.getCompileCommands(FilePath);
  if (Candidates.empty())
    return std::nullopt;

  // Choose the first candidates as the compile commands as the file.
  // Following the same logic with
  // DirectoryBasedGlobalCompilationDatabase::getCompileCommand.
  tooling::CompileCommand Cmd = std::move(Candidates.front());

  if (Mangler)
    Mangler(Cmd, FilePath);

  return Cmd;
}

/// A scanner to query the dependency information for C++20 Modules.
///
/// The scanner can scan a single file with `scan(PathRef)` member function
/// or scan the whole project with `globalScan(vector<PathRef>)` member
/// function. See the comments of `globalScan` to see the details.
///
/// The ModuleDependencyScanner can get the directly required module names for a
/// specific source file. Also the ModuleDependencyScanner can get the source
/// file declaring the primary module interface for a specific module name.
///
/// IMPORTANT NOTE: we assume that every module unit is only declared once in a
/// source file in the project. But the assumption is not strictly true even
/// besides the invalid projects. The language specification requires that every
/// module unit should be unique in a valid program. But a project can contain
/// multiple programs. Then it is valid that we can have multiple source files
/// declaring the same module in a project as long as these source files don't
/// interfere with each other.
class ModuleDependencyScanner {
public:
  ModuleDependencyScanner(
      std::shared_ptr<const clang::tooling::CompilationDatabase> CDB,
      const ThreadsafeFS &TFS)
      : CDB(CDB), TFS(TFS) {}

  /// The scanned modules dependency information for a specific source file.
  struct ModuleDependencyInfo {
    /// The name of the module if the file is a module unit.
    std::optional<std::string> ModuleName;
    /// A list of names for the modules that the file directly depends.
    std::vector<std::string> RequiredModules;
  };

  std::optional<std::string>
  getModuleNameForSource(PathRef File,
                         const ProjectModules::CommandMangler &Mangler);

  std::optional<std::string>
  getSourceForModuleName(llvm::StringRef ModuleName,
                         const ProjectModules::CommandMangler &Mangler);

  /// Return the direct required modules. Indirect required modules are not
  /// included.
  std::vector<std::string>
  getRequiredModules(PathRef File,
                     const ProjectModules::CommandMangler &Mangler);

  void invalidate() { invalidateGlobalScan(); }

private:
  /// Scanning every source file in the current project to get the
  /// <module-name> to <module-unit-source> map.
  /// TODO: Replace the expensive project-wide scan with incrementally
  /// maintained module information.
  /// Returns whether this call performed the scan.
  bool globalScan(const ProjectModules::CommandMangler &Mangler);

  void invalidateGlobalScan() {
    GlobalScanned = false;
    ModuleNameToSource.clear();
  }

  /// Get the source file from the module name. Note that the language
  /// guarantees all the module names are unique in a valid program.
  /// This function should only be called after globalScan.
  ///
  /// TODO: We should handle the case that there are multiple source files
  /// declaring the same module.
  PathRef lookupSourceForModuleName(llvm::StringRef ModuleName) const;

  /// Scanning the single file specified by \param FilePath.
  std::optional<ModuleDependencyInfo>
  scan(PathRef FilePath, const ProjectModules::CommandMangler &Mangler);

  std::unique_ptr<dependencies::DependencyScanningService> scanningService();
  std::optional<ModuleDependencyInfo>
  scan(PathRef FilePath, dependencies::DependencyScanningService &Service,
       const ProjectModules::CommandMangler &Mangler);

  std::shared_ptr<const clang::tooling::CompilationDatabase> CDB;
  const ThreadsafeFS &TFS;

  // Whether the scanner has scanned the project globally.
  bool GlobalScanned = false;

  // TODO: Add a scanning cache.

  // Map module name to source file path.
  llvm::StringMap<std::string> ModuleNameToSource;
};

std::optional<ModuleDependencyScanner::ModuleDependencyInfo>
ModuleDependencyScanner::scan(PathRef FilePath,
                              const ProjectModules::CommandMangler &Mangler) {
  std::unique_ptr<dependencies::DependencyScanningService> Service =
      scanningService();
  return scan(FilePath, *Service, Mangler);
}

std::unique_ptr<dependencies::DependencyScanningService>
ModuleDependencyScanner::scanningService() {
  dependencies::DependencyScanningServiceOptions Opts;
  Opts.MakeVFS = [this] { return TFS.view(std::nullopt); };
  Opts.Mode = dependencies::ScanningMode::CanonicalPreprocessing;
  Opts.EmitWarnings = false;
  Opts.ReportAbsolutePaths = false;
  return std::make_unique<dependencies::DependencyScanningService>(
      std::move(Opts));
}

std::optional<ModuleDependencyScanner::ModuleDependencyInfo>
ModuleDependencyScanner::scan(PathRef FilePath,
                              dependencies::DependencyScanningService &Service,
                              const ProjectModules::CommandMangler &Mangler) {
  auto Cmd = getCompileCommandForFile(*CDB, FilePath, Mangler);
  if (!Cmd)
    return std::nullopt;

  using namespace clang::tooling;

  DependencyScanningTool ScanningTool(Service);

  std::string S;
  llvm::raw_string_ostream OS(S);
  DiagnosticOptions DiagOpts;
  DiagOpts.ShowCarets = false;
  TextDiagnosticPrinter DiagConsumer(OS, DiagOpts);

  std::optional<P1689Rule> ScanningResult =
      ScanningTool.getP1689ModuleDependencyFile(*Cmd, Cmd->Directory,
                                                DiagConsumer);

  if (!ScanningResult) {
    elog("Scanning modules dependencies for {0} failed: {1}", FilePath, S);
    std::string Cmdline;
    for (auto &Arg : Cmd->CommandLine)
      Cmdline += Arg + " ";
    elog("The command line the scanning tool use is: {0}", Cmdline);
    return std::nullopt;
  }

  ModuleDependencyInfo Result;

  if (ScanningResult->Provides) {
    Result.ModuleName = ScanningResult->Provides->ModuleName;

    auto [Iter, Inserted] = ModuleNameToSource.try_emplace(
        ScanningResult->Provides->ModuleName, FilePath);

    if (!Inserted &&
        !pathEqual(normalizePath(Iter->second), normalizePath(FilePath))) {
      elog("Detected multiple source files ({0}, {1}) declaring the same "
           "module: '{2}'. "
           "Now clangd may find the wrong source in such case.",
           Iter->second, FilePath, ScanningResult->Provides->ModuleName);
    }
  }

  for (auto &Required : ScanningResult->Requires)
    Result.RequiredModules.push_back(Required.ModuleName);

  return Result;
}

bool ModuleDependencyScanner::globalScan(
    const ProjectModules::CommandMangler &Mangler) {
  if (GlobalScanned)
    return false;

  std::unique_ptr<dependencies::DependencyScanningService> Service =
      scanningService();
  for (auto &File : CDB->getAllFiles())
    scan(File, *Service, Mangler);

  GlobalScanned = true;
  return true;
}

PathRef ModuleDependencyScanner::lookupSourceForModuleName(
    llvm::StringRef ModuleName) const {
  assert(
      GlobalScanned &&
      "We should only call getSourceForModuleName after calling globalScan()");

  if (auto It = ModuleNameToSource.find(ModuleName);
      It != ModuleNameToSource.end())
    return It->second;

  return {};
}

std::vector<std::string> ModuleDependencyScanner::getRequiredModules(
    PathRef File, const ProjectModules::CommandMangler &Mangler) {
  auto ScanningResult = scan(File, Mangler);
  if (!ScanningResult)
    return {};

  return ScanningResult->RequiredModules;
}

std::optional<std::string> ModuleDependencyScanner::getModuleNameForSource(
    PathRef File, const ProjectModules::CommandMangler &Mangler) {
  auto ScanningResult = scan(File, Mangler);
  if (!ScanningResult || !ScanningResult->ModuleName)
    return std::nullopt;

  return ScanningResult->ModuleName;
}

std::optional<std::string> ModuleDependencyScanner::getSourceForModuleName(
    llvm::StringRef ModuleName, const ProjectModules::CommandMangler &Mangler) {
  bool Scanned = globalScan(Mangler);
  PathRef Source = lookupSourceForModuleName(ModuleName);
  if (!Source.empty()) {
    auto ScanningResult = scan(Source, Mangler);
    if (ScanningResult && ScanningResult->ModuleName == ModuleName)
      return Source.str();
  }

  if (Scanned)
    return std::nullopt;

  invalidateGlobalScan();
  globalScan(Mangler);
  PathRef Result = lookupSourceForModuleName(ModuleName);
  if (Result.empty())
    return std::nullopt;
  return Result.str();
}
} // namespace

/// Reads project module information directly from compile commands.
///
/// The key observation is that compile commands may already encode the mapping
/// between a TU, the module names it imports, and the BMI paths it uses:
/// - producers may spell the BMI path with `--precompile -o <bmi>` or
///   `-fmodule-output=<bmi>`
/// - consumers may spell the mapping from module name to BMI path with
///   `-fmodule-file=<module>=<bmi>`
///
/// When that information is present, we can answer
/// `getSourceForModuleName(ModuleName, RequiredSourceFile)` by first looking up
/// the BMI path the consumer TU uses for `ModuleName`, and then mapping that
/// BMI path back to the module unit source that produced it. This avoids the
/// older scanning-only approach of guessing the module unit from the module
/// name alone.
///
/// Producer commands alone do not reliably tell us the module name associated
/// with a BMI path. The consumer's `-fmodule-file=` entry provides that mapping
/// for each query, and the producer index maps the BMI path back to its source.
///
/// Note that compilation database can be stale, so results from this helper
/// should be treated as preferred hints rather than unquestionable truth.
/// `ProjectModules` validates or falls back when needed.
class CompileCommandsProjectModules {
public:
  CompileCommandsProjectModules(
      std::shared_ptr<const clang::tooling::CompilationDatabase> CDB,
      const ThreadsafeFS &TFS)
      : CDB(std::move(CDB)), TFS(TFS) {}

  std::optional<std::string>
  getSourceForModuleName(llvm::StringRef ModuleName,
                         PathRef RequiredSourceFile) {
    auto Parsed = parseFileCommand(RequiredSourceFile);
    if (!Parsed)
      return std::nullopt;

    auto It = Parsed->RequiredModuleFiles.find(ModuleName);
    if (It == Parsed->RequiredModuleFiles.end())
      return std::nullopt;

    indexProducerCommands();
    auto SourceIt = PCMToSource.find(maybeCaseFoldPath(It->second));
    if (SourceIt == PCMToSource.end())
      return std::nullopt;

    return SourceIt->second;
  }

  void setCommandMangler(ProjectModules::CommandMangler Mangler) {
    this->Mangler = std::move(Mangler);
    ProducerCommandsIndexed = false;
    PCMToSource.clear();
  }

private:
  /// Builds the BMI path to producer source index from compile commands.
  void indexProducerCommands() {
    if (ProducerCommandsIndexed)
      return;

    for (const auto &File : CDB->getAllFiles()) {
      auto Parsed = parseFileCommand(File);
      if (!Parsed)
        continue;

      if (Parsed->OutputModuleFile)
        PCMToSource[maybeCaseFoldPath(*Parsed->OutputModuleFile)] =
            Parsed->SourceFile;
    }

    ProducerCommandsIndexed = true;
  }

  /// Parses the compile command for \p File into the module information
  /// encoded in the command line.
  std::optional<ParsedCompileCommandInfo> parseFileCommand(PathRef File) const {
    auto Cmd = getCompileCommandForFile(*CDB, File, Mangler);
    if (!Cmd)
      return std::nullopt;
    return parseCompileCommandInfo(std::move(*Cmd), TFS);
  }

  std::shared_ptr<const clang::tooling::CompilationDatabase> CDB;
  const ThreadsafeFS &TFS;
  ProjectModules::CommandMangler Mangler;
  bool ProducerCommandsIndexed = false;

  llvm::StringMap<std::string> PCMToSource;
};

class ProjectModules::Impl {
public:
  Impl(std::shared_ptr<const clang::tooling::CompilationDatabase> CDB,
       const ThreadsafeFS &TFS)
      : CompileCommands(CDB, TFS), Scanner(std::move(CDB), TFS) {}

  std::vector<std::string> getRequiredModules(PathRef File) {
    // Return scanning results directly as it is fast enough and up to date.
    return Scanner.getRequiredModules(File, Mangler);
  }

  std::optional<std::string> getModuleNameForSource(PathRef File) {
    // Return scanning results directly as it is fast enough and up to date.
    return Scanner.getModuleNameForSource(File, Mangler);
  }

  std::optional<std::string> getSourceForModuleName(llvm::StringRef ModuleName,
                                                    PathRef RequiredSource) {
    return findSourceForModuleName(ModuleName, RequiredSource);
  }

  void setCommandMangler(ProjectModules::CommandMangler Mangler) {
    this->Mangler = std::move(Mangler);
    CompileCommands.setCommandMangler(
        [this](tooling::CompileCommand &Command, PathRef CommandPath) {
          if (this->Mangler)
            this->Mangler(Command, CommandPath);
        });
    Scanner.invalidate();
  }

private:
  std::optional<std::string> findSourceForModuleName(llvm::StringRef ModuleName,
                                                     PathRef RequiredSource) {
    auto FromCompileCommands =
        CompileCommands.getSourceForModuleName(ModuleName, RequiredSource);
    // Check if the source still declares the module.
    // This is to validate compile-command-derived results may be stale and
    // scanning a single file is fast enough.
    if (FromCompileCommands) {
      auto ScannedModule =
          Scanner.getModuleNameForSource(*FromCompileCommands, Mangler);
      if (ScannedModule && *ScannedModule == ModuleName)
        return FromCompileCommands;
    }

    return Scanner.getSourceForModuleName(ModuleName, Mangler);
  }

  CompileCommandsProjectModules CompileCommands;
  ModuleDependencyScanner Scanner;
  ProjectModules::CommandMangler Mangler;
};

ProjectModules::ProjectModules(
    std::shared_ptr<const clang::tooling::CompilationDatabase> CDB,
    const ThreadsafeFS &TFS)
    : PImpl(std::make_unique<Impl>(std::move(CDB), TFS)) {}

ProjectModules::~ProjectModules() = default;

std::vector<std::string> ProjectModules::getRequiredModules(PathRef File) {
  return PImpl->getRequiredModules(File);
}

std::optional<std::string>
ProjectModules::getModuleNameForSource(PathRef File) {
  return PImpl->getModuleNameForSource(File);
}

std::optional<std::string>
ProjectModules::getSourceForModuleName(llvm::StringRef ModuleName,
                                       PathRef RequiredSource) {
  return PImpl->getSourceForModuleName(ModuleName, RequiredSource);
}

void ProjectModules::setCommandMangler(CommandMangler Mangler) {
  PImpl->setCommandMangler(std::move(Mangler));
}

} // namespace clang::clangd
