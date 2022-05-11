// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "llvm_util/llvm2alive.h"
#include "ir/memory.h"
#include "smt/expr.h"
#include "smt/smt.h"
#include "smt/solver.h"
#include "tools/transform.h"
#include "util/config.h"
#include "util/version.h"
#include "util/symexec.h"
#include "util/interp.h"
#include "util/concreteval.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/StringRef.h"

#include <fstream>
#include <iostream>
#include <utility>

#include <jsoncons/json.hpp>
#include <jsoncons/byte_string.hpp>
#include <jsoncons/json.hpp>

using namespace llvm_util;
using namespace smt;
using namespace tools;
using namespace util;
using namespace std;
using jsoncons::json_object_arg;
using jsoncons::ojson;

#define LLVM_ARGS_PREFIX ""
#include "llvm_util/cmd_args_list.h"

namespace {

llvm::cl::opt<string> opt_file(llvm::cl::Positional,
  llvm::cl::desc("bitcode_file"), llvm::cl::Required,
  llvm::cl::value_desc("filename"), llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<string> opt_file2(llvm::cl::Positional,
  llvm::cl::desc("[second_bitcode_file]"),
  llvm::cl::Optional, llvm::cl::value_desc("filename"),
  llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<std::string> opt_src_fn(LLVM_ARGS_PREFIX "src-fn",
  llvm::cl::desc("Name of src function (without @)"),
  llvm::cl::cat(alive_cmdargs), llvm::cl::init("src"));

llvm::cl::opt<std::string> opt_tgt_fn(LLVM_ARGS_PREFIX"tgt-fn",
  llvm::cl::desc("Name of tgt function (without @)"),
  llvm::cl::cat(alive_cmdargs), llvm::cl::init("tgt"));

llvm::cl::opt<bool> opt_only_interp(
    LLVM_ARGS_PREFIX "only-interp",
    llvm::cl::desc("Only interpret functions (default=false)"),
    llvm::cl::init(false), llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<unsigned> opt_random_input(
    LLVM_ARGS_PREFIX "random-input",
    llvm::cl::desc(
        "use random inputs for interpreter (default=0 uses fixed input)"),
    llvm::cl::init(0), llvm::cl::value_desc("count"),
    llvm::cl::cat(alive_cmdargs));

llvm::ExitOnError ExitOnErr;

// adapted from llvm-dis.cpp
std::unique_ptr<llvm::Module> openInputFile(llvm::LLVMContext &Context,
                                            const string &InputFilename) {
  auto MB =
    ExitOnErr(errorOrToExpected(llvm::MemoryBuffer::getFile(InputFilename)));
  llvm::SMDiagnostic Diag;
  auto M = getLazyIRModule(move(MB), Diag, Context,
                           /*ShouldLazyLoadMetadata=*/true);
  if (!M) {
    Diag.print("", llvm::errs(), false);
    return 0;
  }
  ExitOnErr(M->materializeAll());
  return M;
}

inline bool isDefined(ojson &res) {
  static const ojson POISON = "poison";
  static const ojson UNDEF = "undef";
  return res != POISON && res != UNDEF;
}

bool compareResults(ojson &res_src, ojson &res_tgt, ojson &res) {
  static const ojson POISON = "poison";
  static const ojson UNDEF = "undef";

  if (res_src.is_array() && res_tgt.is_array()) {
    if (res_src.size() != res_tgt.size()) {
      res["status"] =
          "Not a refinement. different aggregate return value sizes";
      res["valid"] = false;
      return false;
    }
    for (size_t i = 0; i < res_src.size(); i++) {
      if (!compareResults(res_src[i], res_tgt[i], res)) {
        // res["status"] = "Not a refinement. different return values on
        // aggregate type";
        res["valid"] = false;
        return false;
      }
    }
    res["status"] =
        "Refinement check for this input seems to hold. equal results";
    res["valid"] = true;
    return true;
  } else {
    if (res_src == res_tgt) { // defined == defined
      res["status"] =
          "Refinement check for this input seems to hold. equal results";
      res["valid"] = true;
      return true;
    } else {
      if (isDefined(res_src) &&
          isDefined(res_tgt)) { // defined != defined result
        res["status"] = "Not a refinement. different defined results";
        res["valid"] = false;
        return false;
      } else if (!isDefined(res_src) && isDefined(res_tgt)) {
        res["status"] = "Refinement check for this input seems to hold. target "
                        "more defined than source";
        res["valid"] = true;
        return true;
      } else if (res_tgt == UNDEF) {
        if (isDefined(res_src)) {
          res["status"] = "Refinement check for this input seems to hold. "
                          "target less defined than source";
          res["valid"] = false;
          return false;
        }
        res["status"] = "Refinement check for this input seems to hold.";
        res["valid"] = true;
        return true;
      } else if (res_tgt == POISON) {
        if (res_src == POISON) {
          res["status"] = "Refinement check for this input seems to hold. "
                          "source and target returned poison";
          res["valid"] = true;
          return true;
        }
        res["status"] = "Refinement check for this input seems to hold. target "
                        "less defined than source";
        res["valid"] = false;
        return false;
      }
    }
  }
  UNREACHABLE();
}

void checkRefinement(ojson &res_src, ojson &res_tgt, ojson &res) {

  if (res_src.is_null() || res_tgt.is_null()) {
    res["error"] = "Could not interpret function";
    res["valid"] = false;
    return;
  }

  if (res_src["status"] == res_tgt["status"] && res_src["status"] == "done") {
    // check for result refinement
    if (res_src["undefined"] == true) {
      res["status"] = "Refinement check for this input seems to hold. "
                      "Undefined output on source ";
      res["valid"] = true;
      return;
    } else if (res_tgt["undefined"] == true) {
      res["status"] = "Refinement check for this input seems to hold. Defined "
                      "source output but Undefined output on target";
      res["valid"] = false;
      return;
    }
    compareResults(res_src["return_value"], res_tgt["return_value"], res);
    return;
  } else if (res_src["status"] == res_tgt["status"]) {
    res["error"] = "Failed to check for refinement. interpreter status";
    res["valid"] = false;
    return;
  } else { // res_src["status"] != res_tgt["status"]
    res["error"] =
        "Failed to check for refinement. Different interpreter return statuses";
    res["valid"] = false;
    return;
  }
  UNREACHABLE();
}

ojson interpFunction(llvm::Function &F, llvm::TargetLibraryInfoWrapperPass &TLI,
                     unsigned &successCount, unsigned &errorCount) {
  auto Func =
      llvm2alive(F, TLI.getTLI(F)); // FIXME change to avoid calling twice
  if (!Func) {
    ojson result(json_object_arg);
    cerr << "ERROR: Could not translate '" << F.getName().str()
         << "' to Alive IR\n";
    ++errorCount;
    std::ostringstream sstr;
    sstr << "Could not translate '" << F.getName().str() << "' to Alive IR\n";
    result["error"] = sstr.str();
    return result;
  }
  if (opt_print_dot) {
    Func->writeDot("");
  }
  if (!opt_quiet)
    Func->print(cout << "\n----------------------------------------\n");
  auto result = interp(*Func);

  cout << "interp result = " << result << '\n';
  successCount++;
  return result;
}

ojson interpFunctionPair(llvm::Function &F1, llvm::Function &F2,
                         llvm::TargetLibraryInfoWrapperPass &TLI,
                         unsigned &successCount, unsigned &errorCount) {
  auto Func1 =
      llvm2alive(F1, TLI.getTLI(F1)); // FIXME change to avoid calling twice
  auto Func2 =
      llvm2alive(F1, TLI.getTLI(F1)); // FIXME change to avoid calling twice
  ojson result(json_object_arg);

  if (!Func1) {
    cerr << "ERROR: Could not translate '" << F1.getName().str()
         << "' to Alive IR\n";
    ++errorCount;
    std::ostringstream sstr;
    sstr << "Could not translate '" << F1.getName().str() << "' to Alive IR\n";
    result["error"] = sstr.str();
  }

  if (!Func2) {
    cerr << "ERROR: Could not translate '" << F2.getName().str()
         << "' to Alive IR\n";
    ++errorCount;
    std::ostringstream sstr;
    sstr << "Could not translate '" << F2.getName().str() << "' to Alive IR\n";
    result["error"] = sstr.str();
  }

  if (!Func1 || !Func2) {
    return result;
  }

  if (opt_print_dot) {
    Func1->writeDot("");
    Func2->writeDot(""); // This is pretty hacky but probably need to remove the
                         // option anyway
  }
  if (!opt_quiet) {
    Func1->print(cout << "\n----------------------------------------\n");
    Func2->print(cout << "\n----------------------------------------\n");
  }

  std::vector<std::shared_ptr<ConcreteVal>> input_vals;
  ojson res_1, res_2;
  bool is_valid;
  for (unsigned i = 0; i < opt_random_input; ++i) {
    res_1 = interp_save_load(*Func1, input_vals, true);
    successCount++;
    cout << "interp src result = " << res_1 << '\n';
    res_2 = interp_save_load(*Func2, input_vals, false);
    successCount++;
    cout << "interp target result = " << res_2 << '\n';
    checkRefinement(res_1, res_2, result);
    is_valid = result.get_with_default<bool>("valid", true);
    if (result.contains("error") || !is_valid) {
      cout << "result = " << result << '\n';
      return result;
    }
    input_vals.clear();
  }

  return result;
}

ojson verify(llvm::Function &F1, llvm::Function &F2,
             llvm::TargetLibraryInfoWrapperPass &TLI, unsigned &successCount,
             unsigned &errorCount) {
  auto fn1 = llvm2alive(F1, TLI.getTLI(F1));
  auto fn2 = llvm2alive(F2, TLI.getTLI(F2));
  ojson result(json_object_arg);

  if (!fn1 || !fn2) {
    result["status"] = "could_not_translate";
    result["valid"] = ojson(nullptr); // unknown
    return result;
  }

  stringstream ss1, ss2;
  fn1->print(ss1);
  fn2->print(ss2);

  if (ss1.str() == ss2.str()) {
    result["status"] = "syntactically equal src and tgt";
    result["equal"] = true;
    result["function"] = ss1.str();
    result["valid"] = true;
    return result;
  }

  ojson res_1, res_2;
  if (opt_random_input > 0) {
    result = interpFunctionPair(F1, F2, TLI, successCount, errorCount);
  } else {
    res_1 = interpFunction(F1, TLI, successCount, errorCount);
    res_2 = interpFunction(F2, TLI, successCount, errorCount);
    checkRefinement(res_1, res_2, result);
  }

  return result;
}

unsigned num_correct = 0;
unsigned num_errors = 0;

bool compareFunctions(llvm::Function &F1, llvm::Function &F2,
                      llvm::TargetLibraryInfoWrapperPass &TLI,
                      unsigned &successCount, unsigned &errorCount) {

  auto res = verify(F1, F2, TLI, successCount, errorCount);
  assert(res.contains("valid") && "missing valid key from result object");
  if (res["valid"] == true) {
    if (res.contains("equal")) {
      *out << res["function"].as_string_view() << endl;
    }
    if (res.contains("status")) {
      *out << "STATUS: " << res["status"].as_string_view() << "\n";
      *out << "\n----------------------------------------\n";
    }
    num_correct++;
    return true;
  } else {
    if (res.contains("error")) {
      *out << "ERROR: " << res["error"].as_string_view() << "\n";
      if (res.contains("status")) {
        *out << "STATUS: " << res["status"].as_string_view() << "\n";
      }
      ++num_errors;
      *out << "\n----------------------------------------\n";
      return true;
    }
    *out << "\n----------------------------------------\n";
    return false;
  }
  UNREACHABLE();
}

void optimizeModule(llvm::Module *M) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::FunctionPassManager FPM = PB.buildFunctionSimplificationPipeline(
#if LLVM_VERSION_MAJOR >= 14
      llvm::OptimizationLevel::O2,
#else
      llvm::PassBuilder::OptimizationLevel::O2,
#endif
      llvm::ThinOrFullLTOPhase::None);
  llvm::ModulePassManager MPM;
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
  MPM.run(*M, MAM);
}

llvm::Function *findFunction(llvm::Module &M, const string &FName) {
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (FName.compare(F.getName()) != 0)
      continue;
    return &F;
  }
  return 0;
}
}

int main(int argc, char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm::EnableDebugBuffering = true;
  llvm::llvm_shutdown_obj llvm_shutdown; // Call llvm_shutdown() on exit.
  llvm::LLVMContext Context;

  
  cout << "Alive-interpreter\n";
  
  std::string Usage =
      R"EOF(Alive2 stand-alone translation validator:
version )EOF";
  Usage += alive_version;
  Usage += R"EOF(
see alive-interp --version  for LLVM version info,

This program takes an LLVM IR files files as a command-line
argument. Both .bc and .ll files are supported.

If one or more functions are specified (as a comma-separated list)
using the --funcs command line option, alive-exec will attempt to
execute them using Alive2 as an interpreter. There are currently many
restrictions on what kind of functions can be executed: they cannot
take inputs, cannot use memory, cannot depend on undefined behaviors,
and cannot include loops that execute too many iterations.

If no functions are specified on the command line, then alive-exec
will attempt to execute every function in the bitcode file.
)EOF";

  llvm::cl::HideUnrelatedOptions(alive_cmdargs);
  llvm::cl::ParseCommandLineOptions(argc, argv, Usage);

  auto M1 = openInputFile(Context, opt_file);
  if (!M1.get()) {
    cerr << "Could not read bitcode from '" << opt_file << "'\n";
    return -1;
  }

