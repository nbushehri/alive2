// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "llvm/MC/MCAsmInfo.h" // include first to avoid ambiguity for comparison operator from util/spaceship.h
#include "ir/instr.h"
#include "ir/type.h"
#include "llvm_util/llvm2alive.h"
#include "llvm_util/llvm_optimizer.h"
#include "llvm_util/utils.h"
#include "smt/smt.h"
#include "tools/transform.h"
#include "util/sort.h"
#include "util/version.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/MCTargetOptionsCommandFlags.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"

#define GET_INSTRINFO_ENUM
#include "Target/AArch64/AArch64GenInstrInfo.inc"

#define GET_REGINFO_ENUM
#include "Target/AArch64/AArch64GenRegisterInfo.inc"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace tools;
using namespace util;
using namespace std;
using namespace llvm_util;
using namespace llvm;

#define LLVM_ARGS_PREFIX ""
#define ARGS_SRC_TGT
#define ARGS_REFINEMENT
#include "llvm_util/cmd_args_list.h"

namespace {

llvm::cl::opt<string> opt_file1(llvm::cl::Positional,
                                llvm::cl::desc("first_bitcode_file"),
                                llvm::cl::Required,
                                llvm::cl::value_desc("filename"),
                                llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<string> opt_file2(llvm::cl::Positional,
                                llvm::cl::desc("[second_bitcode_file]"),
                                llvm::cl::Optional,
                                llvm::cl::value_desc("filename"),
                                llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<std::string>
    opt_src_fn(LLVM_ARGS_PREFIX "src-fn",
               llvm::cl::desc("Name of src function (without @)"),
               llvm::cl::cat(alive_cmdargs), llvm::cl::init("src"));

llvm::cl::opt<std::string>
    opt_tgt_fn(LLVM_ARGS_PREFIX "tgt-fn",
               llvm::cl::desc("Name of tgt function (without @)"),
               llvm::cl::cat(alive_cmdargs), llvm::cl::init("tgt"));

llvm::cl::opt<string> optPass(
    LLVM_ARGS_PREFIX "passes", llvm::cl::value_desc("optimization passes"),
    llvm::cl::desc("Specify which LLVM passes to run (default=O2). "
                   "The syntax is described at "
                   "https://llvm.org/docs/NewPassManager.html#invoking-opt"),
    llvm::cl::cat(alive_cmdargs), llvm::cl::init("O2"));

llvm::cl::opt<bool> opt_backend_tv(
    LLVM_ARGS_PREFIX "backend-tv",
    llvm::cl::desc("Verify operation of a backend (default=false)"),
    llvm::cl::init(false), llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<bool> opt_asm_only(
    "asm-only",
    llvm::cl::desc("Only generate assembly and exit (default=false)"),
    llvm::cl::init(false), llvm::cl::cat(alive_cmdargs));

llvm::cl::opt<bool> asm_input(
    "asm-input",
    llvm::cl::desc("use 2nd positional argument as asm input (default=false)"),
    llvm::cl::init(false), llvm::cl::cat(alive_cmdargs));

llvm::ExitOnError ExitOnErr;

// adapted from llvm-dis.cpp
std::unique_ptr<llvm::Module> openInputFile(llvm::LLVMContext &Context,
                                            const string &InputFilename) {
  auto MB =
      ExitOnErr(errorOrToExpected(llvm::MemoryBuffer::getFile(InputFilename)));
  llvm::SMDiagnostic Diag;
  auto M = getLazyIRModule(std::move(MB), Diag, Context,
                           /*ShouldLazyLoadMetadata=*/true);
  if (!M) {
    Diag.print("", llvm::errs(), false);
    return 0;
  }
  ExitOnErr(M->materializeAll());
  return M;
}

optional<smt::smt_initializer> smt_init;

struct Results {
  Transform t;
  string error;
  Errors errs;
  enum {
    ERROR,
    TYPE_CHECKER_FAILED,
    SYNTACTIC_EQ,
    CORRECT,
    UNSOUND,
    FAILED_TO_PROVE
  } status;

  static Results Error(string &&err) {
    Results r;
    r.status = ERROR;
    r.error = std::move(err);
    return r;
  }
};

set<int> s_flag = {
    // ADDSW
    AArch64::ADDSWri,
    AArch64::ADDSWrs,
    AArch64::ADDSWrx,
    // ADDSX
    AArch64::ADDSXri,
    AArch64::ADDSXrs,
    AArch64::ADDSXrx,
    // SUBSW
    AArch64::SUBSWri,
    AArch64::SUBSWrs,
    AArch64::SUBSWrx,
    // SUBSX
    AArch64::SUBSXri,
    AArch64::SUBSXrs,
    AArch64::SUBSXrx,
    // ANDSW
    AArch64::ANDSWri,
    AArch64::ANDSWrr,
    AArch64::ANDSWrs,
    // ANDSX
    AArch64::ANDSXri,
    AArch64::ANDSXrr,
    AArch64::ANDSXrs,
};

set<int> instrs_32 = {
    AArch64::ADDWrx,  AArch64::ADDSWrs,  AArch64::ADDSWri,  AArch64::ADDWrs,
    AArch64::ADDWri,  AArch64::ADDSWrx,  AArch64::ASRVWr,   AArch64::SUBWri,
    AArch64::SUBWrs,  AArch64::SUBWrx,   AArch64::SUBSWrs,  AArch64::SUBSWri,
    AArch64::SUBSWrx, AArch64::SBFMWri,  AArch64::CSELWr,   AArch64::ANDWri,
    AArch64::ANDWrr,  AArch64::ANDWrs,   AArch64::ANDSWri,  AArch64::ANDSWrr,
    AArch64::ANDSWrs, AArch64::MADDWrrr, AArch64::MSUBWrrr, AArch64::EORWri,
    AArch64::CSINVWr, AArch64::CSINCWr,  AArch64::MOVZWi,   AArch64::MOVNWi,
    AArch64::MOVKWi,  AArch64::LSLVWr,   AArch64::LSRVWr,   AArch64::ORNWrs,
    AArch64::UBFMWri, AArch64::BFMWri,   AArch64::ORRWrs,   AArch64::ORRWri,
    AArch64::SDIVWr,  AArch64::UDIVWr,   AArch64::EXTRWrri, AArch64::EORWrs,
    AArch64::RORVWr,  AArch64::RBITWr,   AArch64::CLZWr,    AArch64::REVWr,
    AArch64::CSNEGWr, AArch64::BICWrs,   AArch64::EONWrs,   AArch64::REV16Wr,
    AArch64::Bcc,     AArch64::CCMPWr,   AArch64::CCMPWi};

set<int> instrs_64 = {
    AArch64::ADDXrx,    AArch64::ADDSXrs,   AArch64::ADDSXri,
    AArch64::ADDXrs,    AArch64::ADDXri,    AArch64::ADDSXrx,
    AArch64::ASRVXr,    AArch64::SUBXri,    AArch64::SUBXrs,
    AArch64::SUBXrx,    AArch64::SUBSXrs,   AArch64::SUBSXri,
    AArch64::SUBSXrx,   AArch64::SBFMXri,   AArch64::CSELXr,
    AArch64::ANDXri,    AArch64::ANDXrr,    AArch64::ANDXrs,
    AArch64::ANDSXri,   AArch64::ANDSXrr,   AArch64::ANDSXrs,
    AArch64::MADDXrrr,  AArch64::MSUBXrrr,  AArch64::EORXri,
    AArch64::CSINVXr,   AArch64::CSINCXr,   AArch64::MOVZXi,
    AArch64::MOVNXi,    AArch64::MOVKXi,    AArch64::LSLVXr,
    AArch64::LSRVXr,    AArch64::ORNXrs,    AArch64::UBFMXri,
    AArch64::BFMXri,    AArch64::ORRXrs,    AArch64::ORRXri,
    AArch64::SDIVXr,    AArch64::UDIVXr,    AArch64::EXTRXrri,
    AArch64::EORXrs,    AArch64::SMADDLrrr, AArch64::UMADDLrrr,
    AArch64::RORVXr,    AArch64::RBITXr,    AArch64::CLZXr,
    AArch64::REVXr,     AArch64::CSNEGXr,   AArch64::BICXrs,
    AArch64::EONXrs,    AArch64::SMULHrr,   AArch64::UMULHrr,
    AArch64::REV32Xr,   AArch64::REV16Xr,   AArch64::SMSUBLrrr,
    AArch64::UMSUBLrrr, AArch64::PHI,       AArch64::TBZW,
    AArch64::TBZX,      AArch64::TBNZW,     AArch64::TBNZX,
    AArch64::B,         AArch64::CBZW,      AArch64::CBZX,
    AArch64::CBNZW,     AArch64::CBNZX,     AArch64::CCMPXr,
    AArch64::CCMPXi};

set<int> instrs_no_write = {AArch64::Bcc,    AArch64::B,      AArch64::TBZW,
                            AArch64::TBZX,   AArch64::TBNZW,  AArch64::TBNZX,
                            AArch64::CBZW,   AArch64::CBZX,   AArch64::CBNZW,
                            AArch64::CBNZX,  AArch64::CCMPWr, AArch64::CCMPWi,
                            AArch64::CCMPXr, AArch64::CCMPXi};

bool has_s(int instr) {
  return s_flag.contains(instr);
}

Results verify(llvm::Function &F1, llvm::Function &F2,
               llvm::TargetLibraryInfoWrapperPass &TLI,
               bool print_transform = false, bool always_verify = false) {
  auto fn1 = llvm2alive(F1, TLI.getTLI(F1));
  if (!fn1)
    return Results::Error("Could not translate '" + F1.getName().str() +
                          "' to Alive IR\n");

  auto fn2 = llvm2alive(F2, TLI.getTLI(F2), fn1->getGlobalVarNames());
  if (!fn2)
    return Results::Error("Could not translate '" + F2.getName().str() +
                          "' to Alive IR\n");

  Results r;
  r.t.src = std::move(*fn1);
  r.t.tgt = std::move(*fn2);

  if (!always_verify) {
    stringstream ss1, ss2;
    r.t.src.print(ss1);
    r.t.tgt.print(ss2);
    if (ss1.str() == ss2.str()) {
      if (print_transform)
        r.t.print(*out, {});
      r.status = Results::SYNTACTIC_EQ;
      return r;
    }
  }

  smt_init->reset();
  r.t.preprocess();
  TransformVerify verifier(r.t, false);

  if (print_transform)
    r.t.print(*out, {});

  {
    auto types = verifier.getTypings();
    if (!types) {
      r.status = Results::TYPE_CHECKER_FAILED;
      return r;
    }
    assert(types.hasSingleTyping());
  }

  r.errs = verifier.verify();
  if (r.errs) {
    r.status = r.errs.isUnsound() ? Results::UNSOUND : Results::FAILED_TO_PROVE;
  } else {
    r.status = Results::CORRECT;
  }
  return r;
}

unsigned num_correct = 0;
unsigned num_unsound = 0;
unsigned num_failed = 0;
unsigned num_errors = 0;

struct MCOperandHash {

  enum Kind {
    reg = (1 << 2) - 1,
    immedidate = (1 << 3) - 1,
    symbol = (1 << 4) - 1
  };

  size_t operator()(const MCOperand &op) const {
    unsigned prefix;
    unsigned id;

    if (op.isReg()) {
      prefix = Kind::reg;
      id = op.getReg();
    } else if (op.isImm()) {
      prefix = Kind::immedidate;
      id = op.getImm();
    } else if (op.isExpr()) {
      prefix = Kind::symbol;
      auto expr = op.getExpr();
      if (expr->getKind() == MCExpr::ExprKind::SymbolRef) {
        const MCSymbolRefExpr &SRE = cast<MCSymbolRefExpr>(*expr);
        const MCSymbol &Sym = SRE.getSymbol();
        errs() << "label : " << Sym.getName() << '\n'; // FIXME remove when done
        id = Sym.getOffset();
      } else {
        assert("unsupported mcExpr" && false);
      }
    } else {
      assert("no" && false);
    }

    return std::hash<unsigned long>()(prefix * id);
  }
};

struct MCOperandEqual {
  enum Kind { reg = (1 << 2) - 1, immedidate = (1 << 3) - 1 };
  bool operator()(const MCOperand &lhs, const MCOperand &rhs) const {
    if ((lhs.isReg() && rhs.isReg() && (lhs.getReg() == rhs.getReg())) ||
        (lhs.isImm() && rhs.isImm() && (lhs.getImm() == rhs.getImm())) ||
        (lhs.isExpr() && rhs.isExpr() &&
         (lhs.getExpr() ==
          rhs.getExpr()))) { // FIXME this is just comparing ptrs
      return true;
    }
    return false;
  }
};

bool compareFunctions(llvm::Function &F1, llvm::Function &F2,
                      llvm::TargetLibraryInfoWrapperPass &TLI) {
  auto r = verify(F1, F2, TLI, !opt_quiet, opt_always_verify);
  if (r.status == Results::ERROR) {
    *out << "ERROR: " << r.error;
    ++num_errors;
    return true;
  }

  if (opt_print_dot) {
    r.t.src.writeDot("src");
    r.t.tgt.writeDot("tgt");
  }

  switch (r.status) {
  case Results::ERROR:
    UNREACHABLE();
    break;

  case Results::SYNTACTIC_EQ:
    *out << "Transformation seems to be correct! (syntactically equal)\n\n";
    ++num_correct;
    break;

  case Results::CORRECT:
    *out << "Transformation seems to be correct!\n\n";
    ++num_correct;
    break;

  case Results::TYPE_CHECKER_FAILED:
    *out << "Transformation doesn't verify!\n"
            "ERROR: program doesn't type check!\n\n";
    ++num_errors;
    return true;

  case Results::UNSOUND:
    *out << "Transformation doesn't verify!\n\n";
    if (!opt_quiet)
      *out << r.errs << endl;
    ++num_unsound;
    return false;

  case Results::FAILED_TO_PROVE:
    *out << r.errs << endl;
    ++num_failed;
    return true;
  }

  if (opt_bidirectional) {
    r = verify(F2, F1, TLI, false, opt_always_verify);
    switch (r.status) {
    case Results::ERROR:
    case Results::TYPE_CHECKER_FAILED:
      UNREACHABLE();
      break;

    case Results::SYNTACTIC_EQ:
    case Results::CORRECT:
      *out << "These functions seem to be equivalent!\n\n";
      return true;

    case Results::FAILED_TO_PROVE:
      *out << "Failed to verify the reverse transformation\n\n";
      if (!opt_quiet)
        *out << r.errs << endl;
      return true;

    case Results::UNSOUND:
      *out << "Reverse transformation doesn't verify!\n\n";
      if (!opt_quiet)
        *out << r.errs << endl;
      return false;
    }
  }
  return true;
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
} // namespace

static llvm::mc::RegisterMCTargetOptionsFlags MOF;

class MCInstWrapper {
private:
  llvm::MCInst instr;
  std::vector<unsigned> op_ids;
  std::map<unsigned, std::string>
      phi_blocks; // This is pretty wasteful but I'm not sure how to add
                  // MCExpr operands to the underlying MCInst phi instructions
public:
  MCInstWrapper(llvm::MCInst _instr) : instr(_instr) {
    op_ids.resize(instr.getNumOperands(), 0);
  }

  llvm::MCInst &getMCInst() {
    return instr;
  }

  // use to assign ids when adding the arugments to phi-nodes
  void pushOpId(unsigned id) {
    op_ids.push_back(id);
  }

  void setOpId(unsigned index, unsigned id) {
    assert(op_ids.size() > index && "Invalid index");
    op_ids[index] = id;
  }

  unsigned getOpId(unsigned index) {
    return op_ids[index];
  }

  void setOpPhiBlock(unsigned index, const std::string &block_name) {
    phi_blocks[index] = block_name;
  }

  const std::string &getOpPhiBlock(unsigned index) const {
    return phi_blocks.at(index);
  }

  unsigned getOpcode() const {
    return instr.getOpcode();
  }

  std::string findTargetLabel() {
    auto num_operands = instr.getNumOperands();
    for (unsigned i = 0; i < num_operands; ++i) {
      auto op = instr.getOperand(i);
      if (op.isExpr()) {
        auto expr = op.getExpr();
        if (expr->getKind() == MCExpr::ExprKind::SymbolRef) {
          const MCSymbolRefExpr &SRE = cast<MCSymbolRefExpr>(*expr);
          const MCSymbol &Sym = SRE.getSymbol();
          return Sym.getName().str();
        }
      }
    }

    assert(false && "Could not find target label in arm branch instruction");
    UNREACHABLE();
  }
  // FIXME: for phi instructions and figure out to use register names rather
  // than numbers
  void print() const {
    cout << "< MCInstWrapper " << getOpcode() << " ";
    unsigned idx = 0;
    for (auto it = instr.begin(); it != instr.end(); ++it) {
      if (it->isReg()) {
        if (getOpcode() == AArch64::PHI && idx >= 1) {
          cout << "<Phi arg>:[(" << it->getReg() << "," << op_ids[idx] << "),"
               << getOpPhiBlock(idx) << "]>";
        } else {
          cout << "<MCOperand Reg:(" << it->getReg() << ", " << op_ids[idx]
               << ")>";
        }
      } else if (it->isImm()) {
        cout << "<MCOperand Imm:" << it->getImm() << ">";
      } else if (it->isExpr()) {
        cout << "<MCOperand Expr:>"; // FIXME
      } else {
        assert("MCInstWrapper printing an unsupported operand" && false);
      }
      idx++;
    }
    cout << ">\n";
  }
};

// Represents a basic block of machine instructions
class MCBasicBlock {
private:
  std::string name;
  using SetTy = llvm::SetVector<MCBasicBlock *>;
  std::vector<MCInstWrapper> Instrs;
  SetTy Succs;
  SetTy Preds;

public:
  MCBasicBlock(std::string _name) : name(_name) {}
  // MCBasicBlock(const MCBasicBlock&) =delete;

  const std::string &getName() const {
    return name;
  }

  auto &getInstrs() {
    return Instrs;
  }

  auto size() const {
    return Instrs.size();
  }

  auto &getSuccs() {
    return Succs;
  }

  auto &getPreds() {
    return Preds;
  }

  void addInst(MCInstWrapper &inst) {
    Instrs.push_back(inst);
  }

  void addInstBegin(MCInstWrapper &&inst) {
    Instrs.insert(Instrs.begin(), std::move(inst));
  }

  void addSucc(MCBasicBlock *succ_block) {
    Succs.insert(succ_block);
  }

  void addPred(MCBasicBlock *pred_block) {
    Preds.insert(pred_block);
  }

  auto predBegin() {
    return Preds.begin();
  }

  auto predEnd() {
    return Preds.end();
  }

  auto succBegin() const {
    return Succs.begin();
  }

  auto succEnd() const {
    return Succs.end();
  }

  void print() const {
    for (auto &instr : Instrs) {
      instr.print();
    }
  }
};

// Represents a machine function
class MCFunction {
  std::string name;
  unsigned label_cnt{0};
  using BlockSetTy = llvm::SetVector<MCBasicBlock *>;
  std::unordered_map<MCBasicBlock *, BlockSetTy> dom;
  std::unordered_map<MCBasicBlock *, BlockSetTy> dom_frontier;
  std::unordered_map<MCBasicBlock *, BlockSetTy> dom_tree;

  std::unordered_map<MCOperand, BlockSetTy, MCOperandHash, MCOperandEqual> defs;
  std::unordered_map<
      MCBasicBlock *,
      std::unordered_set<MCOperand, MCOperandHash, MCOperandEqual>>
      phis; // map from block to variable names that need phi-nodes in those
            // blocks
  std::unordered_map<
      MCBasicBlock *,
      std::unordered_map<MCOperand,
                         std::vector<std::pair<unsigned, std::string>>,
                         MCOperandHash, MCOperandEqual>>
      phi_args;
  std::vector<MCOperand> fn_args;

public:
  llvm::MCInstrAnalysis *Ana_ptr;
  llvm::MCInstPrinter *IP_ptr;
  llvm::MCRegisterInfo *MRI_ptr;
  std::vector<MCBasicBlock> BBs;
  std::unordered_map<MCBasicBlock *, BlockSetTy> dom_tree_inv;

  MCFunction() {}
  MCFunction(std::string _name) : name(_name) {}

  void setName(std::string _name) {
    name = _name;
  }

  MCBasicBlock *addBlock(std::string b_name) {
    return &BBs.emplace_back(b_name);
  }

  std::string getName() {
    return name;
  }

  std::string getLabel() {
    return name + std::to_string(++label_cnt);
  }

  MCBasicBlock *findBlockByName(std::string b_name) {
    for (auto &bb : BBs) {
      if (bb.getName() == b_name) {
        return &bb;
      }
    }
    return nullptr;
  }

  // Make sure that we have an entry label with no predecessors
  void addEntryBlock() {
    // If we have an empty assembly function, we need to add an entry block with
    // a return instruction
    if (BBs.empty()) {
      auto new_block = addBlock("entry");
      MCInst ret_instr;
      ret_instr.setOpcode(AArch64::RET);
      ret_instr.addOperand(MCOperand::createReg(AArch64::X0));
      new_block->addInstBegin(std::move(ret_instr));
    }

    if (BBs.size() == 1)
      return;

    bool add_entry_block = false;
    const auto &first_block = BBs[0];
    for (unsigned i = 0; i < BBs.size(); ++i) {
      auto &cur_bb = BBs[i];
      auto &last_mc_instr = cur_bb.getInstrs().back();
      if (Ana_ptr->isConditionalBranch(last_mc_instr.getMCInst()) ||
          Ana_ptr->isUnconditionalBranch(last_mc_instr.getMCInst())) {
        std::string target = last_mc_instr.findTargetLabel();
        if (target == first_block.getName()) {
          add_entry_block = true;
          break;
        }
      }
    }

    if (add_entry_block) {
      cout << "Added arm_tv_entry block\n";
      BBs.emplace(BBs.begin(), "arm_tv_entry");
      MCInst jmp_instr;
      jmp_instr.setOpcode(AArch64::B);
      jmp_instr.addOperand(MCOperand::createImm(1));
      BBs[0].addInstBegin(std::move(jmp_instr));
    }

    return;
  }

  void postOrderDFS(MCBasicBlock &curBlock, BlockSetTy &visited,
                    std::vector<MCBasicBlock *> &postOrder) {
    visited.insert(&curBlock);
    for (auto succ : curBlock.getSuccs()) {
      if (std::find(visited.begin(), visited.end(), succ) == visited.end()) {
        postOrderDFS(*succ, visited, postOrder);
      }
    }
    postOrder.push_back(&curBlock);
  }

  std::vector<MCBasicBlock *> postOrder() {
    std::vector<MCBasicBlock *> postOrder;
    BlockSetTy visited;
    for (auto &curBlock : BBs) {
      if (visited.count(&curBlock) == 0) {
        postOrderDFS(curBlock, visited, postOrder);
      }
    }
    return postOrder;
  }

  // compute the domination relation
  void generateDominator() {
    auto blocks = postOrder();
    std::reverse(blocks.begin(), blocks.end());
    cout << "postOrder\n";
    for (auto &curBlock : blocks) {
      cout << curBlock->getName() << "\n";
      dom[curBlock] = BlockSetTy();
      for (auto &b : blocks) {
        dom[curBlock].insert(b);
      }
    }

    cout << "printing dom before\n";
    printGraph(dom);
    while (true) {
      bool changed = false;
      for (auto &curBlock : blocks) {
        BlockSetTy newDom = intersect(curBlock->getPreds(), dom);
        newDom.insert(curBlock);

        if (newDom != dom[curBlock]) {
          changed = true;
          dom[curBlock] = newDom;
        }
      }
      if (!changed) {
        break;
      }
    }
    cout << "printing dom after\n";
    printGraph(dom);
  }

  void generateDominatorFrontier() {
    auto dominates = invertGraph(dom);
    cout << "printing dom_inverse\n";
    printGraph(dominates);
    for (auto &[block, domSet] : dom) {
      BlockSetTy dominated_succs;
      dom_frontier[block] = BlockSetTy();
      for (auto &dominated : dominates[block]) {
        auto &temp_succs = dominated->getSuccs();
        for (auto &elem : temp_succs) {
          dominated_succs.insert(elem);
        }

        for (auto &b : dominated_succs) {
          if (b == block || dominates[block].count(b) == 0) {
            dom_frontier[block].insert(b);
          }
        }
      }
    }
    cout << "printing dom_frontier\n";
    printGraph(dom_frontier);
    return;
  }

  void generateDomTree() {
    auto dominates = invertGraph(dom);
    cout << "printing dom_inverse\n";
    printGraph(dominates);
    cout << "-----------------\n";
    std::unordered_map<MCBasicBlock *, BlockSetTy> s_dom;
    for (auto &[block, children] : dominates) {
      s_dom[block] = BlockSetTy();
      for (auto &child : children) {
        if (child != block) {
          s_dom[block].insert(child);
        }
      }
    }

    std::unordered_map<MCBasicBlock *, BlockSetTy> child_dom;

    for (auto &[block, children] : s_dom) {
      child_dom[block] = BlockSetTy();
      for (auto &child : children) {
        for (auto &child_doominates : s_dom[child]) {
          child_dom[block].insert(child_doominates);
        }
      }
    }

    for (auto &[block, children] : s_dom) {
      for (auto &child : children) {
        if (child_dom[block].count(child) == 0) {
          dom_tree[block].insert(child);
        }
      }
    }

    cout << "printing s_dom\n";
    printGraph(s_dom);
    cout << "-----------------\n";

    cout << "printing child_dom\n";
    printGraph(child_dom);
    cout << "-----------------\n";

    cout << "printing dom_tree\n";
    printGraph(dom_tree);
    cout << "-----------------\n";

    dom_tree_inv = invertGraph(dom_tree);
    cout << "printing dom_tree_inv\n";
    printGraph(dom_tree_inv);
    cout << "-----------------\n";
  }

  // compute a map from each variable to its defining block
  void findDefiningBlocks() {
    for (auto &block : BBs) {
      for (auto &w_instr : block.getInstrs()) {
        auto &mc_instr = w_instr.getMCInst();
        // need to check for special instructions like ret and branch
        // need to check for special destination operands like WZR

        if (Ana_ptr->isCall(mc_instr))
          report_fatal_error("Function calls not supported yet");

        if (Ana_ptr->isReturn(mc_instr) || Ana_ptr->isBranch(mc_instr)) {
          continue;
        }

        assert(mc_instr.getNumOperands() > 0 && "MCInst with zero operands");

        // CHECK: if there is an ARM instruction that writes to two variables
        auto &dst_operand = mc_instr.getOperand(0);

        assert((dst_operand.isReg() || dst_operand.isImm()) &&
               "unsupported destination operand");

        if (dst_operand.isImm()) {
          cout << "destination operand is an immediate. printing the "
                  "instruction and skipping it\n";
          w_instr.print();
          continue;
        }

        auto dst_reg = dst_operand.getReg();
        // skip constant registers like WZR
        if (dst_reg == AArch64::WZR || dst_reg == AArch64::XZR)
          continue;

        defs[dst_operand].insert(&block);
      }
    }

    // temp for debugging
    for (auto &[var, blockSet] : defs) {
      cout << "defs for \n";
      var.print(errs(), MRI_ptr);
      cout << "\n";
      for (auto &block : blockSet) {
        cout << block->getName() << ",";
      }
      cout << "\n";
    }
  }

  void findPhis() {
    for (auto &[var, block_set] : defs) {
      vector<MCBasicBlock *> block_list(block_set.begin(), block_set.end());
      for (unsigned i = 0; i < block_list.size(); ++i) {
        // auto& df_blocks = dom_frontier[block_list[i]];
        for (auto block_ptr : dom_frontier[block_list[i]]) {
          if (phis[block_ptr].count(var) == 0) {
            phis[block_ptr].insert(var);

            if (std::find(block_list.begin(), block_list.end(), block_ptr) ==
                block_list.end()) {
              block_list.push_back(block_ptr);
            }
          }
        }
      }
    }
    // temp for debugging
    cout << "mapping from block name to variable names that require phi nodes "
            "in block\n";
    for (auto &[block, varSet] : phis) {
      cout << "phis for: " << block->getName() << "\n";
      for (auto &var : varSet) {
        var.print(errs(), MRI_ptr);
        cout << "\n";
      }
      cout << "-------------\n";
    }
  }

  // FIXME: this is duplicated code. need to refactor
  void findArgs(std::optional<IR::Function> &src_fn) {
    unsigned arg_num = 0;

    for (auto &v : src_fn->getInputs()) {
      auto &typ = v.getType();
      if (!typ.isIntType())
        report_fatal_error("Only int types supported for now");
      // FIXME. Do a switch statement to figure out which register to start from
      auto start = typ.bits() == 32 ? AArch64::W0 : AArch64::X0;
      auto arg = MCOperand::createReg(start + (arg_num++));
      fn_args.push_back(std::move(arg));
    }

    // temp for debugging
    cout << "printing fn_args\n";
    for (auto &arg : fn_args) {
      arg.print(errs(), MRI_ptr);
      cout << "\n";
    }
  }

  // go over 32 bit registers and replace them with the corresponding 64 bit
  // FIXME: this will probably have some uninteded consequences that we need to
  // identify
  void rewriteOperands() {

    // FIXME: this lambda is pretty hacky and brittle
    auto in_range_rewrite = [](MCOperand &op) {
      if (op.isReg()) {
        if (op.getReg() >= AArch64::W0 &&
            op.getReg() <= AArch64::W28) { // FIXME: Why 28?
          op.setReg(op.getReg() + AArch64::X0 - AArch64::W0);
        } else if (!(op.getReg() >= AArch64::X0 &&
                     op.getReg() <= AArch64::X28) &&
                   !(op.getReg() <= AArch64::XZR &&
                     op.getReg() >= AArch64::WZR) &&
                   !(op.getReg() == AArch64::NoRegister) &&
                   !(op.getReg() == AArch64::LR)) {
          report_fatal_error("Unsupported registers detected in the Assembly");
        }
      }
    };

    for (auto &fn_arg : fn_args) {
      in_range_rewrite(fn_arg);
    }

    for (auto &block : BBs) {
      for (auto &w_instr : block.getInstrs()) {
        auto &mc_instr = w_instr.getMCInst();
        for (unsigned i = 0; i < mc_instr.getNumOperands(); ++i) {
          auto &operand = mc_instr.getOperand(i);
          in_range_rewrite(operand);
        }
      }
    }

    cout << "printing fn_args after rewrite\n";
    for (auto &arg : fn_args) {
      arg.print(errs(), MRI_ptr);
      cout << "\n";
    }

    cout << "printing MCInsts after rewriting operands\n";
    printBlocks();
  }

  void ssaRename() {
    std::unordered_map<MCOperand, std::vector<unsigned>, MCOperandHash,
                       MCOperandEqual>
        stack;
    std::unordered_map<MCOperand, unsigned, MCOperandHash, MCOperandEqual>
        counters;

    cout << "SSA rename\n";

    // auto printStack = [&](std::unordered_map<MCOperand,
    // std::vector<unsigned>,
    //                                          MCOperandHash, MCOperandEqual>
    //                           s) {
    //   for (auto &[var, stack_vec] : s) {
    //     errs() << "stack for ";
    //     var.print(errs(), MRI_ptr);
    //     errs() << "\n";
    //     for (auto &stack_item : stack_vec) {
    //       cout << stack_item << ",";
    //     }
    //     cout << "\n";
    //   }
    // };

    auto pushFresh = [&](const MCOperand &op) {
      if (counters.find(op) == counters.end()) {
        counters[op] = 2; // Set the stack to 2 to account for input registers
                          // and renaming (freeze + extension)
      }
      auto fresh_id = counters[op]++;
      auto &var_stack = stack[op];
      var_stack.insert(var_stack.begin(), fresh_id);
      return fresh_id;
    };

    std::function<void(MCBasicBlock *)> rename;
    rename = [&](MCBasicBlock *block) {
      auto old_stack = stack;
      cout << "renaming block: " << block->getName() << "\n";
      block->print();
      cout << "----\n";
      for (auto &phi_var : phis[block]) {

        MCInst new_phi_instr;
        new_phi_instr.setOpcode(AArch64::PHI);
        new_phi_instr.addOperand(MCOperand::createReg(phi_var.getReg()));
        new_phi_instr.dump_pretty(errs(), IP_ptr, " ", MRI_ptr);

        MCInstWrapper new_w_instr(new_phi_instr);
        block->addInstBegin(std::move(new_w_instr));
        auto phi_dst_id = pushFresh(phi_var);
        cout << "phi_dst_id: " << phi_dst_id << "\n";
        block->getInstrs()[0].setOpId(0, phi_dst_id);
      }
      cout << "after phis\n";
      block->print();
      cout << "----\n";

      cout << "renaming instructions\n";
      for (auto &w_instr : block->getInstrs()) {
        auto &mc_instr = w_instr.getMCInst();

        if (mc_instr.getOpcode() == AArch64::PHI) {
          continue;
        }

        assert(mc_instr.getNumOperands() > 0 && "MCInst with zero operands");

        // nothing to rename
        if (mc_instr.getNumOperands() == 1) {
          continue;
        }

        // mc_instr.dump_pretty(errs(), IP_ptr, " ", MRI_ptr);
        // errs() << "\n";
        // errs() << "printing stack\n";
        // printStack(stack);
        // errs() << "printing operands\n";
        unsigned i = 1;
        if (instrs_no_write.contains(mc_instr.getOpcode())) {
          cout << "iterating from first element in rename\n";
          i = 0;
        }

        for (; i < mc_instr.getNumOperands(); ++i) {
          auto &op = mc_instr.getOperand(i);
          if (!op.isReg()) {
            continue;
          }

          auto op_reg_num = op.getReg();
          if (op_reg_num == AArch64::WZR || op_reg_num == AArch64::XZR) {
            continue;
          }

          op.print(errs(), MRI_ptr);
          errs() << "\n";

          auto &arg_id = stack[op][0];
          w_instr.setOpId(i, arg_id);
        }
        errs() << "printing operands done\n";
        if (instrs_no_write.contains(mc_instr.getOpcode()))
          continue;

        errs() << "renaming dst\n";
        auto &dst_op = mc_instr.getOperand(0);
        dst_op.print(errs(), MRI_ptr);
        auto dst_id = pushFresh(dst_op);
        w_instr.setOpId(0, dst_id);
        errs() << "\n";
      }

      errs() << "renaming phi args in block's successors\n";

      for (auto s_block : block->getSuccs()) {
        errs() << block->getName() << " -> " << s_block->getName() << "\n";

        for (auto &phi_var : phis[s_block]) {
          if (stack.find(phi_var) == stack.end()) {
            phi_var.print(errs(), MRI_ptr);
            assert(false && "phi var not in stack");
          }
          assert(stack[phi_var].size() > 0 && "phi var stack empty");

          if (phi_args[s_block].find(phi_var) == phi_args[s_block].end()) {
            phi_args[s_block][phi_var] =
                std::vector<std::pair<unsigned, std::string>>();
          }
          errs() << "phi_arg[" << s_block->getName() << "][" << phi_var.getReg()
                 << "]=" << stack[phi_var][0] << "\n";
          phi_args[s_block][phi_var].push_back(
              std::make_pair(stack[phi_var][0], block->getName()));
        }
      }

      for (auto b : dom_tree[block]) {
        rename(b);
      }

      stack = old_stack;
    };

    auto entry_block_ptr = &(BBs[0]);

    entry_block_ptr->getInstrs()[0].print();

    for (auto &arg : fn_args) {
      stack[arg] = std::vector<unsigned>();
      pushFresh(arg);
    }
    rename(entry_block_ptr);
    cout << "printing MCInsts after renaming operands\n";
    printBlocks();

    cout << "printing phi args\n";
    for (auto &[block, phi_vars] : phi_args) {
      cout << "block: " << block->getName() << "\n";
      for (auto &[phi_var, args] : phi_vars) {
        cout << "phi_var: " << phi_var.getReg() << "\n";
        for (auto arg : args) {
          cout << arg.first << "-" << arg.second << ", ";
        }
        cout << "\n";
      }
    }

    cout << "-----------------\n"; // adding args to phi-nodes
    for (auto &[block, phi_vars] : phi_args) {
      for (auto &w_instr : block->getInstrs()) {
        auto &mc_instr = w_instr.getMCInst();
        if (mc_instr.getOpcode() != AArch64::PHI) {
          break;
        }

        auto phi_var = mc_instr.getOperand(0);
        unsigned index = 1;
        cout << "phi arg size " << phi_args[block][phi_var].size() << "\n";
        for (auto var_id_label_pair : phi_args[block][phi_var]) {
          cout << "index = " << index
               << ", var_id = " << var_id_label_pair.first << "\n";
          mc_instr.addOperand(MCOperand::createReg(phi_var.getReg()));
          w_instr.pushOpId(var_id_label_pair.first);
          w_instr.setOpPhiBlock(index, var_id_label_pair.second);
          w_instr.print();
          index++;
        }
      }
    }

    cout << "printing MCInsts after adding args to phi-nodes\n";
    for (auto &b : BBs) {
      cout << b.getName() << ":\n";
      b.print();
    }
  }

  // helper function to compute the intersection of predecessor dominator sets
  BlockSetTy intersect(BlockSetTy &preds,
                       std::unordered_map<MCBasicBlock *, BlockSetTy> &dom) {
    BlockSetTy ret;
    if (preds.size() == 0) {
      return ret;
    }
    if (preds.size() == 1) {
      return dom[*preds.begin()];
    }
    ret = dom[*preds.begin()];
    auto second = ++preds.begin();
    for (auto it = second; it != preds.end(); ++it) {
      auto &pred_set = dom[*it];
      BlockSetTy new_ret;
      for (auto &b : ret) {
        if (pred_set.count(b) == 1) {
          new_ret.insert(b);
        }
      }
      ret = new_ret;
    }
    return ret;
  }

  // helper function to invert a graph
  std::unordered_map<MCBasicBlock *, BlockSetTy>
  invertGraph(std::unordered_map<MCBasicBlock *, BlockSetTy> &graph) {
    std::unordered_map<MCBasicBlock *, BlockSetTy> res;
    for (auto &curBlock : graph) {
      for (auto &succ : curBlock.second) {
        res[succ].insert(curBlock.first);
      }
    }
    return res;
  }

  // Debug function to print domination info
  void printGraph(std::unordered_map<MCBasicBlock *, BlockSetTy> &graph) {
    for (auto &curBlock : graph) {
      cout << curBlock.first->getName() << ": ";
      for (auto &dst : curBlock.second) {
        cout << dst->getName() << " ";
      }
      cout << "\n";
    }
  }

  void printBlocks() {
    cout << "#of Blocks = " << BBs.size() << '\n';
    cout << "-------------\n";
    int i = 0;
    for (auto &block : BBs) {
      errs() << "block " << i << ", name= " << block.getName() << '\n';
      for (auto &inst : block.getInstrs()) {
        inst.getMCInst().dump_pretty(llvm::errs(), IP_ptr, " ", MRI_ptr);
        errs() << '\n';
      }
      i++;
    }
  }
};

// Some variables that we need to maintain as we're performing arm-tv
static std::map<std::pair<unsigned, unsigned>, IR::Value *> cache;
static std::unordered_map<MCOperand, unique_ptr<IR::StructType>, MCOperandHash,
                          MCOperandEqual>
    overflow_aggregate_types;
unsigned type_id_counter{0};

// Keep track of which oprands had their type adjusted and their original
// bitwidth
std::vector<std::pair<unsigned, unsigned>> new_input_idx_bitwidth;
unsigned int original_ret_bitwidth{64};
bool has_ret_attr{false};

// Generate the required struct type for an alive2 *_overflow instructions
// FIXME: @ryan-berger padding may change, hardcoding 24 bits is a bad idea
// FIXME these type object generators should be either grouped in one class or
// be refactored in some other way.
// We should also pass something more useful than just one operand that can be
// used as a key to cache complex types as right now this function leaks memory
// This function should also be moved to utils.cpp as it will need to use
// objects that are defined there further I'm not sure if the padding matters at
// this point but the code is based on utils.cpp llvm_type2alive function that
// uses padding for struct types
static IR::Type *sadd_overflow_type(MCOperand op, int size) {
  assert(op.isReg());

  auto p = overflow_aggregate_types.try_emplace(op);
  auto &st = p.first->second;
  vector<IR::Type *> elems;
  vector<bool> is_padding{false, false, true};

  if (p.second) {
    auto add_res_ty = &get_int_type(size);
    auto add_ov_ty = &get_int_type(1);
    auto padding_ty = &get_int_type(24);
    elems.push_back(add_res_ty);
    elems.push_back(add_ov_ty);
    elems.push_back(padding_ty);
    st = make_unique<IR::StructType>("ty_" + to_string(type_id_counter++),
                                     move(elems), move(is_padding));
  }
  return st.get();
}

// static IR::Type *uadd_overflow_type(MCOperand op, int size) {
//   assert(op.isReg());
//
//   auto p = overflow_aggregate_types.try_emplace(op);
//   auto &st = p.first->second;
//   vector<IR::Type *> elems;
//   vector<bool> is_padding{false, false, true};
//
//   if (p.second) {
//     auto add_res_ty = &get_int_type(size);
//     auto add_ov_ty = &get_int_type(1);
//     auto padding_ty = &get_int_type(24);
//     elems.push_back(add_res_ty);
//     elems.push_back(add_ov_ty);
//     elems.push_back(padding_ty);
//     st = make_unique<IR::StructType>("ty_" + to_string(type_id_counter++),
//                                  move(elems), move(is_padding));
//   }
//   return st.get();
// }

// Add IR value to cache
void mc_add_identifier(unsigned reg, unsigned version, IR::Value &v) {
  cache.emplace(std::make_pair(reg, version), &v);
}

IR::Value *mc_get_operand(unsigned reg, unsigned version) {
  if (auto I = cache.find(std::make_pair(reg, version)); I != cache.end())
    return I->second;
  return nullptr;
}

// Code taken from llvm. This should be okay for now. But we generally
// don't want to trust the llvm implementation so we need to complete my
// implementation at function decode_bit_mask
static inline uint64_t ror(uint64_t elt, unsigned size) {
  return ((elt & 1) << (size - 1)) | (elt >> 1);
}

/// decodeLogicalImmediate - Decode a logical immediate value in the form
/// "N:immr:imms" (where the immr and imms fields are each 6 bits) into the
/// integer value it represents with regSize bits.
static inline uint64_t decodeLogicalImmediate(uint64_t val, unsigned regSize) {
  // Extract the N, imms, and immr fields.
  unsigned N = (val >> 12) & 1;
  unsigned immr = (val >> 6) & 0x3f;
  unsigned imms = val & 0x3f;

  assert((regSize == 64 || N == 0) && "undefined logical immediate encoding");
  int len = 31 - llvm::countLeadingZeros((N << 6) | (~imms & 0x3f));
  assert(len >= 0 && "undefined logical immediate encoding");
  unsigned size = (1 << len);
  unsigned R = immr & (size - 1);
  unsigned S = imms & (size - 1);
  assert(S != size - 1 && "undefined logical immediate encoding");
  uint64_t pattern = (1ULL << (S + 1)) - 1;
  for (unsigned i = 0; i < R; ++i)
    pattern = ror(pattern, size);

  // Replicate the pattern to fill the regSize.
  while (size != regSize) {
    pattern |= (pattern << size);
    size *= 2;
  }
  return pattern;
}

// replicate is a pseudocode function used in the ARM ISA
// replicate's documentation isn't particularly clear, but it takes a bit-vector
// of size M, and duplicates it N times, returning a bit-vector of size M*N
// reference:
// https://developer.arm.com/documentation/ddi0596/2020-12/Shared-Pseudocode/Shared-Functions?lang=en#impl-shared.Replicate.2
llvm::APInt replicate(llvm::APInt bits, unsigned N) {
  auto bitsWidth = bits.getBitWidth();

  auto newInt = llvm::APInt(bitsWidth * N, 0);
  auto mask = llvm::APInt(bitsWidth * N, bits.getZExtValue());
  for (size_t i = 0; i < N; i++) {
    newInt |= (mask << (bitsWidth * i));
  }
  return newInt;
}

// adapted from the arm ISA DecodeBitMasks:
// https://developer.arm.com/documentation/ddi0596/2020-12/Shared-Pseudocode/AArch64-Instrs?lang=en#impl-aarch64.DecodeBitMasks.4
// Decode AArch64 bitfield and logical immediate masks which use a similar
// encoding structure
// TODO: this is super broken
std::tuple<llvm::APInt, llvm::APInt> decode_bit_mask(bool immNBit,
                                                     uint32_t _imms,
                                                     uint32_t _immr,
                                                     bool immediate, int M) {
  llvm::APInt imms(6, _imms);
  llvm::APInt immr(6, _immr);

  auto notImm = APInt(6, _imms);
  notImm.flipAllBits();

  auto concatted = APInt(1, (immNBit ? 1 : 0)).concat(notImm);
  auto len = concatted.getBitWidth() - concatted.countLeadingZeros() - 1;

  // Undefined behavior
  assert(len >= 1);
  assert(M >= (1 << len));

  auto levels = llvm::APInt::getAllOnes(len).zext(6);

  auto S = (imms & levels);
  auto R = (immr & levels);

  auto diff = S - R;

  auto esize = (1 << len);
  auto d = llvm::APInt(len - 1, diff.getZExtValue());

  auto welem =
      llvm::APInt::getAllOnes(S.getZExtValue() + 1).zext(esize).rotr(R);
  auto telem = llvm::APInt::getAllOnes(d.getZExtValue() + 1).zext(esize);

  auto wmask = replicate(welem, esize);
  auto tmask = replicate(telem, esize);

  return {welem.trunc(M), telem.trunc(M)};
}

// Values currently holding ZNCV bits, for each basicblock respectively
unordered_map<MCBasicBlock *, IR::Value *> cur_vs;
unordered_map<MCBasicBlock *, IR::Value *> cur_zs;
unordered_map<MCBasicBlock *, IR::Value *> cur_ns;
unordered_map<MCBasicBlock *, IR::Value *> cur_cs;

IR::BasicBlock *get_basic_block(IR::Function &f, MCOperand &jmp_tgt) {
  assert(jmp_tgt.isExpr() && "[get_basic_block] expected expression operand");
  assert((jmp_tgt.getExpr()->getKind() == MCExpr::ExprKind::SymbolRef) &&
         "[get_basic_block] expected symbol ref as jump operand");
  const MCSymbolRefExpr &SRE = cast<MCSymbolRefExpr>(*jmp_tgt.getExpr());
  const MCSymbol &Sym = SRE.getSymbol();
  cout << "jump target: " << Sym.getName().str() << '\n';
  auto target_bb = &f.getBB(Sym.getName());
  return target_bb;
}

class arm2alive_ {
  MCFunction &MF;
  // const llvm::DataLayout &DL;
  std::optional<IR::Function> &srcFn;
  IR::BasicBlock *BB; // the current block
  MCBasicBlock *MCBB; // the current machine block
  unsigned blockCount{0};

  MCInstPrinter *instrPrinter;
  MCRegisterInfo *registerInfo;
  std::vector<std::pair<IR::Phi *, MCInstWrapper *>> lift_todo_phis;

  MCInstWrapper *wrapper{nullptr};

  unsigned instructionCount;
  unsigned curId;
  bool ret_void{false};

  std::vector<std::unique_ptr<IR::Instr>> visitError(MCInstWrapper &I) {
    // flush must happen before error is printed to make sure the error
    // comes out nice and pretty when combing the stdout/stderr in scripts
    cout.flush();

    llvm::errs() << "ERROR: Unsupported arm instruction: "
                 << instrPrinter->getOpcodeName(I.getMCInst().getOpcode())
                 << "\n";
    llvm::errs().flush();
    cerr.flush();
    exit(1); // for now lets exit the program if the arm instruction is not
             // supported
  }

  int get_size(int instr) {
    if (instrs_32.contains(instr)) {
      return 32;
    }

    if (instrs_64.contains(instr)) {
      return 64;
    }

    if (instr == AArch64::RET)
      return 0;

    cout << "get_size encountered unknown instruction" << endl;
    visitError(*wrapper);
    UNREACHABLE();
  }

  // yoinked form getShiftType/getShiftValue:
  // https://github.com/llvm/llvm-project/blob/93d1a623cecb6f732db7900baf230a13e6ac6c6a/llvm/lib/Target/AArch64/MCTargetDesc/AArch64AddressingModes.h#L74
  // LLVM encodes its shifts into immediates (ARM doesn't, it has a shift
  // field in the instruction)
  IR::Value *reg_shift(IR::Value *value, int encodedShift) {
    int shift_type = ((encodedShift >> 6) & 0x7);
    auto typ = &value->getType();

    IR::BinOp::Op op;

    switch (shift_type) {
    case 0:
      op = IR::BinOp::Shl;
      break;
    case 1:
      op = IR::BinOp::LShr;
      break;
    case 2:
      op = IR::BinOp::AShr;
      break;
    case 3:
      // ROR shift
      return add_instr<IR::TernaryOp>(
          *typ, next_name(), *value, *value,
          *make_intconst(encodedShift & 0x3f, typ->bits()),
          IR::TernaryOp::FShr);
      break;
    default:
      // FIXME: handle other case (msl)
      assert(false && "shift type not supported");
    }

    return add_instr<IR::BinOp>(
        *typ, next_name(), *value,
        *make_intconst(encodedShift & 0x3f, typ->bits()), op);
  }

  IR::Value *reg_shift(int value, int size, int encodedShift) {
    auto v = make_intconst(value, size);
    return reg_shift(v, encodedShift);
  }

  IR::Value *getIdentifier(unsigned reg, unsigned id) {
    auto val = mc_get_operand(reg, id);

    // if (val == nullptr) {
    //   cout << "getIdentifier: " << reg << " " << id << endl;
    // }
    // assert(val != nullptr && "getIdentifier: null operand");
    return val;
  }

  // TODO: make it so that lshr generates code on register lookups
  //  some instructions make use of this, and the semantics need to be worked
  //  out
  IR::Value *get_value(int idx, int shift = 0) {
    auto inst = wrapper->getMCInst();
    auto op = inst.getOperand(idx);
    auto size = get_size(inst.getOpcode());

    assert(op.isImm() || op.isReg());

    IR::Value *v;
    auto ty = &get_int_type(size);

    if (op.isImm()) {
      v = make_intconst(op.getImm(), size);
    } else if (op.getReg() == AArch64::XZR) {
      v = make_intconst(0, 64);
    } else if (op.getReg() == AArch64::WZR) {
      v = make_intconst(0, 32);
    } else if (size == 64) {
      v = getIdentifier(op.getReg(), wrapper->getOpId(idx));
    } else if (size == 32) {
      auto tmp = getIdentifier(op.getReg(), wrapper->getOpId(idx));
      v = add_instr<IR::ConversionOp>(*ty, next_name(), *tmp,
                                      IR::ConversionOp::Trunc);
    } else {
      assert(false && "unhandled case in get_value*");
    }

    if (shift != 0) {
      v = reg_shift(v, shift);
    }

    return v;
  }

  // Generates string name for the next alive instruction
  std::string next_name() {
    std::stringstream ss;
    if (instrs_no_write.contains(wrapper->getOpcode())) {
      ss << "\%tx" << ++curId << "x" << instructionCount << "x" << blockCount;
    } else {
      ss << "\%"
         << registerInfo->getName(wrapper->getMCInst().getOperand(0).getReg())
         << "_" << wrapper->getOpId(0) << "x" << ++curId << "x"
         << instructionCount << "x" << blockCount;
    }
    return ss.str();
  }

  std::string next_name(unsigned reg_num, unsigned id_num) {
    std::stringstream ss;
    ss << "\%" << registerInfo->getName(reg_num) << "_" << id_num;
    return ss.str();
  }

  void add_phi_params(IR::Phi *phi_instr, MCInstWrapper *phi_mc_wrapper) {
    // auto val = mc_get_operand(reg, id);
    assert(phi_mc_wrapper->getOpcode() == AArch64::PHI &&
           "cannot add params to non-phi instr");
    for (unsigned i = 1; i < phi_mc_wrapper->getMCInst().getNumOperands();
         i++) {
      assert(phi_mc_wrapper->getMCInst().getOperand(i).isReg());
      cout << "<Phi arg>:[("
           << phi_mc_wrapper->getMCInst().getOperand(i).getReg() << ","
           << phi_mc_wrapper->getOpId(i) << "),"
           << phi_mc_wrapper->getOpPhiBlock(i) << "]>\n";
      string block_name(phi_mc_wrapper->getOpPhiBlock(i));
      auto val =
          mc_get_operand(phi_mc_wrapper->getMCInst().getOperand(i).getReg(),
                         phi_mc_wrapper->getOpId(i));
      assert(val != nullptr);
      cout << "block name = " << block_name << endl;
      phi_instr->addValue(*val, std::move(block_name));
      cout << "i is = " << i << endl;
    }
    cout << "exiting add_phi_params \n";
  }

  void add_identifier(IR::Value &v) {
    auto reg = wrapper->getMCInst().getOperand(0).getReg();
    auto version = wrapper->getOpId(0);
    // TODO: this probably should be in visit
    instructionCount++;
    mc_add_identifier(reg, version, v);
  }

  // store will store an IR value using the current instruction's destination
  // register.
  // All values are kept track of in their full-width counterparts to simulate
  // registers. For example, a x0 register would be kept track of in the bottom
  // bits of w0. Optionally, there is an "s" or signed flag that can be used
  // when writing smaller bit-width values to half or full-width registers which
  // will perform a small sign extension procedure.
  void store(IR::Value &v, bool s = false) {
    if (wrapper->getMCInst().getOperand(0).getReg() == AArch64::WZR) {
      instructionCount++;
      return;
    }

    // if v.bits() == 64, regSize == 64 because of above assertion
    if (v.bits() == 64) {
      add_identifier(v);
      return;
    }

    size_t regSize = get_size(wrapper->getMCInst().getOpcode());

    // regSize should only be 32/64
    assert(regSize == 32 || regSize == 64);

    // if the s flag is set, the value is smaller than 32 bits,
    // and the register we are storing it in _is_ 32 bits, we sign extend
    // to 32 bits before zero-extending to 64
    if (s && regSize == 32 && v.bits() < 32) {
      auto ty = &get_int_type(32);
      auto sext32 = add_instr<IR::ConversionOp>(*ty, next_name(), v,
                                                IR::ConversionOp::SExt);

      ty = &get_int_type(64);
      auto zext64 = add_instr<IR::ConversionOp>(*ty, next_name(), *sext32,
                                                IR::ConversionOp::ZExt);

      add_identifier(*zext64);
      return;
    }

    auto op = s ? IR::ConversionOp::SExt : IR::ConversionOp::ZExt;
    auto ty = &get_int_type(64);
    auto new_val = add_instr<IR::ConversionOp>(*ty, next_name(), v, op);

    add_identifier(*new_val);
  }

  IR::Value *
  retrieve_pstate(unordered_map<MCBasicBlock *, IR::Value *> &pstate_map,
                  MCBasicBlock *bb) {
    auto pstate_val = pstate_map[bb];
    if (pstate_val) {
      return pstate_val;
    }
    cout << "retrieving pstate for block " << bb->getName() << endl;
    assert(
        bb->getPreds().size() == 1 &&
        "pstate can only be retrieved for blocks with up to one predecessor");
    auto pred_bb = bb->getPreds().front();
    auto pred_pstate = pstate_map[pred_bb];
    assert(pred_pstate != nullptr && "pstate must be defined for predecessor");
    pstate_map[bb] = pred_pstate;
    return pred_pstate;
  }

  IR::Value *evaluate_condition(uint64_t cond, MCBasicBlock *bb) {
    // cond<0> == '1' && cond != '1111'
    auto invert_bit = (cond & 1) && (cond != 15);

    cond >>= 1;

    auto cur_v = retrieve_pstate(cur_vs, bb);
    auto cur_z = retrieve_pstate(cur_zs, bb);
    auto cur_n = retrieve_pstate(cur_ns, bb);
    auto cur_c = retrieve_pstate(cur_cs, bb);

    assert(cur_v != nullptr && cur_z != nullptr && cur_n != nullptr &&
           cur_c != nullptr && "condition not initialized");

    IR::Value *res = nullptr;
    switch (cond) {
    case 0:
      res = cur_z;
      break; // EQ/NE
    case 1:
      res = cur_c;
      break; // CS/CC
    case 2:
      res = cur_n;
      break; // MI/PL
    case 3:
      res = cur_v;
      break; // VS/VC
    case 4:  // HI/LS: PSTATE.C == '1' && PSTATE.Z == '0'
    {
      assert(cur_c != nullptr && cur_z != nullptr &&
             "HI/LS requires C and Z bits to be generated");
      auto ty = &get_int_type(1);

      // C == 1
      auto c_cond = add_instr<IR::ICmp>(*ty, next_name(), IR::ICmp::EQ, *cur_c,
                                        *make_intconst(1, 1));
      // Z == 0
      auto z_cond = add_instr<IR::ICmp>(*ty, next_name(), IR::ICmp::EQ, *cur_z,
                                        *make_intconst(0, 1));
      // C == 1 && Z == 0
      res = add_instr<IR::BinOp>(*ty, next_name(), *c_cond, *z_cond,
                                 IR::BinOp::And);
      break;
    }
    case 5: // GE/LT PSTATE.N == PSTATE.V
    {
      assert(cur_n != nullptr && cur_v != nullptr &&
             "GE/LT requires N and V bits to be generated");
      auto ty = &get_int_type(1);

      res = add_instr<IR::ICmp>(*ty, next_name(), IR::ICmp::EQ, *cur_n, *cur_v);
      break;
    }
    case 6: // GT/LE PSTATE.N == PSTATE.V && PSTATE.Z == 0
    {
      assert(cur_n != nullptr && cur_v != nullptr && cur_z != nullptr &&
             "GT/LE requires N, V and Z bits to be generated");
      auto ty = &get_int_type(1);

      auto n_eq_v =
          add_instr<IR::ICmp>(*ty, next_name(), IR::ICmp::EQ, *cur_n, *cur_v);
      auto z_cond = add_instr<IR::ICmp>(*ty, next_name(), IR::ICmp::EQ, *cur_z,
                                        *make_intconst(0, 1));

      res = add_instr<IR::BinOp>(*ty, next_name(), *n_eq_v, *z_cond,
                                 IR::BinOp::And);
      break;
    }
    case 7: {
      res = make_intconst(1, 1);
      break;
    }
    default:
      assert(false && "invalid condition code");
      break;
    }

    assert(res != nullptr && "condition code was not generated");
    if (invert_bit) {
      auto ty = &get_int_type(1);
      auto one = make_intconst(1, 1);
      res = add_instr<IR::BinOp>(*ty, next_name(), *res, *one, IR::BinOp::Xor);
    }

    return res;
  }

  void set_z(IR::Value *val) {
    auto typ = &get_int_type(1);
    auto zero = make_intconst(0, val->bits());

    auto z =
        add_instr<IR::ICmp>(*typ, next_name(), IR::ICmp::Cond::EQ, *val, *zero);

    cur_zs[MCBB] = z;
  }

  void set_n(IR::Value *val) {
    auto typ = &get_int_type(1);
    auto zero = make_intconst(0, val->bits());

    auto n = add_instr<IR::ICmp>(*typ, next_name(), IR::ICmp::Cond::SLT, *val,
                                 *zero);
    cur_ns[MCBB] = n;
  }

  // add_instr is a thin wrapper around make_unique which adds an instruction to
  // the current basic block, and returns a pointer to the value.
  template <typename _Tp, typename... _Args> _Tp *add_instr(_Args &&...__args) {
    assert(BB != nullptr);
    auto instr = make_unique<_Tp>(std::forward<_Args>(__args)...);
    auto ret = instr.get();

    BB->addInstr(move(instr));

    return ret;
  }

public:
  arm2alive_(MCFunction &MF, std::optional<IR::Function> &srcFn,
             MCInstPrinter *instrPrinter, MCRegisterInfo *registerInfo)
      : MF(MF), srcFn(srcFn), instrPrinter(instrPrinter),
        registerInfo(registerInfo), instructionCount(0), curId(0) {}

  // Visit an MCInstWrapper instructions and convert it to alive IR
  void mc_visit(MCInstWrapper &I, IR::Function &Fn) {
    std::vector<std::unique_ptr<IR::Instr>> res;
    auto opcode = I.getOpcode();
    auto &mc_inst = I.getMCInst();
    wrapper = &I;
    curId = 0;

    auto size = get_size(opcode);
    auto ty = &get_int_type(size);

    switch (opcode) {
    case AArch64::ADDWrs:
    case AArch64::ADDWri:
    case AArch64::ADDWrx:
    case AArch64::ADDSWrs:
    case AArch64::ADDSWri:
    case AArch64::ADDSWrx:
    case AArch64::ADDXrs:
    case AArch64::ADDXri:
    case AArch64::ADDXrx:
    case AArch64::ADDSXrs:
    case AArch64::ADDSXri:
    case AArch64::ADDSXrx: {
      auto a = get_value(1);
      IR::Value *b;

      switch (opcode) {
      case AArch64::ADDWrx:
      case AArch64::ADDSWrx:
      case AArch64::ADDXrx:
      case AArch64::ADDSXrx: {
        auto extendImm = mc_inst.getOperand(3).getImm();
        auto extendType = ((extendImm >> 3) & 0x7);

        cout << "extendImm: " << extendImm << ", extendType: " << extendType
             << "\n";

        auto isSigned = extendType / 4;

        // extendSize is necessary so that we can start with the word size
        // ARM wants us to (byte, half, full) and then sign extend to a new
        // size. Without extendSize being used for a trunc, a lot of masking
        // and more manual work to sign extend would be necessary
        unsigned extendSize = 8 << (extendType % 4);
        auto shift = extendImm & 0x7;

        b = get_value(2);

        // Make sure to not to trunc to the same size as the parameter.
        // Sometimes ADDrx is generated using 32 bit registers and "extends" to
        // a 32 bit value. This is seen as a type error in Alive, but is valid
        // ARM
        if (extendSize != ty->bits()) {
          auto truncType = &get_int_type(extendSize);
          b = add_instr<IR::ConversionOp>(*truncType, next_name(), *b,
                                          IR::ConversionOp::Trunc);
          b = add_instr<IR::ConversionOp>(*ty, next_name(), *b,
                                          isSigned ? IR::ConversionOp::SExt
                                                   : IR::ConversionOp::ZExt);
        }

        // shift may not be there, it may just be the extend
        if (shift != 0) {
          b = add_instr<IR::BinOp>(*ty, next_name(), *b,
                                   *make_intconst(shift, size), IR::BinOp::Shl);
        }
        break;
      }
      default:
        b = get_value(2, mc_inst.getOperand(3).getImm());
        break;
      }

      if (has_s(opcode)) {
        auto overflow_type = sadd_overflow_type(mc_inst.getOperand(1), size);
        auto sadd = add_instr<IR::BinOp>(*overflow_type, next_name(), *a, *b,
                                         IR::BinOp::SAdd_Overflow);

        auto result = add_instr<IR::ExtractValue>(*ty, next_name(), *sadd);
        result->addIdx(0);

        auto i1 = &get_int_type(1);
        // generate v flag from SAdd result
        auto new_v = add_instr<IR::ExtractValue>(*i1, next_name(), *sadd);
        new_v->addIdx(1);

        // generate c flag from UAdd result
        auto uadd = add_instr<IR::BinOp>(*overflow_type, next_name(), *a, *b,
                                         IR::BinOp::UAdd_Overflow);
        auto new_c = add_instr<IR::ExtractValue>(*i1, next_name(), *uadd);
        new_c->addIdx(1);

        cur_cs[MCBB] = new_c;
        cur_vs[MCBB] = new_v;

        set_n(result);
        set_z(result);

        store(*result);
      }

      auto result =
          add_instr<IR::BinOp>(*ty, next_name(), *a, *b, IR::BinOp::Add);
      store(*result);
      break;
    }
    case AArch64::ASRVWr:
    case AArch64::ASRVXr: {
      auto a = get_value(1);
      auto b = get_value(2);

      auto shift_amt = add_instr<IR::BinOp>(
          *ty, next_name(), *b, *make_intconst(size, size), IR::BinOp::URem);

      auto res = add_instr<IR::BinOp>(*ty, next_name(), *a, *shift_amt,
                                      IR::BinOp::AShr);
      store(*res);
      break;
    }
      // SUBrx is a subtract instruction with an extended register.
      // ARM has 8 types of extensions:
      // 000 -> uxtb
      // 001 -> uxth
      // 010 -> uxtw
      // 011 -> uxtx
      // 100 -> sxtb
      // 110 -> sxth
      // 101 -> sxtw
      // 111 -> sxtx
      // To figure out if the extension is signed, we can use (extendType / 4)
      // Since the types repeat byte, half word, word, etc. for signed and
      // unsigned extensions, we can use 8 << (extendType % 4) to calculate
      // the extension's byte size
    case AArch64::SUBWri:
    case AArch64::SUBWrs:
    case AArch64::SUBWrx:
    case AArch64::SUBSWrs:
    case AArch64::SUBSWri:
    case AArch64::SUBSWrx:
    case AArch64::SUBXri:
    case AArch64::SUBXrs:
    case AArch64::SUBXrx:
    case AArch64::SUBSXrs:
    case AArch64::SUBSXri:
    case AArch64::SUBSXrx: {
      assert(mc_inst.getNumOperands() == 4); // dst, lhs, rhs, shift amt
      assert(mc_inst.getOperand(3).isImm());

      // convert lhs, rhs operands to IR::Values
      auto a = get_value(1);
      IR::Value *b;
      switch (opcode) {
      case AArch64::SUBWrx:
      case AArch64::SUBSWrx:
      case AArch64::SUBXrx:
      case AArch64::SUBSXrx: {
        auto extendImm = mc_inst.getOperand(3).getImm();
        auto extendType = ((extendImm >> 3) & 0x7);

        auto isSigned = extendType / 4;
        // extendSize is necessary so that we can start with the word size
        // ARM wants us to (byte, half, full) and then sign extend to a new
        // size. Without extendSize being used for a trunc, a lot of masking
        // and more manual work to sign extend would be necessary
        unsigned extendSize = 8 << (extendType % 4);
        auto shift = extendImm & 0x7;

        b = get_value(2);

        // Make sure to not to trunc to the same size as the parameter.
        // Sometimes SUBrx is generated using 32 bit registers and "extends" to
        // a 32 bit value. This is seen as a type error in Alive, but is valid
        // ARM
        if (extendSize != ty->bits()) {
          auto truncType = &get_int_type(extendSize);
          b = add_instr<IR::ConversionOp>(*truncType, next_name(), *b,
                                          IR::ConversionOp::Trunc);
          b = add_instr<IR::ConversionOp>(*ty, next_name(), *b,
                                          isSigned ? IR::ConversionOp::SExt
                                                   : IR::ConversionOp::ZExt);
        }

        // shift may not be there, it may just be the extend
        if (shift != 0) {
          b = add_instr<IR::BinOp>(*ty, next_name(), *b,
                                   *make_intconst(shift, size), IR::BinOp::Shl);
        }
        break;
      }
      default:
        b = get_value(2, mc_inst.getOperand(3).getImm());
      }

      // make sure that lhs and rhs conversion succeeded, type lookup succeeded
      if (!ty || !a || !b)
        visitError(I);

      if (has_s(opcode)) {
        auto ty_ptr = sadd_overflow_type(mc_inst.getOperand(1), size);

        auto ssub = add_instr<IR::BinOp>(*ty_ptr, next_name(), *a, *b,
                                         IR::BinOp::SSub_Overflow);
        auto result = add_instr<IR::ExtractValue>(*ty, next_name(), *ssub);
        result->addIdx(0);

        auto ty_i1 = &get_int_type(1);

        auto new_v = add_instr<IR::ExtractValue>(*ty_i1, next_name(), *ssub);
        new_v->addIdx(1);

        cur_cs[MCBB] =
            add_instr<IR::ICmp>(*ty_i1, next_name(), IR::ICmp::UGE, *a, *b);
        cur_zs[MCBB] =
            add_instr<IR::ICmp>(*ty_i1, next_name(), IR::ICmp::EQ, *a, *b);
        cur_vs[MCBB] = new_v;
        set_n(result);
        store(*result);
        break;
      }

      auto sub = add_instr<IR::BinOp>(*ty, next_name(), *a, *b, IR::BinOp::Sub);
      store(*sub);
      break;
    }
    case AArch64::CSELWr:
    case AArch64::CSELXr: {
      assert(mc_inst.getNumOperands() == 4); // dst, lhs, rhs, cond
      // TODO decode condition and find the approprate cond val
      assert(mc_inst.getOperand(1).isReg() && mc_inst.getOperand(2).isReg());
      assert(mc_inst.getOperand(3).isImm());

      auto a = get_value(1);
      auto b = get_value(2);

      auto cond_val_imm = mc_inst.getOperand(3).getImm();
      auto cond_val = evaluate_condition(cond_val_imm, MCBB);

      if (!ty || !a || !b)
        visitError(I);

      auto ret = add_instr<IR::Select>(*ty, next_name(), *cond_val, *a, *b);
      store(*ret);
      break;
    }
    case AArch64::ANDWri:
    case AArch64::ANDWrr:
    case AArch64::ANDWrs:
    case AArch64::ANDSWri:
    case AArch64::ANDSWrr:
    case AArch64::ANDSWrs:
    case AArch64::ANDXri:
    case AArch64::ANDXrr:
    case AArch64::ANDXrs:
    case AArch64::ANDSXri:
    case AArch64::ANDSXrr:
    case AArch64::ANDSXrs: {
      IR::Value *rhs;
      if (mc_inst.getOperand(2).isImm()) {
        auto imm = decodeLogicalImmediate(mc_inst.getOperand(2).getImm(), size);
        rhs = make_intconst(imm, size);

      } else {
        rhs = get_value(2);
      }

      // We are in a ANDrs case. We need to handle a shift
      if (mc_inst.getNumOperands() == 4) {
        // the 4th operand (if it exists) must be an immediate
        assert(mc_inst.getOperand(3).isImm());
        rhs = reg_shift(rhs, mc_inst.getOperand(3).getImm());
      }

      auto and_op = add_instr<IR::BinOp>(*ty, next_name(), *get_value(1), *rhs,
                                         IR::BinOp::And);

      if (has_s(opcode)) {
        set_n(and_op);
        set_z(and_op);
        cur_cs[MCBB] = make_intconst(0, 1);
        cur_vs[MCBB] = make_intconst(0, 1);
      }

      store(*and_op);
      break;
    }
    case AArch64::MADDWrrr:
    case AArch64::MADDXrrr: {
      auto mul_lhs = get_value(1, 0);
      auto mul_rhs = get_value(2, 0);
      auto addend = get_value(3, 0);

      auto mul = add_instr<IR::BinOp>(*ty, next_name(), *mul_lhs, *mul_rhs,
                                      IR::BinOp::Mul);
      auto add =
          add_instr<IR::BinOp>(*ty, next_name(), *mul, *addend, IR::BinOp::Add);
      store(*add);
      break;
    }
    case AArch64::UMADDLrrr: {
      auto mul_lhs = get_value(1, 0);
      auto mul_rhs = get_value(2, 0);
      auto addend = get_value(3, 0);

      auto lhs_masked = add_instr<IR::BinOp>(
          *ty, next_name(), *mul_lhs,
          *make_intconst((uint64_t)0xffffffff, size), IR::BinOp::And);

      auto rhs_masked = add_instr<IR::BinOp>(
          *ty, next_name(), *mul_rhs,
          *make_intconst((uint64_t)0xffffffff, size), IR::BinOp::And);

      auto mul = add_instr<IR::BinOp>(*ty, next_name(), *lhs_masked,
                                      *rhs_masked, IR::BinOp::Mul);
      auto add =
          add_instr<IR::BinOp>(*ty, next_name(), *mul, *addend, IR::BinOp::Add);
      store(*add);
      break;
    }
    case AArch64::SMADDLrrr: {
      // Signed Multiply-Add Long multiplies two 32-bit register values,
      // adds a 64-bit register value, and writes the result to the 64-bit
      // destination register.
      auto mul_lhs = get_value(1, 0);
      auto mul_rhs = get_value(2, 0);
      auto addend = get_value(3, 0);

      auto i32 = &get_int_type(32);
      auto i64 = &get_int_type(64);

      // The inputs are automatically zero extended, but we want sign extension,
      // so we need to truncate them back to i32s
      auto lhs_trunc = add_instr<IR::ConversionOp>(*i32, next_name(), *mul_lhs,
                                                   IR::ConversionOp::Trunc);
      auto rhs_trunc = add_instr<IR::ConversionOp>(*i32, next_name(), *mul_rhs,
                                                   IR::ConversionOp::Trunc);

      // For signed multiplication, must sign extend the lhs and rhs to not
      // overflow
      auto lhs_extended = add_instr<IR::ConversionOp>(
          *i64, next_name(), *lhs_trunc, IR::ConversionOp::SExt);
      auto rhs_extended = add_instr<IR::ConversionOp>(
          *i64, next_name(), *rhs_trunc, IR::ConversionOp::SExt);

      auto mul = add_instr<IR::BinOp>(*ty, next_name(), *lhs_extended,
                                      *rhs_extended, IR::BinOp::Mul);
      auto add =
          add_instr<IR::BinOp>(*ty, next_name(), *mul, *addend, IR::BinOp::Add);
      store(*add);
      break;
    }
    case AArch64::SMSUBLrrr:
    case AArch64::UMSUBLrrr: {
      // SMSUBL: Signed Multiply-Subtract Long.
      // UMSUBL: Unsigned Multiply-Subtract Long.
      IR::Value *mul_lhs;
      IR::Value *mul_rhs;
      if (wrapper->getMCInst().getOperand(1).getReg() == AArch64::WZR) {
        mul_lhs = make_intconst(0, size);
      } else {
        mul_lhs = get_value(1, 0);
      }

      if (wrapper->getMCInst().getOperand(2).getReg() == AArch64::WZR) {
        mul_rhs = make_intconst(0, size);
      } else {
        mul_rhs = get_value(2, 0);
      }
      auto minuend = get_value(3, 0);
      auto i32 = &get_int_type(32);
      auto i64 = &get_int_type(64);

      IR::Value *lhs_extended;
      IR::Value *rhs_extended;

      // The inputs are automatically zero extended, but we want sign
      // extension for signed, so we need to truncate them back to i32s
      auto lhs_trunc = add_instr<IR::ConversionOp>(*i32, next_name(), *mul_lhs,
                                                   IR::ConversionOp::Trunc);
      auto rhs_trunc = add_instr<IR::ConversionOp>(*i32, next_name(), *mul_rhs,
                                                   IR::ConversionOp::Trunc);

      if (opcode == AArch64::SMSUBLrrr) {
        // For signed multiplication, must sign extend the lhs and rhs to not
        // overflow
        lhs_extended = add_instr<IR::ConversionOp>(
            *i64, next_name(), *lhs_trunc, IR::ConversionOp::SExt);
        rhs_extended = add_instr<IR::ConversionOp>(
            *i64, next_name(), *rhs_trunc, IR::ConversionOp::SExt);
      } else {
        lhs_extended = add_instr<IR::ConversionOp>(
            *i64, next_name(), *lhs_trunc, IR::ConversionOp::ZExt);
        rhs_extended = add_instr<IR::ConversionOp>(
            *i64, next_name(), *rhs_trunc, IR::ConversionOp::ZExt);
      }

      auto mul = add_instr<IR::BinOp>(*ty, next_name(), *lhs_extended,
                                      *rhs_extended, IR::BinOp::Mul);
      auto subtract = add_instr<IR::BinOp>(*ty, next_name(), *minuend, *mul,
                                           IR::BinOp::Sub);
      store(*subtract);
      break;
    }
    case AArch64::SMULHrr:
    case AArch64::UMULHrr: {
      // SMULH: Signed Multiply High
      // UMULH: Unsigned Multiply High
      auto mul_lhs = get_value(1, 0);
      auto mul_rhs = get_value(2, 0);

      auto i64 = &get_int_type(64);
      auto i128 = &get_int_type(128);

      IR::ConversionOp *lhs_extended;
      IR::ConversionOp *rhs_extended;

      // For unsigned multiplication, must zero extend the lhs and rhs to not
      // overflow For signed multiplication, must sign extend the lhs and rhs to
      // not overflow
      if (opcode == AArch64::UMULHrr) {
        lhs_extended = add_instr<IR::ConversionOp>(*i128, next_name(), *mul_lhs,
                                                   IR::ConversionOp::ZExt);
        rhs_extended = add_instr<IR::ConversionOp>(*i128, next_name(), *mul_rhs,
                                                   IR::ConversionOp::ZExt);
      } else {
        lhs_extended = add_instr<IR::ConversionOp>(*i128, next_name(), *mul_lhs,
                                                   IR::ConversionOp::SExt);
        rhs_extended = add_instr<IR::ConversionOp>(*i128, next_name(), *mul_rhs,
                                                   IR::ConversionOp::SExt);
      }

      auto mul = add_instr<IR::BinOp>(*i128, next_name(), *lhs_extended,
                                      *rhs_extended, IR::BinOp::Mul);
      // After multiplying, shift down 64 bits to get the top half of the i128
      // into the bottom half
      auto shift = add_instr<IR::BinOp>(
          *i128, next_name(), *mul, *make_intconst(64, 128), IR::BinOp::LShr);

      // Truncate to the proper size:
      auto trunc = add_instr<IR::ConversionOp>(*i64, next_name(), *shift,
                                               IR::ConversionOp::Trunc);
      store(*trunc);
      break;
    }
    case AArch64::MSUBWrrr:
    case AArch64::MSUBXrrr: {
      auto mul_lhs = get_value(1, 0);
      auto mul_rhs = get_value(2, 0);
      auto minuend = get_value(3, 0);

      auto mul = add_instr<IR::BinOp>(*ty, next_name(), *mul_lhs, *mul_rhs,
                                      IR::BinOp::Mul);
      auto sub = add_instr<IR::BinOp>(*ty, next_name(), *minuend, *mul,
                                      IR::BinOp::Sub);
      store(*sub);
      break;
    }
    case AArch64::SBFMWri:
    case AArch64::SBFMXri: {
      auto src = get_value(1);
      auto immr = mc_inst.getOperand(2).getImm();
      auto imms = mc_inst.getOperand(3).getImm();

      auto r = make_intconst(immr, size);
      //      auto s = make_intconst(imms, size);

      // arithmetic shift right (ASR) alias is perferred when:
      // imms == 011111 and size == 32 or when imms == 111111 and size = 64
      if ((size == 32 && imms == 31) || (size == 64 && imms == 63)) {
        auto dst =
            add_instr<IR::BinOp>(*ty, next_name(), *src, *r, IR::BinOp::AShr);
        store(*dst);
        return;
      }

      // SXTB
      if (immr == 0 && imms == 7) {
        auto i8 = &get_int_type(8);
        auto trunc = add_instr<IR::ConversionOp>(*i8, next_name(), *src,
                                                 IR::ConversionOp::Trunc);
        auto dst = add_instr<IR::ConversionOp>(*ty, next_name(), *trunc,
                                               IR::ConversionOp::SExt);
        store(*dst);
        return;
      }

      // SXTH
      if (immr == 0 && imms == 15) {
        auto i8 = &get_int_type(16);
        auto trunc = add_instr<IR::ConversionOp>(*i8, next_name(), *src,
                                                 IR::ConversionOp::Trunc);
        auto dst = add_instr<IR::ConversionOp>(*ty, next_name(), *trunc,
                                               IR::ConversionOp::SExt);
        store(*dst);
        return;
      }

      // SXTW
      if (immr == 0 && imms == 31) {
        auto i8 = &get_int_type(32);
        auto trunc = add_instr<IR::ConversionOp>(*i8, next_name(), *src,
                                                 IR::ConversionOp::Trunc);
        auto dst = add_instr<IR::ConversionOp>(*ty, next_name(), *trunc,
                                               IR::ConversionOp::SExt);
        store(*dst);
        return;
      }

      // SBFIZ
      if (imms < immr) {

        auto pos = size - immr;
        auto width = imms + 1;
        auto mask = ((uint64_t)1 << (width)) - 1;
        auto bitfield_mask = (uint64_t)1 << (width - 1);

        auto masked = add_instr<IR::BinOp>(
            *ty, next_name(), *src, *make_intconst(mask, size), IR::BinOp::And);

        auto bitfield_lsb = add_instr<IR::BinOp>(
            *ty, next_name(), *src, *make_intconst(bitfield_mask, size),
            IR::BinOp::And);

        auto insert_ones =
            add_instr<IR::BinOp>(*ty, next_name(), *masked,
                                 *make_intconst(~mask, size), IR::BinOp::Or);

        auto cond_ty = &get_int_type(1);

        auto bitfield_lsb_set =
            add_instr<IR::ICmp>(*cond_ty, next_name(), IR::ICmp::NE,
                                *bitfield_lsb, *make_intconst(0, size));

        auto res = add_instr<IR::Select>(*ty, next_name(), *bitfield_lsb_set,
                                         *insert_ones, *masked);
        auto shifted_res = add_instr<IR::BinOp>(
            *ty, next_name(), *res, *make_intconst(pos, size), IR::BinOp::Shl);
        store(*shifted_res);
        return;
      }
      // FIXME: this requires checking if SBFX is preferred.
      // For now, assume this is always SBFX
      auto width = imms + 1;
      auto mask = ((uint64_t)1 << (width)) - 1;
      auto pos = immr;

      auto masked = add_instr<IR::BinOp>(
          *ty, next_name(), *src, *make_intconst(mask, size), IR::BinOp::And);
      auto l_shifted = add_instr<IR::BinOp>(*ty, next_name(), *masked,
                                            *make_intconst(size - width, size),
                                            IR::BinOp::Shl);
      auto shifted_res = add_instr<IR::BinOp>(
          *ty, next_name(), *l_shifted,
          *make_intconst(size - width + pos, size), IR::BinOp::AShr);
      store(*shifted_res);
      return;
    }
    case AArch64::CCMPWi:
    case AArch64::CCMPWr:
    case AArch64::CCMPXi:
    case AArch64::CCMPXr: {
      assert(mc_inst.getNumOperands() == 4);

      auto lhs = get_value(0);
      auto imm_rhs = get_value(1);

      if (!ty || !lhs || !imm_rhs)
        visitError(I);

      auto imm_flags = mc_inst.getOperand(2).getImm();
      auto imm_v_val = make_intconst((imm_flags & 1) ? 1 : 0, 1);
      auto imm_c_val = make_intconst((imm_flags & 2) ? 1 : 0, 1);
      auto imm_z_val = make_intconst((imm_flags & 4) ? 1 : 0, 1);
      auto imm_n_val = make_intconst((imm_flags & 8) ? 1 : 0, 1);

      auto cond_val_imm = mc_inst.getOperand(3).getImm();
      auto cond_val = evaluate_condition(cond_val_imm, MCBB);

      auto ty_ptr = sadd_overflow_type(mc_inst.getOperand(0), size);

      auto ssub = add_instr<IR::BinOp>(*ty_ptr, next_name(), *lhs, *imm_rhs,
                                       IR::BinOp::SSub_Overflow);
      auto result = add_instr<IR::ExtractValue>(*ty, next_name(), *ssub);
      result->addIdx(0);
      auto ty_i1 = &get_int_type(1);
      auto zero_val = make_intconst(0, result->bits());

      auto new_n = add_instr<IR::ICmp>(*ty_i1, next_name(), IR::ICmp::SLT,
                                       *result, *zero_val);
      auto new_z = add_instr<IR::ICmp>(*ty_i1, next_name(), IR::ICmp::EQ, *lhs,
                                       *imm_rhs);

      auto new_c = add_instr<IR::ICmp>(*ty_i1, next_name(), IR::ICmp::UGE, *lhs,
                                       *imm_rhs);

      auto new_v = add_instr<IR::ExtractValue>(*ty_i1, next_name(), *ssub);
      new_v->addIdx(1);

      auto new_n_flag = add_instr<IR::Select>(*ty_i1, next_name(), *cond_val,
                                              *new_n, *imm_n_val);

      auto new_z_flag = add_instr<IR::Select>(*ty_i1, next_name(), *cond_val,
                                              *new_z, *imm_z_val);

      auto new_c_flag = add_instr<IR::Select>(*ty_i1, next_name(), *cond_val,
                                              *new_c, *imm_c_val);

      auto new_v_flag = add_instr<IR::Select>(*ty_i1, next_name(), *cond_val,
                                              *new_v, *imm_v_val);
      store(*new_n_flag);
      cur_ns[MCBB] = new_n_flag;
      store(*new_z_flag);
      cur_zs[MCBB] = new_z_flag;
      store(*new_c_flag);
      cur_cs[MCBB] = new_c_flag;
      store(*new_v_flag);
      cur_vs[MCBB] = new_v_flag;
      break;
    }
    case AArch64::EORWri:
    case AArch64::EORXri: {
      assert(mc_inst.getNumOperands() == 3); // dst, src, imm
      assert(mc_inst.getOperand(1).isReg() && mc_inst.getOperand(2).isImm());

      auto a = get_value(1);
      auto decoded_immediate =
          decodeLogicalImmediate(mc_inst.getOperand(2).getImm(), size);
      auto imm_val = make_intconst(decoded_immediate,
                                   size); // FIXME, need to decode immediate val
      if (!ty || !a || !imm_val)
        visitError(I);

      auto res =
          add_instr<IR::BinOp>(*ty, next_name(), *a, *imm_val, IR::BinOp::Xor);
      store(*res);
      break;
    }
    case AArch64::EORWrs:
    case AArch64::EORXrs: {
      auto lhs = get_value(1);
      auto rhs = get_value(2, mc_inst.getOperand(3).getImm());

      auto result =
          add_instr<IR::BinOp>(*ty, next_name(), *lhs, *rhs, IR::BinOp::Xor);
      store(*result);
      break;
    }
    case AArch64::CSINVWr:
    case AArch64::CSINVXr:
    case AArch64::CSNEGWr:
    case AArch64::CSNEGXr: {
      // csinv dst, a, b, cond
      // if (cond) a else ~b
      assert(mc_inst.getNumOperands() == 4); // dst, lhs, rhs, cond
      // TODO decode condition and find the approprate cond val
      assert(mc_inst.getOperand(1).isReg() && mc_inst.getOperand(2).isReg());
      assert(mc_inst.getOperand(3).isImm());

      auto a = get_value(1);
      auto b = get_value(2);

      auto cond_val_imm = mc_inst.getOperand(3).getImm();
      auto cond_val = evaluate_condition(cond_val_imm, MCBB);

      if (!ty || !a || !b)
        visitError(I);

      auto neg_one = make_intconst(-1, size);
      auto inverted_b =
          add_instr<IR::BinOp>(*ty, next_name(), *b, *neg_one, IR::BinOp::Xor);

      if (opcode == AArch64::CSNEGWr || opcode == AArch64::CSNEGXr) {
        auto negated_b =
            add_instr<IR::BinOp>(*ty, next_name(), *inverted_b,
                                 *make_intconst(1, size), IR::BinOp::Add);
        auto ret =
            add_instr<IR::Select>(*ty, next_name(), *cond_val, *a, *negated_b);
        store(*ret);
        break;
      }

      auto ret =
          add_instr<IR::Select>(*ty, next_name(), *cond_val, *a, *inverted_b);
      store(*ret);
      break;
    }
    case AArch64::CSINCWr:
    case AArch64::CSINCXr: {
      assert(mc_inst.getOperand(1).isReg() && mc_inst.getOperand(2).isReg());
      assert(mc_inst.getOperand(3).isImm());

      auto a = get_value(1);
      auto b = get_value(2);

      auto cond_val_imm = mc_inst.getOperand(3).getImm();
      auto cond_val = evaluate_condition(cond_val_imm, MCBB);

      auto inc = add_instr<IR::BinOp>(
          *ty, next_name(), *b, *make_intconst(1, ty->bits()), IR::BinOp::Add);
      auto sel = add_instr<IR::Select>(*ty, next_name(), *cond_val, *a, *inc);

      store(*sel);
      break;
    }
    case AArch64::MOVZWi:
    case AArch64::MOVZXi: {
      assert(mc_inst.getOperand(0).isReg());
      assert(mc_inst.getOperand(1).isImm());

      auto lhs = get_value(1, mc_inst.getOperand(2).getImm());

      auto rhs = make_intconst(0, size);
      auto ident =
          add_instr<IR::BinOp>(*ty, next_name(), *lhs, *rhs, IR::BinOp::Add);

      store(*ident);
      break;
    }
    case AArch64::MOVNWi:
    case AArch64::MOVNXi: {
      assert(mc_inst.getOperand(0).isReg());
      assert(mc_inst.getOperand(1).isImm());
      assert(mc_inst.getOperand(2).isImm());

      auto lhs = get_value(1, mc_inst.getOperand(2).getImm());

      auto neg_one = make_intconst(-1, size);
      auto not_lhs = add_instr<IR::BinOp>(*ty, next_name(), *lhs, *neg_one,
                                          IR::BinOp::Xor);

      store(*not_lhs);
      break;
    }
    case AArch64::LSLVWr:
    case AArch64::LSLVXr: {

      auto zero = make_intconst(0, size);
      auto lhs = get_value(1);
      auto rhs = get_value(2);

      auto exp = add_instr<IR::TernaryOp>(*ty, next_name(), *lhs, *zero, *rhs,
                                          IR::TernaryOp::FShl);
      store(*exp);
      break;
    }
    case AArch64::LSRVWr:
    case AArch64::LSRVXr: {

      auto zero = make_intconst(0, size);
      auto lhs = get_value(1);
      auto rhs = get_value(2);

      auto exp = add_instr<IR::TernaryOp>(*ty, next_name(), *zero, *lhs, *rhs,
                                          IR::TernaryOp::FShr);
      store(*exp);
      break;
    }
    case AArch64::ORNWrs:
    case AArch64::ORNXrs: {
      auto lhs = get_value(1);
      auto rhs = get_value(2, mc_inst.getOperand(3).getImm());

      auto neg_one = make_intconst(-1, size);
      auto not_rhs = add_instr<IR::BinOp>(*ty, next_name(), *rhs, *neg_one,
                                          IR::BinOp::Xor);

      auto ident =
          add_instr<IR::BinOp>(*ty, next_name(), *lhs, *not_rhs, IR::BinOp::Or);
      store(*ident);
      break;
    }
    case AArch64::MOVKWi:
    case AArch64::MOVKXi: {
      auto dest = get_value(1);
      auto lhs = get_value(2, mc_inst.getOperand(3).getImm());

      uint64_t bitmask;
      auto shift_amt = mc_inst.getOperand(3).getImm();

      if (opcode == AArch64::MOVKWi) {
        assert(shift_amt == 0 || shift_amt == 16);
        bitmask = (shift_amt == 0) ? 0xffff0000 : 0xffff;
      } else {
        assert(shift_amt == 0 || shift_amt == 16 || shift_amt == 32 ||
               shift_amt == 48);
        bitmask = ~(((uint64_t)0xffff) << shift_amt);
      }

      auto bottom_bits = make_intconst(bitmask, size);
      auto cleared = add_instr<IR::BinOp>(*ty, next_name(), *dest, *bottom_bits,
                                          IR::BinOp::And);

      auto ident =
          add_instr<IR::BinOp>(*ty, next_name(), *cleared, *lhs, IR::BinOp::Or);
      store(*ident);
      break;
    }
    case AArch64::UBFMWri:
    case AArch64::UBFMXri: {
      auto src = get_value(1);
      auto immr = mc_inst.getOperand(2).getImm();
      auto imms = mc_inst.getOperand(3).getImm();

      // LSL is preferred when imms != 31 and imms + 1 == immr
      if (size == 32 && imms != 31 && imms + 1 == immr) {
        auto dst = add_instr<IR::BinOp>(*ty, next_name(), *src,
                                        *make_intconst(31 - imms, size),
                                        IR::BinOp::Shl);
        store(*dst);
        return;
      }

      // LSL is preferred when imms != 63 and imms + 1 == immr
      if (size == 64 && imms != 63 && imms + 1 == immr) {
        auto dst = add_instr<IR::BinOp>(*ty, next_name(), *src,
                                        *make_intconst(63 - imms, size),
                                        IR::BinOp::Shl);
        store(*dst);
        return;
      }

      // LSR is preferred when imms == 31 or 63 (size - 1)
      if (imms == size - 1) {
        auto dst =
            add_instr<IR::BinOp>(*ty, next_name(), *src,
                                 *make_intconst(immr, size), IR::BinOp::LShr);
        store(*dst);
        return;
      }

      // UBFIZ
      if (imms < immr) {
        auto pos = size - immr;
        auto width = imms + 1;
        auto mask = ((uint64_t)1 << (width)) - 1;
        auto masked = add_instr<IR::BinOp>(
            *ty, next_name(), *src, *make_intconst(mask, size), IR::BinOp::And);
        auto shifted =
            add_instr<IR::BinOp>(*ty, next_name(), *masked,
                                 *make_intconst(pos, size), IR::BinOp::Shl);
        store(*shifted);
        return;
      }

      // UXTB
      if (immr == 0 && imms == 7) {
        auto mask = ((uint64_t)1 << 8) - 1;
        auto masked = add_instr<IR::BinOp>(
            *ty, next_name(), *src, *make_intconst(mask, size), IR::BinOp::And);
        auto zexted = add_instr<IR::ConversionOp>(*ty, next_name(), *masked,
                                                  IR::ConversionOp::ZExt);
        store(*zexted);
        // assert(false && "UXTB not supported");
        return;
      }
      // UXTH
      if (immr == 0 && imms == 15) {
        assert(false && "UXTH not supported");
        return;
      }

      // UBFX
      // FIXME: this requires checking if UBFX is preferred.
      // For now, assume this is always UBFX
      // we mask from lsb to lsb + width and then perform a logical shift right
      auto width = imms + 1;
      auto mask = ((uint64_t)1 << (width)) - 1;
      auto pos = immr;

      auto masked = add_instr<IR::BinOp>(
          *ty, next_name(), *src, *make_intconst(mask, size), IR::BinOp::And);
      auto shifted_res =
          add_instr<IR::BinOp>(*ty, next_name(), *masked,
                               *make_intconst(pos, size), IR::BinOp::LShr);
      store(*shifted_res);
      return;
      // assert(false && "UBFX not supported");
    }
    case AArch64::BFMWri:
    case AArch64::BFMXri: {
      auto dst = get_value(1);
      auto src = get_value(2);

      auto immr = mc_inst.getOperand(3).getImm();
      auto imms = mc_inst.getOperand(4).getImm();

      if (imms >= immr) {
        auto bits = (imms - immr + 1);
        auto pos = immr;

        auto mask = (((uint64_t)1 << bits) - 1) << pos;

        auto masked = add_instr<IR::BinOp>(
            *ty, next_name(), *src, *make_intconst(mask, size), IR::BinOp::And);
        auto shifted =
            add_instr<IR::BinOp>(*ty, next_name(), *masked,
                                 *make_intconst(pos, size), IR::BinOp::LShr);

        auto cleared = add_instr<IR::BinOp>(
            *ty, next_name(), *dst,
            *make_intconst((uint64_t)(-1) << bits, size), IR::BinOp::And);

        auto res = add_instr<IR::BinOp>(*ty, next_name(), *cleared, *shifted,
                                        IR::BinOp::Or);
        store(*res);
        return;
      }

      auto bits = imms + 1;
      auto pos = size - immr;

      // This mask deletes `bits` number of bits starting at `pos`.
      // If the mask is for a 32 bit value, it will chop off the top 32 bits of
      // the 64 bit mask to keep the mask to a size of 32 bits
      auto mask =
          ~((((uint64_t)1 << bits) - 1) << pos) & ((uint64_t)-1 >> (64 - size));

      // get `bits` number of bits from the least significant bits
      auto bitfield = add_instr<IR::BinOp>(
          *ty, next_name(), *src, *make_intconst(~((uint64_t)-1 << bits), size),
          IR::BinOp::And);

      // move the bitfield into position
      auto moved =
          add_instr<IR::BinOp>(*ty, next_name(), *bitfield,
                               *make_intconst(pos, size), IR::BinOp::Shl);

      // carve out a place for the bitfield
      auto masked = add_instr<IR::BinOp>(
          *ty, next_name(), *dst, *make_intconst(mask, size), IR::BinOp::And);
      // place the bitfield
      auto res = add_instr<IR::BinOp>(*ty, next_name(), *masked, *moved,
                                      IR::BinOp::Or);
      store(*res);
      return;
    }
    case AArch64::ORRWri:
    case AArch64::ORRXri: {
      auto lhs = get_value(1);

      auto imm = mc_inst.getOperand(2).getImm();
      auto decoded = decodeLogicalImmediate(imm, size);

      auto result = add_instr<IR::BinOp>(
          *ty, next_name(), *lhs, *make_intconst(decoded, size), IR::BinOp::Or);
      store(*result);
      break;
    }
    case AArch64::ORRWrs:
    case AArch64::ORRXrs: {
      auto lhs = get_value(1);
      auto rhs = get_value(2, mc_inst.getOperand(3).getImm());

      auto result =
          add_instr<IR::BinOp>(*ty, next_name(), *lhs, *rhs, IR::BinOp::Or);
      store(*result);
      break;
    }
    case AArch64::SDIVWr:
    case AArch64::SDIVXr: {
      auto lhs = get_value(1);
      auto rhs = get_value(2);

      auto result =
          add_instr<IR::BinOp>(*ty, next_name(), *lhs, *rhs, IR::BinOp::SDiv);
      store(*result);
      break;
    }
    case AArch64::UDIVWr:
    case AArch64::UDIVXr: {
      auto lhs = get_value(1);
      auto rhs = get_value(2);

      auto result =
          add_instr<IR::BinOp>(*ty, next_name(), *lhs, *rhs, IR::BinOp::UDiv);
      store(*result);
      break;
    }
    case AArch64::EXTRWrri:
    case AArch64::EXTRXrri: {
      auto op1 = get_value(1);
      auto op2 = get_value(2);
      auto shift = get_value(3);

      auto result = add_instr<IR::TernaryOp>(*ty, next_name(), *op1, *op2,
                                             *shift, IR::TernaryOp::FShr);
      store(*result);
      break;
    }
    case AArch64::RORVWr:
    case AArch64::RORVXr: {
      auto op = get_value(1);
      auto shift = get_value(2);

      auto result = add_instr<IR::TernaryOp>(*ty, next_name(), *op, *op, *shift,
                                             IR::TernaryOp::FShr);
      store(*result);
      break;
    }
    case AArch64::RBITWr:
    case AArch64::RBITXr: {
      auto op = get_value(1);

      auto result = add_instr<IR::UnaryOp>(*ty, next_name(), *op,
                                           IR::UnaryOp::BitReverse);
      store(*result);
      break;
    }
    case AArch64::REVWr:
    case AArch64::REVXr: {
      auto op = get_value(1);

      auto result =
          add_instr<IR::UnaryOp>(*ty, next_name(), *op, IR::UnaryOp::BSwap);
      store(*result);
      break;
    }
    case AArch64::CLZWr:
    case AArch64::CLZXr: {
      auto op = get_value(1);

      auto result = add_instr<IR::BinOp>(*ty, next_name(), *op,
                                         *make_intconst(0, 1), IR::BinOp::Ctlz);

      store(*result);
      break;
    }
    case AArch64::EONWrs:
    case AArch64::EONXrs:
    case AArch64::BICWrs:
    case AArch64::BICXrs: {
      // BIC:
      // return = op1 AND NOT (optional shift) op2
      // EON:
      // return = op1 XOR NOT (optional shift) op2

      auto op1 = get_value(1);
      auto op2 = get_value(2);

      // If there is a shift to be performed on the second operand
      if (mc_inst.getNumOperands() == 4) {
        // the 4th operand (if it exists) must b an immediate
        assert(mc_inst.getOperand(3).isImm());
        op2 = reg_shift(op2, mc_inst.getOperand(3).getImm());
      }

      // Perform NOT
      auto neg_one = make_intconst(-1, size);
      auto inverted_op2 = add_instr<IR::BinOp>(*ty, next_name(), *op2, *neg_one,
                                               IR::BinOp::Xor);

      // Perform final Op: AND for BIC, XOR for EON
      IR::BinOp::Op finalBinOp;
      switch (opcode) {
      case AArch64::BICWrs:
      case AArch64::BICXrs: {
        finalBinOp = IR::BinOp::And;
        break;
      }
      case AArch64::EONWrs:
      case AArch64::EONXrs: {
        finalBinOp = IR::BinOp::Xor;
        break;
      }
      }

      auto ret = add_instr<IR::BinOp>(*ty, next_name(), *op1, *inverted_op2,
                                      finalBinOp);
      store(*ret);
      break;
    }
    case AArch64::REV16Xr: {
      // REV16Xr: Reverse bytes of 64 bit value in 16-bit half-words.
      auto val = get_value(1);

      auto first_part = add_instr<IR::BinOp>(
          *ty, next_name(), *val, *make_intconst(8, size), IR::BinOp::Shl);
      auto first_part_and = add_instr<IR::BinOp>(
          *ty, next_name(), *first_part,
          *make_intconst(0xFF00FF00FF00FF00, size), IR::BinOp::And);

      auto second_part = add_instr<IR::BinOp>(
          *ty, next_name(), *val, *make_intconst(8, size), IR::BinOp::LShr);
      auto second_part_and = add_instr<IR::BinOp>(
          *ty, next_name(), *second_part,
          *make_intconst(0x00FF00FF00FF00FF, size), IR::BinOp::And);

      auto combined_val = add_instr<IR::BinOp>(
          *ty, next_name(), *first_part_and, *second_part_and, IR::BinOp::Or);

      store(*combined_val);
      break;
    }
    case AArch64::REV16Wr:
    case AArch64::REV32Xr: {
      // REV16Wr: Reverse bytes of 32 bit value in 16-bit half-words.
      // REV32Xr: Reverse bytes of 64 bit value in 32-bit words.
      auto val = get_value(1);

      // Reversing all of the bytes, then performing a rotation by half the
      // width reverses bytes in 16-bit halfwords for a 32 bit int and reverses
      // bytes in a 32-bit word for a 64 bit int
      auto reverse_val =
          add_instr<IR::UnaryOp>(*ty, next_name(), *val, IR::UnaryOp::BSwap);

      auto ret = add_instr<IR::TernaryOp>(
          *ty, next_name(), *reverse_val, *reverse_val,
          *make_intconst(size / 2, size), IR::TernaryOp::FShr);

      store(*ret);
      break;
    }
    case AArch64::RET: {
      // for now we're assuming that the function returns an integer or void
      // value
      if (!ret_void) {
        auto retTyp = &get_int_type(srcFn->getType().bits());

        // unsigned latest_id = 0;
        // Hacky way to find the latest use of the return value
        // FIXME: this will not work in a setting with multiple blocks
        // for (auto &[reg_pair, _] : cache) {
        //   if (reg_pair.first == mc_inst.getOperand(0).getReg())
        //     latest_id =
        //         latest_id < reg_pair.second ? reg_pair.second : latest_id;
        // }
        auto val = getIdentifier(mc_inst.getOperand(0).getReg(), I.getOpId(0));

        if (val == nullptr) {
          cout << "null val" << endl;
        }
        if (val) {
          if (retTyp->bits() < val->bits()) {
            auto trunc = add_instr<IR::ConversionOp>(*retTyp, next_name(), *val,
                                                     IR::ConversionOp::Trunc);
            val = trunc;
          }

          // for don't care bits we need to mask them off before returning
          if (has_ret_attr && (original_ret_bitwidth < 32)) {
            assert(retTyp->bits() >= original_ret_bitwidth);
            assert(retTyp->bits() == 64);
            auto trunc = add_instr<IR::ConversionOp>(
                get_int_type(32), next_name(), *val, IR::ConversionOp::Trunc);
            val = add_instr<IR::ConversionOp>(get_int_type(64), next_name(),
                                              *trunc, IR::ConversionOp::ZExt);
          }
          add_instr<IR::Return>(*retTyp, *val);
        } else { // Hacky solution to deal with functions where the assembly is
                 // just a ret instruction
          add_instr<IR::Return>(*retTyp, *make_intconst(0, retTyp->bits()));
        }
      } else {
        add_instr<IR::Return>(IR::Type::voidTy, IR::Value::voidVal);
      }

      break;
    }
    case AArch64::B: {
      const auto &op = mc_inst.getOperand(0);
      if (op.isImm()) { // handles the case when we add an entry block with no
                        // predecessors
        auto &dst_name = MF.BBs[mc_inst.getOperand(0).getImm()].getName();
        auto &dst = Fn.getBB(dst_name);
        add_instr<IR::Branch>(dst);
        break;
      }

      auto dst_ptr = get_basic_block(Fn, mc_inst.getOperand(0));
      add_instr<IR::Branch>(*dst_ptr);
      break;
    }
    case AArch64::Bcc: {
      auto cond_val_imm = mc_inst.getOperand(0).getImm();
      auto cond_val = evaluate_condition(cond_val_imm, MCBB);

      auto &jmp_tgt_op = mc_inst.getOperand(1);
      assert(jmp_tgt_op.isExpr() && "expected expression");
      assert((jmp_tgt_op.getExpr()->getKind() == MCExpr::ExprKind::SymbolRef) &&
             "expected symbol ref as bcc operand");
      const MCSymbolRefExpr &SRE = cast<MCSymbolRefExpr>(*jmp_tgt_op.getExpr());
      const MCSymbol &Sym = SRE.getSymbol();
      cout << "bcc target: " << Sym.getName().str() << '\n';
      auto &dst_true = Fn.getBB(Sym.getName());

      assert(MCBB->getSuccs().size() == 2 && "expected 2 successors");
      const std::string *dst_false_name;
      for (auto &succ : MCBB->getSuccs()) {
        if (succ->getName() != Sym.getName()) {
          dst_false_name = &succ->getName();
          break;
        }
      }
      auto &dst_false = Fn.getBB(*dst_false_name);

      add_instr<IR::Branch>(*cond_val, dst_true, dst_false);
      break;
    }
    case AArch64::CBZW:
    case AArch64::CBZX: {
      auto operand = get_value(0);
      assert(operand != nullptr && "operand is null");
      auto cond_val =
          add_instr<IR::ICmp>(get_int_type(1), next_name(), IR::ICmp::EQ,
                              *operand, *make_intconst(0, operand->bits()));

      auto dst_true = get_basic_block(Fn, mc_inst.getOperand(1));
      assert(MCBB->getSuccs().size() == 2 && "expected 2 successors");

      const std::string *dst_false_name;
      for (auto &succ : MCBB->getSuccs()) {
        if (succ->getName() != dst_true->getName()) {
          dst_false_name = &succ->getName();
          break;
        }
      }
      auto &dst_false = Fn.getBB(*dst_false_name);
      add_instr<IR::Branch>(*cond_val, *dst_true, dst_false);
      break;
    }
    case AArch64::CBNZW:
    case AArch64::CBNZX: {
      auto operand = get_value(0);
      assert(operand != nullptr && "operand is null");
      auto cond_val =
          add_instr<IR::ICmp>(get_int_type(1), next_name(), IR::ICmp::NE,
                              *operand, *make_intconst(0, operand->bits()));

      auto dst_true = get_basic_block(Fn, mc_inst.getOperand(1));
      assert(MCBB->getSuccs().size() == 2 && "expected 2 successors");

      const std::string *dst_false_name;
      for (auto &succ : MCBB->getSuccs()) {
        if (succ->getName() != dst_true->getName()) {
          dst_false_name = &succ->getName();
          break;
        }
      }
      auto &dst_false = Fn.getBB(*dst_false_name);
      add_instr<IR::Branch>(*cond_val, *dst_true, dst_false);
      break;
    }
    case AArch64::TBZW:
    case AArch64::TBZX:
    case AArch64::TBNZW:
    case AArch64::TBNZX: {
      auto operand = get_value(0);
      assert(operand != nullptr && "operand is null");
      auto bit_pos = mc_inst.getOperand(1).getImm();
      auto shift =
          add_instr<IR::BinOp>(*ty, next_name(), *operand,
                               *make_intconst(bit_pos, size), IR::BinOp::LShr);
      auto cond_val = add_instr<IR::ConversionOp>(
          get_int_type(1), next_name(), *shift, IR::ConversionOp::Trunc);

      auto &jmp_tgt_op = mc_inst.getOperand(2);
      assert(jmp_tgt_op.isExpr() && "expected expression");
      assert((jmp_tgt_op.getExpr()->getKind() == MCExpr::ExprKind::SymbolRef) &&
             "expected symbol ref as bcc operand");
      const MCSymbolRefExpr &SRE = cast<MCSymbolRefExpr>(*jmp_tgt_op.getExpr());
      const MCSymbol &Sym =
          SRE.getSymbol(); // FIXME refactor this into a function
      auto &dst_false = Fn.getBB(Sym.getName());

      cout << "current mcblock = " << MCBB->getName() << endl;
      cout << "BB=" << BB->getName() << endl;
      cout << "jump target = " << Sym.getName().str() << endl;
      assert(MCBB->getSuccs().size() == 2 && "expected 2 successors");

      const std::string *dst_true_name;
      for (auto &succ : MCBB->getSuccs()) {
        if (succ->getName() != Sym.getName()) {
          dst_true_name = &succ->getName();
          break;
        }
      }
      auto &dst_true = Fn.getBB(*dst_true_name);

      switch (opcode) {
      case AArch64::TBNZW:
      case AArch64::TBNZX: {
        add_instr<IR::Branch>(*cond_val, dst_false, dst_true);
        break;
      }
      default:
        add_instr<IR::Branch>(*cond_val, dst_true, dst_false);
      }

      break;
    }
    case AArch64::PHI: {
      auto result = add_instr<IR::Phi>(*ty, next_name());
      cout << "pushing phi in todo : " << endl;
      wrapper->print();
      lift_todo_phis.emplace_back(result, wrapper);
      store(*result);
      break;
    }
    default:
      Fn.print(
          cout << "\nError "
                  "detected----------partially-lifted-arm-target----------\n");
      visitError(I);
    }
  }

  std::optional<IR::Function> run() {
    if (&srcFn->getType() == &IR::Type::voidTy) {
      cout << "function is void type\n";
      ret_void = true;
    } else if (!srcFn->getType().isIntType())
      report_fatal_error("Only int/void return types supported for now");

    IR::Type *func_return_type{nullptr};
    if (ret_void) {
      func_return_type = &IR::Type::voidTy;
    } else {
      // TODO: Will need to handle other return types here later
      func_return_type = &get_int_type(srcFn->getType().bits());
    }

    if (!func_return_type)
      return {};

    IR::Function Fn(*func_return_type, MF.getName());
    reset_state(Fn);

    // set function attribute to include noundef
    // Fn.getFnAttrs().set(IR::FnAttrs::NoUndef);
    // TODO need to disable poison values as well. Figure how to do so

    // FIXME infer function attributes if any
    // Most likely need to emit and read the debug info from the MCStreamer

    // Create Fn's BBs
    vector<pair<IR::BasicBlock *, MCBasicBlock *>> sorted_bbs;
    {
      util::edgesTy edges;
      vector<MCBasicBlock *> bbs;
      unordered_map<MCBasicBlock *, unsigned> bb_map;

      auto bb_num = [&](MCBasicBlock *bb) {
        auto [I, inserted] = bb_map.emplace(bb, bbs.size());
        if (inserted) {
          bbs.emplace_back(bb);
          edges.emplace_back();
        }
        return I->second;
      };

      for (auto &bb : MF.BBs) {
        auto n = bb_num(&bb);
        for (auto it = bb.succBegin(); it != bb.succEnd(); ++it) {
          auto succ_ptr = *it;
          auto n_dst = bb_num(succ_ptr);
          edges[n].emplace(n_dst);
        }
      }

      for (auto v : top_sort(edges)) {
        sorted_bbs.emplace_back(&Fn.getBB(bbs[v]->getName()), bbs[v]);
      }
    }

    // setup BB so subsequent add_instr calls work
    BB = sorted_bbs[0].first;
    unsigned argNum = 0;
    unsigned idx = 0;
    for (auto &v : srcFn->getInputs()) {
      auto &typ = v.getType();

      auto input_ptr = dynamic_cast<const IR::Input *>(&v);
      assert(input_ptr);
      // generate names and values for the input arguments
      // FIXME this is pretty convulated and needs to be cleaned up
      auto operand = MCOperand::createReg(AArch64::X0 + (argNum));

      std::stringstream ss;
      ss << "\%" << registerInfo->getName(operand.getReg());
      IR::ParamAttrs attrs(input_ptr->getAttributes());

      auto val = make_unique<IR::Input>(typ, ss.str(), move(attrs));
      IR::Value *stored = val.get();

      stored =
          add_instr<IR::Freeze>(typ, next_name(operand.getReg(), 1), *stored);
      mc_add_identifier(operand.getReg(), 1, *stored);
      assert(typ.bits() == 64 && "at this point input type should be 64 bits");

      // if (typ.bits() < 64) {

      //  auto extended_type = &get_int_type(64);
      //  if (input_ptr->getAttributes().has(IR::ParamAttrs::Sext))
      //    stored = add_instr<IR::ConversionOp>(*extended_type,
      //                                         next_name(operand.getReg(), 2),
      //                                         *stored,
      //                                         IR::ConversionOp::SExt);
      //  else
      //    stored = add_instr<IR::ConversionOp>(*extended_type,
      //                                         next_name(operand.getReg(), 2),
      //                                         *stored,
      //                                         IR::ConversionOp::ZExt);
      //}
      if (!new_input_idx_bitwidth.empty() &&
          (argNum == new_input_idx_bitwidth[idx].first)) {
        IR::ConversionOp::Op op(IR::ConversionOp::ZExt);

        if (input_ptr->getAttributes().has(IR::ParamAttrs::Sext)) {
          op = IR::ConversionOp::SExt;
        }

        auto truncated_type = &get_int_type(new_input_idx_bitwidth[idx].second);
        stored = add_instr<IR::ConversionOp>(*truncated_type,
                                             next_name(operand.getReg(), 2),
                                             *stored, IR::ConversionOp::Trunc);
        auto extended_type = &get_int_type(64);
        if (truncated_type->bits() == 1) {
          cout << "encounterd 1 bit input\n";
          stored = add_instr<IR::ConversionOp>(get_int_type(8),
                                               next_name(operand.getReg(), 3),
                                               *stored, IR::ConversionOp::ZExt);
          stored = add_instr<IR::ConversionOp>(
              *extended_type, next_name(operand.getReg(), 4), *stored, op);
        } else {
          if (truncated_type->bits() < 32) {
            stored = add_instr<IR::ConversionOp>(
                get_int_type(32), next_name(operand.getReg(), 3), *stored, op);
            stored = add_instr<IR::ConversionOp>(
                *extended_type, next_name(operand.getReg(), 4), *stored,
                IR::ConversionOp::ZExt);
          } else {
            stored = add_instr<IR::ConversionOp>(
                *extended_type, next_name(operand.getReg(), 4), *stored, op);
          }
        }

        idx++;
      }
      instructionCount++;
      mc_add_identifier(operand.getReg(), 2, *stored);
      Fn.addInput(move(val));
      argNum++;
    }

    for (auto &[alive_bb, mc_bb] : sorted_bbs) {
      cout << "visitng bb: " << mc_bb->getName() << endl;
      BB = alive_bb;
      MCBB = mc_bb;
      auto &mc_instrs = mc_bb->getInstrs();

      for (auto &mc_instr : mc_instrs) {
        cout << "before visit\n";
        mc_instr.print();
        mc_visit(mc_instr, Fn);
      }

      auto jump_instr = dynamic_cast<IR::JumpInstr *>(&BB->back());
      auto ret_instr = dynamic_cast<IR::Return *>(&BB->back());
      if (!jump_instr && !ret_instr) {
        cout << "Last basicBlock instruction is not a terminator!\n";
        assert(MCBB->getSuccs().size() == 1 &&
               "expected 1 successor for block with no terminator");
        auto &dst = Fn.getBB(MCBB->getSuccs()[0]->getName());
        add_instr<IR::Branch>(dst);
      }

      blockCount++;
    }

    Fn.print(
        cout << "\n----------lifted-arm-target-missing-phi-params----------\n");
    cout << "lift_todo_phis.size() = " << lift_todo_phis.size() << "\n";

    int tmp_index = 0;
    for (auto &[phi, phi_mc_wrapper] : lift_todo_phis) {
      cout << "index = " << tmp_index
           << "opcode =" << phi_mc_wrapper->getOpcode() << endl;
      tmp_index++;
      add_phi_params(phi, phi_mc_wrapper);
    }
    return move(Fn);
  }
};

// Convert an MCFucntion to IR::Function
// Adapted from llvm2alive_ in llvm2alive.cpp with some simplifying assumptions
// FIXME for now, we are making a lot of simplifying assumptions like assuming
// types of arguments.
std::optional<IR::Function> arm2alive(MCFunction &MF,
                                      std::optional<IR::Function> &srcFn,
                                      MCInstPrinter *instrPrinter,
                                      MCRegisterInfo *registerInfo) {
  return arm2alive_(MF, srcFn, instrPrinter, registerInfo).run();
}

// We're overriding MCStreamerWrapper to generate an MCFunction
// from the arm assembly. MCStreamerWrapper provides callbacks to handle
// different parts of the assembly file. The main callbacks that we're
// using right now are emitInstruction and emitLabel to access the
// instruction and labels in the arm assembly.
//
// FIXME for now, we're using this class to generate the MCFunction and
// also print the MCFunction and to convert the MCFunction into SSA form.
// We should move this implementation somewhere else
// TODO we'll need to implement some of the other callbacks to extract more
// information from the asm file. For example, it would be useful to extract
// debug info to determine the number of function parameters.
class MCStreamerWrapper final : public llvm::MCStreamer {
  enum ASMLine { none = 0, label = 1, non_term_instr = 2, terminator = 3 };

private:
  MCBasicBlock *temp_block{nullptr};
  bool first_label{true};
  unsigned prev_line{0};
  llvm::MCInstrAnalysis *Ana_ptr;
  llvm::MCInstPrinter *IP_ptr;
  llvm::MCRegisterInfo *MRI_ptr;

public:
  MCFunction MF;
  unsigned cnt{0};
  std::vector<llvm::MCInst>
      Insts; // CHECK this should go as it's only being used for pretty printing
             // which makes it unused after fixing MCInstWrapper::print
  using BlockSetTy = llvm::SetVector<MCBasicBlock *>;

  MCStreamerWrapper(llvm::MCContext &Context, llvm::MCInstrAnalysis *_Ana_ptr,
                    llvm::MCInstPrinter *_IP_ptr,
                    llvm::MCRegisterInfo *_MRI_ptr)
      : MCStreamer(Context), Ana_ptr(_Ana_ptr), IP_ptr(_IP_ptr),
        MRI_ptr(_MRI_ptr) {
    MF.Ana_ptr = Ana_ptr;
    MF.IP_ptr = IP_ptr;
    MF.MRI_ptr = MRI_ptr;
  }

  // We only want to intercept the emission of new instructions.
  virtual void
  emitInstruction(const llvm::MCInst &Inst,
                  const llvm::MCSubtargetInfo & /* unused */) override {

    assert(prev_line != ASMLine::none);

    if (prev_line == ASMLine::terminator) {
      temp_block = MF.addBlock(MF.getLabel());
    }
    MCInstWrapper Cur_Inst(Inst);
    temp_block->addInst(Cur_Inst);
    Insts.push_back(Inst);

    if (Ana_ptr->isTerminator(Inst)) {
      prev_line = ASMLine::terminator;
    } else {
      prev_line = ASMLine::non_term_instr;
    }
    auto &inst_ref = Cur_Inst.getMCInst();
    auto num_operands = inst_ref.getNumOperands();
    for (unsigned i = 0; i < num_operands; ++i) {
      auto op = inst_ref.getOperand(i);
      if (op.isExpr()) {
        auto expr = op.getExpr();
        if (expr->getKind() == MCExpr::ExprKind::SymbolRef) {
          const MCSymbolRefExpr &SRE = cast<MCSymbolRefExpr>(*expr);
          const MCSymbol &Sym = SRE.getSymbol();
          errs() << "target label : " << Sym.getName()
                 << ", offset=" << Sym.getOffset()
                 << '\n'; // FIXME remove when done
        }
      }
    }

    errs() << cnt++ << "  : ";
    Inst.dump_pretty(llvm::errs(), IP_ptr, " ", MRI_ptr);
    if (Ana_ptr->isBranch(Inst))
      errs() << ": branch ";
    if (Ana_ptr->isConditionalBranch(Inst))
      errs() << ": conditional branch ";
    if (Ana_ptr->isUnconditionalBranch(Inst))
      errs() << ": unconditional branch ";
    if (Ana_ptr->isTerminator(Inst))
      errs() << ": terminator ";
    errs() << "\n";
  }

  bool emitSymbolAttribute(llvm::MCSymbol *Symbol,
                           llvm::MCSymbolAttr Attribute) override {
    return true;
  }

  void emitCommonSymbol(llvm::MCSymbol *Symbol, uint64_t Size,
                        unsigned ByteAlignment) override {}
  void emitZerofill(llvm::MCSection *Section, llvm::MCSymbol *Symbol = nullptr,
                    uint64_t Size = 0, unsigned ByteAlignment = 0,
                    llvm::SMLoc Loc = llvm::SMLoc()) override {}

  virtual void emitLabel(MCSymbol *Symbol, SMLoc Loc) override {
    // Assuming the first label encountered is the function's name
    // Need to figure out if there is a better way to get access to the
    // function's name
    if (first_label) {
      MF.setName(Symbol->getName().str() + "-tgt");
      first_label = false;
    }
    string cur_label = Symbol->getName().str();
    temp_block = MF.addBlock(cur_label);
    prev_line = ASMLine::label;
    errs() << cnt++ << "  : ";
    errs() << "inside Emit Label: symbol=" << Symbol->getName() << '\n';
  }

  std::string findTargetLabel(MCInst &inst_ref) {
    auto num_operands = inst_ref.getNumOperands();
    for (unsigned i = 0; i < num_operands; ++i) {
      auto op = inst_ref.getOperand(i);
      if (op.isExpr()) {
        auto expr = op.getExpr();
        if (expr->getKind() == MCExpr::ExprKind::SymbolRef) {
          const MCSymbolRefExpr &SRE = cast<MCSymbolRefExpr>(*expr);
          const MCSymbol &Sym = SRE.getSymbol();
          return Sym.getName().str();
        }
      }
    }

    assert(false && "Could not find target label in arm branch instruction");
    UNREACHABLE();
  }

  // Make sure that we have an entry label with no predecessors
  void addEntryBlock() {
    MF.addEntryBlock();
  }

  void postOrderDFS(MCBasicBlock &curBlock, BlockSetTy &visited,
                    std::vector<MCBasicBlock *> &postOrder) {
    visited.insert(&curBlock);
    for (auto succ : curBlock.getSuccs()) {
      if (std::find(visited.begin(), visited.end(), succ) == visited.end()) {
        postOrderDFS(*succ, visited, postOrder);
      }
    }
    postOrder.push_back(&curBlock);
  }

  std::vector<MCBasicBlock *> postOrder() {
    std::vector<MCBasicBlock *> postOrder;
    BlockSetTy visited;
    for (auto &curBlock : MF.BBs) {
      if (visited.count(&curBlock) == 0) {
        postOrderDFS(curBlock, visited, postOrder);
      }
    }
    return postOrder;
  }

  // compute the domination relation
  void generateDominator() {
    MF.generateDominator();
  }

  void generateDominatorFrontier() {
    MF.generateDominatorFrontier();
  }

  void generateDomTree() {
    MF.generateDomTree();
  }

  // compute a map from each variable to its defining block
  void findDefiningBlocks() {
    MF.findDefiningBlocks();
  }

  void findPhis() {
    MF.findPhis();
  }

  // FIXME: this is duplicated code. need to refactor
  void findArgs(std::optional<IR::Function> &src_fn) {
    MF.findArgs(src_fn);
  }

  void rewriteOperands() {
    MF.rewriteOperands();
  }

  void ssaRename() {
    MF.ssaRename();
  }

  // helper function to compute the intersection of predecessor dominator sets
  BlockSetTy intersect(BlockSetTy &preds,
                       std::unordered_map<MCBasicBlock *, BlockSetTy> &dom) {
    BlockSetTy ret;
    if (preds.size() == 0) {
      return ret;
    }
    if (preds.size() == 1) {
      return dom[*preds.begin()];
    }
    ret = dom[*preds.begin()];
    auto second = ++preds.begin();
    for (auto it = second; it != preds.end(); ++it) {
      auto &pred_set = dom[*it];
      BlockSetTy new_ret;
      for (auto &b : ret) {
        if (pred_set.count(b) == 1) {
          new_ret.insert(b);
        }
      }
      ret = new_ret;
    }
    return ret;
  }

  // helper function to invert a graph
  std::unordered_map<MCBasicBlock *, BlockSetTy>
  invertGraph(std::unordered_map<MCBasicBlock *, BlockSetTy> &graph) {
    std::unordered_map<MCBasicBlock *, BlockSetTy> res;
    for (auto &curBlock : graph) {
      for (auto &succ : curBlock.second) {
        res[succ].insert(curBlock.first);
      }
    }
    return res;
  }

  // Debug function to print domination info
  void printGraph(std::unordered_map<MCBasicBlock *, BlockSetTy> &graph) {
    for (auto &curBlock : graph) {
      cout << curBlock.first->getName() << ": ";
      for (auto &dst : curBlock.second) {
        cout << dst->getName() << " ";
      }
      cout << "\n";
    }
  }

  // Only call after MF with Basicblocks is constructed to generate the
  // successors for each basic block
  void generateSuccessors() {
    cout << "generating basic block successors" << '\n';
    for (unsigned i = 0; i < MF.BBs.size(); ++i) {
      auto &cur_bb = MF.BBs[i];
      MCBasicBlock *next_bb_ptr = nullptr;
      if (i < MF.BBs.size() - 1)
        next_bb_ptr = &MF.BBs[i + 1];

      if (cur_bb.size() == 0) {
        cout
            << "generateSuccessors, encountered basic block with 0 instructions"
            << '\n';
        continue;
      }
      auto &last_mc_instr = cur_bb.getInstrs().back().getMCInst();
      // handle the special case of adding where we have added a new entry block
      // with no predecessors. This is hacky because I don't know the API to
      // create and MCExpr and have to create a branch with an immediate operand
      // instead
      if (i == 0 && (Ana_ptr->isUnconditionalBranch(last_mc_instr)) &&
          last_mc_instr.getOperand(0).isImm()) {
        cur_bb.addSucc(next_bb_ptr);
        continue;
      }
      if (Ana_ptr->isConditionalBranch(last_mc_instr)) {
        std::string target = findTargetLabel(last_mc_instr);
        auto target_bb = MF.findBlockByName(target);
        cur_bb.addSucc(target_bb);
        if (next_bb_ptr)
          cur_bb.addSucc(next_bb_ptr);
      } else if (Ana_ptr->isUnconditionalBranch(last_mc_instr)) {
        std::string target = findTargetLabel(last_mc_instr);
        auto target_bb = MF.findBlockByName(target);
        cur_bb.addSucc(target_bb);
      } else if (Ana_ptr->isReturn(last_mc_instr)) {
        continue;
      } else if (next_bb_ptr) { // add edge to next block
        cur_bb.addSucc(next_bb_ptr);
      }
    }
  }

  // Remove empty basic blocks from the machine function
  void removeEmptyBlocks() {
    cout << "removing empty basic blocks" << '\n';
    std::erase_if(MF.BBs, [](MCBasicBlock b) { return b.size() == 0; });
  }

  // Only call after generateSucessors() has been called
  // generate predecessors for each basic block in a MCFunction
  void generatePredecessors() {
    cout << "generating basic block predecessors" << '\n';
    for (auto &block : MF.BBs) {
      for (auto it = block.succBegin(); it != block.succEnd(); ++it) {
        auto successor = *it;
        successor->addPred(&block);
      }
    }
  }

  void printBlocks() {
    cout << "#of Blocks = " << MF.BBs.size() << '\n';
    cout << "-------------\n";
    int i = 0;
    for (auto &block : MF.BBs) {
      errs() << "block " << i << ", name= " << block.getName() << '\n';
      for (auto &inst : block.getInstrs()) {
        inst.getMCInst().dump_pretty(llvm::errs(), IP_ptr, " ", MRI_ptr);
        errs() << '\n';
      }
      i++;
    }
  }

  void printCFG() {
    cout << "printing arm function CFG" << '\n';
    cout << "successors" << '\n';
    for (auto &block : MF.BBs) {
      cout << block.getName() << ": [";
      for (auto it = block.succBegin(); it != block.succEnd(); ++it) {
        auto successor = *it;
        cout << successor->getName() << ", ";
      }
      cout << "]\n";
    }

    cout << "predecessors" << '\n';
    for (auto &block : MF.BBs) {
      cout << block.getName() << ": [";
      for (auto it = block.predBegin(); it != block.predEnd(); ++it) {
        auto predecessor = *it;
        cout << predecessor->getName() << ", ";
      }
      cout << "]\n";
    }
  }

  // findLastRetWrite will do a breadth-first search through the dominator tree
  // looking for the last write to X0.
  int findLastRetWrite(MCBasicBlock *bb) {
    set<MCBasicBlock *> to_search = {bb};
    while (to_search.size() != 0) { // exhaustively search previous BBs
      set<MCBasicBlock *> next_search =
          {}; // blocks to search in the next iteration
      for (auto &b : to_search) {
        auto instrs = b->getInstrs();
        for (auto it = instrs.rbegin(); it != instrs.rend();
             it++) { // iterate backwards looking for writes
          const auto &inst = it->getMCInst();
          const auto &firstOp = inst.getOperand(0);
          if (firstOp.isReg() && firstOp.getReg() == AArch64::X0) {
            return it->getOpId(0);
          }
        }
        for (auto &new_b : MF.dom_tree_inv[b]) {
          next_search.insert(new_b);
        }
      }
      to_search = next_search;
    }

    // if we've found no write to X0, we just need to return version 2,
    // which corresponds to the function argument X0 after freeze
    return 2;
  }

  // TODO: @Nader this should just fall out of our SSA implementation
  void adjustReturns() {
    for (auto &block : MF.BBs) {
      for (auto &instr : block.getInstrs()) {
        if (instr.getOpcode() == AArch64::RET) {
          auto lastWriteID = findLastRetWrite(&block);
          auto &retArg = instr.getMCInst().getOperand(0);
          instr.setOpId(0, lastWriteID);
          retArg.setReg(AArch64::X0);
        }
      }
    }

    cout << "After adjusting return:\n";
    for (auto &b : MF.BBs) {
      cout << b.getName() << ":\n";
      b.print();
    }
  }
};

// Return variables that are read before being written in the basic block
auto FindReadBeforeWritten(std::vector<MCInst> &instrs,
                           llvm::MCInstrAnalysis *Ana_ptr) {
  std::unordered_set<MCOperand, MCOperandHash, MCOperandEqual> reads;
  std::unordered_set<MCOperand, MCOperandHash, MCOperandEqual> writes;
  // TODO for writes, should only apply to instructions that update a
  // destination register
  for (auto &I : instrs) {
    if (Ana_ptr->isReturn(I))
      continue;
    assert(I.getNumOperands() > 0 && "MCInst with zero operands");
    for (unsigned j = 1; j < I.getNumOperands(); ++j) {
      if (!writes.contains(I.getOperand(j)))
        reads.insert(I.getOperand(j));
    }
    writes.insert(I.getOperand(0));
  }

  return reads;
}

// Return variable that are read before being written in the basicblock
auto FindReadBeforeWritten(MCBasicBlock &block,
                           llvm::MCInstrAnalysis *Ana_ptr) {
  auto mcInstrs = block.getInstrs();
  std::unordered_set<MCOperand, MCOperandHash, MCOperandEqual> reads;
  std::unordered_set<MCOperand, MCOperandHash, MCOperandEqual> writes;
  // TODO for writes, should only apply to instructions that update a
  // destination register
  for (auto &WI : mcInstrs) {
    auto &I = WI.getMCInst();
    if (Ana_ptr->isReturn(I))
      continue;
    assert(I.getNumOperands() > 0 && "MCInst with zero operands");
    for (unsigned j = 1; j < I.getNumOperands(); ++j) {
      if (!writes.contains(I.getOperand(j)))
        reads.insert(I.getOperand(j));
    }
    writes.insert(I.getOperand(0));
  }

  return reads;
}

// Perform verification on two alive functions
Results backend_verify(std::optional<IR::Function> &fn1,
                       std::optional<IR::Function> &fn2,
                       llvm::TargetLibraryInfoWrapperPass &TLI,
                       bool print_transform = false,
                       bool always_verify = false) {
  Results r;
  r.t.src = move(*fn1);
  r.t.tgt = move(*fn2);

  if (!always_verify) {
    stringstream ss1, ss2;
    r.t.src.print(ss1);
    r.t.tgt.print(ss2);
    if (ss1.str() == ss2.str()) {
      if (print_transform)
        r.t.print(*out, {});
      r.status = Results::SYNTACTIC_EQ;
      return r;
    }
  }

  smt_init->reset();
  r.t.preprocess();
  TransformVerify verifier(r.t, false);

  if (print_transform)
    r.t.print(*out, {});

  {
    auto types = verifier.getTypings();
    if (!types) {
      r.status = Results::TYPE_CHECKER_FAILED;
      return r;
    }
    assert(types.hasSingleTyping());
  }

  r.errs = verifier.verify();
  if (r.errs) {
    r.status = r.errs.isUnsound() ? Results::UNSOUND : Results::FAILED_TO_PROVE;
  } else {
    r.status = Results::CORRECT;
  }
  return r;
}

void adjustSrcInputs(std::optional<IR::Function> &srcFn) {
  std::vector<std::unique_ptr<IR::Value>> new_inputs;
  unsigned idx = 0;

  for (auto &v : srcFn->getInputs()) {
    auto &orig_typ = v.getType();

    if (!orig_typ.isIntType())
      report_fatal_error("[Unsupported Function Argument]: Only int types "
                         "supported for now");
    // FIXME: need to handle wider types
    if (orig_typ.bits() > 64)
      report_fatal_error("[Unsupported Function Argument]: Only int types 64 "
                         "bits or smaller supported for now");

    if (orig_typ.bits() == 64) {
      idx++;
      continue;
    }

    auto input_ptr = dynamic_cast<const IR::Input *>(&v);
    assert(input_ptr);
    auto extended_type = &get_int_type(64);
    string name(v.getName().substr(v.getName().rfind('%')));

    IR::ParamAttrs attrs(input_ptr->getAttributes());
    // Do we need to update the value_cache?
    new_inputs.emplace_back(make_unique<IR::Input>(
        *extended_type, std::move(name), std::move(attrs)));
    new_input_idx_bitwidth.emplace_back(idx, orig_typ.bits());
    idx++;
  }

  for (unsigned i = 0; i < new_inputs.size(); ++i) {
    string input_trunc(
        new_inputs[i]->getName().substr(new_inputs[i]->getName().rfind('%')) +
        "_t");
    auto new_ir = make_unique<IR::ConversionOp>(
        srcFn->getInput(i).getType(), std::move(input_trunc),
        *new_inputs[i].get(), IR::ConversionOp::Trunc);
    srcFn->rauw(srcFn->getInput(new_input_idx_bitwidth[i].first), *new_ir);
    srcFn->getFirstBB().addInstr(std::move(new_ir), true);
    srcFn->addInputAt(move(new_inputs[i]), new_input_idx_bitwidth[i].first);
  }

  // cout << "After adjusting inputs:\n";
  // for (auto &v : srcFn->getInputs()) {
  //   cout << v.getName() << endl;
  // }
}

void adjustSourceReturn(std::optional<IR::Function> &srcFn) {
  // Do nothing if the return operand attribute does not include signext/zeroext
  auto &ret_typ = srcFn->getType();
  auto &fnAttrs = srcFn->getFnAttrs();

  if (!fnAttrs.has(IR::FnAttrs::Sext)) {
    return;
  }

  if (!ret_typ.isIntType())
    report_fatal_error("[Unsupported Function Return]: Only int types "
                       "supported for now");

  if (ret_typ.bits() > 64)
    report_fatal_error("[Unsupported Function Return]: Only int types 64 "
                       "bits or smaller supported for now");

  // don't need to do any extension if the return type is exactly 32 bits
  if (ret_typ.bits() == 64 || ret_typ.bits() == 32)
    return;

  has_ret_attr = true;
  original_ret_bitwidth = ret_typ.bits();

  srcFn->setType(get_int_type(64));
  for (auto bb : srcFn->getBBs()) {

    for (auto &i : bb->instrs()) {
      if (dynamic_cast<const IR::Return *>(&i)) {
        auto v_op = i.operands();
        assert(v_op.size() == 1);

        if (original_ret_bitwidth < 32) {
          string val_name(v_op[0]->getName() + "_sext");
          string val_name_2(v_op[0]->getName() + "_zext");
          auto new_ir = make_unique<IR::ConversionOp>(
              get_int_type(32), std::move(val_name), *v_op[0],
              IR::ConversionOp::SExt);
          auto new_ir_2 = make_unique<IR::ConversionOp>(
              get_int_type(64), std::move(val_name_2), *new_ir,
              IR::ConversionOp::ZExt);
          auto new_ret = make_unique<IR::Return>(get_int_type(64), *new_ir_2);

          bb->addInstrAt(std::move(new_ir), &i, true);
          bb->addInstrAt(std::move(new_ir_2), &i, true);
          bb->addInstrAt(std::move(new_ret), &i, false);
          bb->delInstr(&i);
        } else {
          string val_name(v_op[0]->getName() + "_sext");
          auto new_ir = make_unique<IR::ConversionOp>(
              get_int_type(64), std::move(val_name), *v_op[0],
              IR::ConversionOp::SExt);

          auto new_ret = make_unique<IR::Return>(get_int_type(64), *new_ir);

          bb->addInstrAt(std::move(new_ir), &i, true);
          bb->addInstrAt(std::move(new_ret), &i, false);
          bb->delInstr(&i);
        }
        break;
      }
    }
  }
}

bool backendTV() {
  if (asm_input) {
    if (opt_file2.empty()) {
      cerr << "Missing asm input file" << endl;
      exit(-1);
    }
    cout << "Using file " << opt_file2 << " as asm input" << endl;
    // exit(-1);
  }

  llvm::LLVMContext Context;
  auto M1 = openInputFile(Context, opt_file1);
  if (!M1.get()) {
    cerr << "Could not read bitcode from '" << opt_file1 << "'\n";
    exit(-1);
  }

#define ARGS_MODULE_VAR M1
#include "llvm_util/cmd_args_def.h"

  // FIXME: For now, we're hardcoding the target triple
  M1.get()->setDataLayout(
      "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128");
  auto &DL = M1.get()->getDataLayout();
  llvm::Triple targetTriple(M1.get()->getTargetTriple());
  llvm::TargetLibraryInfoWrapperPass TLI(targetTriple);

  llvm_util::initializer llvm_util_init(*out, DL);
  smt_init.emplace();

  cout << "\n\nConverting source llvm function to alive ir\n";
  std::optional<IR::Function> AF;
  unsigned f_def_cnt = 0;
  for (auto &F : *M1.get()) {
    if (F.isDeclaration())
      continue;
    f_def_cnt++;
  }

  // FIXME: temporarily here to pass tests with multiple functions in the VM
  // test cases
  if (f_def_cnt != 1) {
    cout << "defined functions = " << M1.get()->getFunctionList().size()
         << "\n";
    cout << "Transformation seems to be correct!\n\n";
    ++num_correct;
    return false;
  }

  // Only try to verify the first function in the module
  for (auto &F : *M1.get()) {
    if (F.isDeclaration())
      continue;
    if (!func_names.empty() && !func_names.count(F.getName().str()))
      continue;
    AF = llvm2alive(F, TLI.getTLI(F));
    break;
  }

  if (!AF) {
    report_fatal_error("Could not convert llvm function to alive ir");
  }

  AF->print(cout << "\n----------alive-ir-src.ll-file----------\n");
  adjustSrcInputs(AF);
  AF->print(cout << "\n----------alive-ir-src.ll-file----changed-input-\n");
  adjustSourceReturn(AF);
  AF->print(cout << "\n----------alive-ir-src.ll-file----changed-return-\n");

  LLVMInitializeAArch64TargetInfo();
  LLVMInitializeAArch64Target();
  LLVMInitializeAArch64TargetMC();
  LLVMInitializeAArch64AsmParser();
  LLVMInitializeAArch64AsmPrinter();

  std::string Error;
  const char *TripleName = "aarch64-arm-none-eabi";
  auto Target = llvm::TargetRegistry::lookupTarget(TripleName, Error);
  if (!Target) {
    cerr << Error;
    exit(-1);
  }
  llvm::TargetOptions Opt;
  const char *CPU = "apple-a12";
  auto RM = llvm::Optional<llvm::Reloc::Model>();
  std::unique_ptr<llvm::TargetMachine> TM(
      Target->createTargetMachine(TripleName, CPU, "", Opt, RM));
  llvm::SmallString<1024> Asm;
  llvm::raw_svector_ostream Dest(Asm);

  llvm::legacy::PassManager pass;
  if (TM->addPassesToEmitFile(pass, Dest, nullptr, llvm::CGFT_AssemblyFile)) {
    cerr << "Failed to generate assembly";
    exit(-1);
  }
  pass.run(*M1);

  // FIXME only do this in verbose mode, or something
  cout << "\n----------arm asm----------\n\n";
  for (size_t i = 0; i < Asm.size(); ++i)
    cout << Asm[i];
  cout << "-------------\n";
  cout << "\n\n";
  llvm::Triple TheTriple(TripleName);

  auto MCOptions = llvm::mc::InitMCTargetOptionsFromFlags();
  std::unique_ptr<llvm::MCRegisterInfo> MRI(
      Target->createMCRegInfo(TripleName));
  assert(MRI && "Unable to create target register info!");

  std::unique_ptr<llvm::MCAsmInfo> MAI(
      Target->createMCAsmInfo(*MRI, TripleName, MCOptions));
  assert(MAI && "Unable to create MC asm info!");

  std::unique_ptr<llvm::MCSubtargetInfo> STI(
      Target->createMCSubtargetInfo(TripleName, CPU, ""));
  assert(STI && "Unable to create subtarget info!");
  assert(STI->isCPUStringValid(CPU) && "Invalid CPU!");

  llvm::MCContext Ctx(TheTriple, MAI.get(), MRI.get(), STI.get());
  llvm::SourceMgr SrcMgr;

  if (asm_input) {
    auto MB_Asm =
        ExitOnErr(errorOrToExpected(llvm::MemoryBuffer::getFile(opt_file2)));
    assert(MB_Asm);
    cout << "reading asm from file\n";
    for (auto it = MB_Asm->getBuffer().begin(); it != MB_Asm->getBuffer().end();
         ++it) {
      cout << *it;
    }
    cout << "-------------\n";
    SrcMgr.AddNewSourceBuffer(std::move(MB_Asm), llvm::SMLoc());
  } else {
    auto Buf = llvm::MemoryBuffer::getMemBuffer(Asm.c_str());
    SrcMgr.AddNewSourceBuffer(std::move(Buf), llvm::SMLoc());
  }

  std::unique_ptr<llvm::MCInstrInfo> MCII(Target->createMCInstrInfo());
  assert(MCII && "Unable to create instruction info!");

  std::unique_ptr<llvm::MCInstPrinter> IPtemp(
      Target->createMCInstPrinter(TheTriple, 0, *MAI, *MCII, *MRI));

  auto Ana = std::make_unique<MCInstrAnalysis>(MCII.get());

  MCStreamerWrapper Str(Ctx, Ana.get(), IPtemp.get(), MRI.get());

  std::unique_ptr<llvm::MCAsmParser> Parser(
      llvm::createMCAsmParser(SrcMgr, Ctx, Str, *MAI));
  assert(Parser);

  llvm::MCTargetOptions Opts;
  Opts.PreserveAsmComments = false;
  std::unique_ptr<llvm::MCTargetAsmParser> TAP(
      Target->createMCAsmParser(*STI, *Parser, *MCII, Opts));
  assert(TAP);
  Parser->setTargetParser(*TAP);
  Parser->Run(true); // ??

  // FIXME remove printing of the mcInsts
  // For now, print the parsed instructions for debug puropses
  cout << "\n\nPretty Parsed MCInsts:\n";
  for (auto I : Str.Insts) {
    I.dump_pretty(llvm::errs(), IPtemp.get(), " ", MRI.get());
    llvm::errs() << '\n';
  }

  cout << "\n\nParsed MCInsts:\n";
  for (auto I : Str.Insts) {
    I.dump_pretty(llvm::errs());
    llvm::errs() << '\n';
  }

  if (opt_asm_only) {
    cout << "arm instruction count = " << Str.Insts.size() << "\n";
    cout.flush();
    llvm::errs().flush();
    cerr.flush();
    exit(0);
  }

  cout << "\n\n";

  if (AF->isVarArgs()) {
    report_fatal_error("Varargs not supported yet");
  }

  Str.printBlocks();
  Str.removeEmptyBlocks(); // remove empty basic blocks, including .Lfunc_end
  Str.printBlocks();

  Str.addEntryBlock();
  // Str.addTerminator();
  Str.generateSuccessors();
  Str.generatePredecessors();
  Str.findArgs(AF);
  Str.rewriteOperands();
  Str.printCFG();
  Str.generateDominator();
  Str.generateDominatorFrontier();
  Str.findDefiningBlocks();
  Str.findPhis();
  Str.generateDomTree();
  Str.ssaRename();
  Str.adjustReturns();

  cout << "after SSA conversion\n";
  Str.printBlocks();

  auto &MF = Str.MF;
  auto TF = arm2alive(MF, AF, IPtemp.get(), MRI.get());
  if (TF)
    TF->print(cout << "\n----------alive-lift-arm-target----------\n");

  auto r = backend_verify(AF, TF, TLI, true);

  // cout << "exiting for valgrind\n";
  // return false;
  if (r.status == Results::ERROR) {
    *out << "ERROR: " << r.error;
    ++num_errors;
    return true;
  }

  if (opt_print_dot) {
    r.t.src.writeDot("src");
    r.t.tgt.writeDot("tgt");
  }

  switch (r.status) {
  case Results::ERROR:
    UNREACHABLE();
    break;

  case Results::SYNTACTIC_EQ:
    *out << "Transformation seems to be correct! (syntactically equal)\n\n";
    ++num_correct;
    break;

  case Results::CORRECT:
    *out << "Transformation seems to be correct!\n\n";
    ++num_correct;
    break;

  case Results::TYPE_CHECKER_FAILED:
    *out << "Transformation doesn't verify!\n"
            "ERROR: program doesn't type check!\n\n";
    ++num_errors;
    return true;

  case Results::UNSOUND:
    *out << "Transformation doesn't verify!\n\n";
    if (!opt_quiet)
      *out << r.errs << endl;
    ++num_unsound;
    return false;

  case Results::FAILED_TO_PROVE:
    *out << r.errs << endl;
    ++num_failed;
    return true;
  }
  return false;
  /*
    auto SRC = findFunction(*M1, opt_src_fn);
    auto TGT = findFunction(*M1, opt_tgt_fn);
    if (SRC && TGT) {
      compareFunctions(*SRC, *TGT, TLI);
      return;
    } else {
      M2 = CloneModule(*M1);
      optimizeModule(M2.get());
    }

    // FIXME: quadratic, may not be suitable for very large modules
    // emitted by opt-fuzz
    for (auto &F1 : *M1.get()) {
      if (F1.isDeclaration())
        continue;
      if (!func_names.empty() && !func_names.count(F1.getName().str()))
        continue;
      for (auto &F2 : *M2.get()) {
        if (F2.isDeclaration() || F1.getName() != F2.getName())
          continue;
        if (!compareFunctions(F1, F2, TLI))
          if (opt_error_fatal)
            return;
        break;
      }
    }

    */
}

void bitcodeTV() {
  llvm::LLVMContext Context;
  unsigned M1_anon_count = 0;
  auto M1 = openInputFile(Context, opt_file1);
  if (!M1.get()) {
    cerr << "Could not read bitcode from '" << opt_file1 << "'\n";
    exit(-1);
  }

#define ARGS_MODULE_VAR M1
#include "llvm_util/cmd_args_def.h"

  auto &DL = M1.get()->getDataLayout();
  llvm::Triple targetTriple(M1.get()->getTargetTriple());
  llvm::TargetLibraryInfoWrapperPass TLI(targetTriple);

  llvm_util::initializer llvm_util_init(*out, DL);
  smt_init.emplace();

  unique_ptr<llvm::Module> M2;
  if (opt_file2.empty()) {
    auto SRC = findFunction(*M1, opt_src_fn);
    auto TGT = findFunction(*M1, opt_tgt_fn);
    if (SRC && TGT) {
      compareFunctions(*SRC, *TGT, TLI);
      return;
    } else {
      M2 = CloneModule(*M1);
      optimize_module(M2.get(), optPass);
    }
  } else {
    M2 = openInputFile(Context, opt_file2);
    if (!M2.get()) {
      *out << "Could not read bitcode from '" << opt_file2 << "'\n";
      exit(-1);
    }
  }

  if (M1.get()->getTargetTriple() != M2.get()->getTargetTriple()) {
    *out << "Modules have different target triples\n";
    exit(-1);
  }

  // FIXME: quadratic, may not be suitable for very large modules
  // emitted by opt-fuzz
  for (auto &F1 : *M1.get()) {
    if (F1.isDeclaration())
      continue;
    if (F1.getName().empty())
      M1_anon_count++;
    if (!func_names.empty() && !func_names.count(F1.getName().str()))
      continue;
    unsigned M2_anon_count = 0;
    for (auto &F2 : *M2.get()) {
      if (F2.isDeclaration())
        continue;
      if (F2.getName().empty())
        M2_anon_count++;
      if ((F1.getName().empty() && (M1_anon_count == M2_anon_count)) ||
          (F1.getName() == F2.getName())) {
        if (!compareFunctions(F1, F2, TLI))
          if (opt_error_fatal)
            return;
        break;
      }
    }
  }
}

// arm util functions

int main(int argc, char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm::EnableDebugBuffering = true;
  llvm::llvm_shutdown_obj llvm_shutdown; // Call llvm_shutdown() on exit.

  std::string Usage =
      R"EOF(Alive2 stand-alone translation validator:
version )EOF";
  Usage += alive_version;
  Usage += R"EOF(
see alive-tv --version  for LLVM version info,

This program takes either one or two LLVM IR files files as
command-line arguments. Both .bc and .ll files are supported.

If two files are provided, alive-tv checks that functions in the
second file refine functions in the first file, matching up functions
by name. Functions not found in both files are ignored. It is an error
for a function to be found in both files unless they have the same
signature.

If one file is provided, there are two possibilities. If the file
contains a function called "src" and also a function called "tgt",
then alive-tv will determine whether src is refined by tgt. It is an
error if src and tgt do not have the same signature. Otherwise,
alive-tv will optimize the entire module using an optimization
pipeline similar to -O2, and then verify that functions in the
optimized module refine those in the original one. This provides a
convenient way to demonstrate an existing optimizer bug.
)EOF";

  llvm::cl::HideUnrelatedOptions(alive_cmdargs);
  llvm::cl::ParseCommandLineOptions(argc, argv, Usage);

  if (opt_backend_tv) {
    backendTV(); // this is the function we use to perform arm translation
                 // validation
  } else {
    bitcodeTV();
  }

  *out << "Summary:\n"
          "  "
       << num_correct
       << " correct transformations\n"
          "  "
       << num_unsound
       << " incorrect transformations\n"
          "  "
       << num_failed
       << " failed-to-prove transformations\n"
          "  "
       << num_errors << " Alive2 errors\n";

  if (opt_smt_stats)
    smt::solver_print_stats(*out);

  smt_init.reset();

  if (opt_alias_stats)
    IR::Memory::printAliasStats(*out);

  return num_errors > 0;
}