#define ARGS_MODULE_VAR M1
# include "llvm_util/cmd_args_def.h"

  auto &DL = M1.get()->getDataLayout();
  llvm::Triple targetTriple(M1.get()->getTargetTriple());
  llvm::TargetLibraryInfoWrapperPass TLI(targetTriple);

  llvm_util::initializer llvm_util_init(cerr, DL);
  
  unsigned successCount = 0, errorCount = 0;

  unique_ptr<llvm::Module> M2;
  if (opt_only_interp) {
    for (auto &F : *M1.get()) {
      if (F.isDeclaration())
        continue;
      if (!func_names.empty() && !func_names.count(F.getName().str()))
        continue;
      interpFunction(F, TLI, successCount, errorCount);
    }
    goto end;
  } else {
    if (opt_file2.empty()) {
      auto SRC = findFunction(*M1, opt_src_fn);
      auto TGT = findFunction(*M1, opt_tgt_fn);
      if (SRC && TGT) {
        compareFunctions(*SRC, *TGT, TLI, successCount, errorCount);
        goto end;
      } else {
        M2 = CloneModule(*M1);
        optimizeModule(M2.get());
      }
    } else { // a seocond file exits
      M2 = openInputFile(Context, opt_file2);
      if (!M2.get()) {
        *out << "Could not read bitcode from '" << opt_file2 << "'\n";
        return -1;
      }
    }
  }

  if (M1.get()->getTargetTriple() != M2.get()->getTargetTriple()) {
      *out << "Modules have different target triples\n";
      return -1;
  }
  
  for (auto &F1 : *M1.get()) {
    if (F1.isDeclaration())
      continue;
    if (!func_names.empty() && !func_names.count(F1.getName().str()))
      continue;
    for (auto &F2 : *M2.get()) {
      if (F2.isDeclaration() || F1.getName() != F2.getName())
        continue;
      if (!compareFunctions(F1, F2, TLI, successCount, errorCount))
        if (opt_error_fatal)
          goto end;
      break;
    }
  }


  // if (!opt_file2.empty()) {
  //   for (auto &F : *M2.get()) {
  //     if (F.isDeclaration())
  //       continue;
  //     if (!func_names.empty() && !func_names.count(F.getName().str()))
  //       continue;
  //     interpFunction(F, TLI, successCount, errorCount);
  //   }
  // }
end: 
  cout << "Summary:\n"
          "  " << successCount << " functions interpreted successfully\n"
          "  " << errorCount << " functions not interpreterd\n"
          "  " << num_correct << " correct transformations\n"
          "  " << num_errors << " Alive2 errors\n";

  return errorCount > 0;
}
