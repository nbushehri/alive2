// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "ir/function.h"
#include "ir/state.h"
#include "util/config.h"
#include "util/symexec.h"
#include "util/concreteval.h"
#include "util/random.h"
#include <iostream>

using namespace IR;
using namespace std;
using util::config::dbg;

namespace util {

void sym_exec(State &s) {
  Function &f = const_cast<Function &>(s.getFn());

  // global constants need to be created in the right order so they get the
  // first bids in source, and the last in target
  set<const Value *> seen_inits;
  for (const auto &v : f.getConstants()) {
    if (auto gv = dynamic_cast<const GlobalVariable *>(&v)) {
      if (gv->isConst() == s.isSource()) {
        s.exec(v);
        seen_inits.emplace(&v);
      }
    }
  }

  // Hacky solution but should be fine for now
  // This is needed since alive-exec doens't work with vector types
  bool alive_abort = false;

  cout << "running exec.cpp" << '\n';
  // TODO need to check for Value subclasses for inputs and constants
  // i.e. PoisonValue, UndefValue, and etc.
  // initialize inputs with concrete values
  std::map<const Value *, ConcreteVal * > concrete_vals;
  for (auto &i : f.getInputs()) {
    // TODO for now we support IntType and FloatType
    assert((i.getType().isIntType() 
          || i.getType().isFloatType() 
          || i.getType().isVectorType()) && "AliveExec-Error : input type not supported");
    auto I = concrete_vals.find(&i);
    assert(I == concrete_vals.end());
    if (i.getType().isIntType()) {
      // Comment this to avoid random int function arguments 
      //auto rand_int64 = get_random_int64();
      //cout << "input param random value = " << rand_int64 << "\n"; 
      auto new_val = new ConcreteVal(false, llvm::APInt(i.getType().bits(), 3));
      concrete_vals.emplace(&i, new_val);
    } else if (i.getType().isFloatType()) {
      cout << "float input encountered " << '\n';

      if (i.bits() == 32) {
        auto new_val = new ConcreteVal(false, llvm::APFloat(3.0f));
        concrete_vals.emplace(&i, new_val);
      } else if (i.bits() == 64) {
        auto new_val = new ConcreteVal(false, llvm::APFloat(3.0));
        concrete_vals.emplace(&i, new_val);
      } else if (i.bits() == 16) {
        auto new_val = new ConcreteVal(
            false, llvm::APFloat(llvm::APFloatBase::IEEEhalf(), "3.0"));
        concrete_vals.emplace(&i, new_val);
      } else {
        cout << "AliveExec-Error : Unsupported float input type. Aborting"
             << '\n';
        exit(EXIT_FAILURE);
      }
    } else if (i.getType().isVectorType()) {
      alive_abort = true;
      auto *new_val = new ConcreteValVect(false, &i);
      //auto new_val = new ConcreteVal(false, &i);
      concrete_vals.emplace(&i, new_val);
      //concrete_vals.emplace(&i, new_val);
    } else {
      cout << "AliveExec-Error : input type not supported" << '\n';
      exit(EXIT_FAILURE);
    }
  }
  for (auto &i : f.getConstants()) {
    auto I = concrete_vals.find(&i);
    assert(I == concrete_vals.end());
    // read poison consts
    if (dynamic_cast<const PoisonValue *>(&i)) {
      auto new_val = new ConcreteVal(true, llvm::APInt(i.getType().bits(), 0));
      concrete_vals.emplace(&i, new_val);
    } else if (auto const_ptr = dynamic_cast<const IntConst *>(&i)) {
      // auto const_ptr = dynamic_cast<const IntConst *>(&i);
      // cout << "constant: " << i.getName() << " type: " <<
      // i.getType().toString()
      //<< " bitwidth: " << i.getType().bits() << '\n';
      // need to do this for constants that are larger than i64
      if (const_ptr->getString()) {
        // cout << "encountered const stored as string: " <<
        // *(const_ptr->getString())<< '\n';
        auto new_val = new ConcreteVal(
            false,
            llvm::APInt(i.getType().bits(), *(const_ptr->getString()), 10));
        concrete_vals.emplace(&i, new_val);
      } else if (const_ptr->getInt()) {
        // cout << "encountered const stored as int: " <<
        // *(const_ptr->getInt())<< '\n';
        auto new_val = new ConcreteVal(
            false, llvm::APInt(i.getType().bits(), *(const_ptr->getInt())));
        concrete_vals.emplace(&i, new_val);
      }
    } else if (auto const_ptr = dynamic_cast<const FloatConst *>(&i)) {
      assert((const_ptr->bits() == 32) || (const_ptr->bits() == 64) ||
             (const_ptr->bits() == 16)); // TODO: add support for other FP types
      cout << "float constant: " << const_ptr->getName()
           << " type: " << i.getType().toString()
           << " bitwidth: " << i.getType().bits() << '\n';
      if (auto double_float = const_ptr->getDouble()) {
        cout << "double repr of float constant : " << *double_float << '\n';
        if (const_ptr->bits() == 32) {
          // Since the original ir was representable using float, casting the
          // double back to float should be safe I think but this looks pretty
          // bad and should think of a better solution
          auto new_val = new ConcreteVal(
              false, llvm::APFloat(static_cast<float>(*double_float)));
          concrete_vals.emplace(&i, new_val);
        } else if (const_ptr->bits() == 64) {
          auto new_val = new ConcreteVal(false, llvm::APFloat(*double_float));
          concrete_vals.emplace(&i, new_val);
        } else {
          UNREACHABLE();
        }
      }
      if (auto string_float = const_ptr->getString()) {
        cout << "string repr of float constant : " << *string_float << '\n';
      } else if (auto int_float = const_ptr->getInt()) {

        auto new_val = new ConcreteVal(false,
                                  llvm::APFloat(llvm::APFloatBase::IEEEhalf(),
                                                llvm::APInt(16, *int_float)));
        concrete_vals.emplace(&i, new_val);

        cout << "int repr of float constant : " << *int_float << '\n';
      }

    } else { // TODO for now we only support Int constants
      cout << "AliveExec-Error : Unsupported constant type. Aborting!" << '\n';
      exit(EXIT_FAILURE);
    }
  }

  // add constants & inputs to State table first of all
  for (auto &l : {f.getConstants(), f.getInputs(), f.getUndefs()}) {
    for (const auto &v : l) {
      if (!seen_inits.count(&v))
        s.exec(v);
    }
  }

  s.exec(Value::voidVal);

  bool first = true;
  if (f.getFirstBB().getName() != "#init") {
    s.finishInitializer();
    first = false;
  }

  const BasicBlock *pred_block = nullptr;
  const BasicBlock *cur_block = nullptr;
  
  bool UB_flag = false;
  // TODO add stack for alloca

  // TODO add support for noninteger types

  cout << "---run Interpreter---" << '\n';
  // Interpreter returns with a message as soon as it encounters undef
  // Hence we only need to deal with defined and poison vals during
  // interpretation

  for (auto &bb : f.getBBs()) {
    if (&f.getFirstBB() != bb)
      continue;
    // bb is the first basicblock
    cur_block = bb;
    while (cur_block) {
      if (UB_flag) {
        cout << "Interpreter reached UB state. Aborting!" << '\n';
        exit(EXIT_SUCCESS);
      }
      for (auto &i : cur_block->instrs()) {
        cout << "cur inst: ";
        i.print(cout);
        cout << '\n';
        if (dynamic_cast<const BinOp *>(&i)) {
          auto v_op = i.operands();
          // i.print(cout);
          // cout << '\n';
          auto ptr = dynamic_cast<const BinOp *>(&i);
          auto res_val = ptr->concreteEval(concrete_vals, UB_flag);
          auto I = concrete_vals.find(ptr);
          if (I == concrete_vals.end()) {
            concrete_vals.emplace(ptr, res_val);
          } else {
            concrete_vals[ptr] = res_val;
          }
        } else if (dynamic_cast<const UnaryOp *>(&i)) {
          auto ptr = dynamic_cast<const UnaryOp *>(&i);
          auto res_val = ptr->concreteEval(concrete_vals);
          auto I = concrete_vals.find(ptr);
          if (I == concrete_vals.end()) {
            concrete_vals.emplace(ptr, res_val);
          } else {
            concrete_vals[ptr] = res_val;
          }
        } else if (dynamic_cast<const ConversionOp *>(&i)) {
          // auto v_op = i.operands();
          // cout << "conv ops len" << v_op.size() << '\n';
          // cout << i.getType().toString() << '\n';
          auto ptr = dynamic_cast<const ConversionOp *>(&i);
          auto res_val = ptr->concreteEval(concrete_vals);
          auto I = concrete_vals.find(ptr);
          if (I == concrete_vals.end()) {
            concrete_vals.emplace(ptr, res_val);
          } else {
            concrete_vals[ptr] = res_val;
          }
        } else if (dynamic_cast<const ICmp *>(&i)) {
          auto icmp_ptr = dynamic_cast<const ICmp *>(&i);
          auto res_val = icmp_ptr->concreteEval(concrete_vals);
          auto I = concrete_vals.find(icmp_ptr);
          if (I == concrete_vals.end()) {
            concrete_vals.emplace(icmp_ptr, res_val);
          } else {
            concrete_vals[icmp_ptr] = res_val;
          }
        } else if (dynamic_cast<const FCmp *>(&i)) {
          auto fcmp_ptr = dynamic_cast<const FCmp *>(&i);
          auto res_val = fcmp_ptr->concreteEval(concrete_vals);
          auto I = concrete_vals.find(fcmp_ptr);
          if (I == concrete_vals.end()) {
            concrete_vals.emplace(fcmp_ptr, res_val);
          } else {
            concrete_vals[fcmp_ptr] = res_val;
          }
        } else if (dynamic_cast<const Select *>(&i)) {
          // cout << "ICMP instr" << '\n';
          auto select_ptr = dynamic_cast<const Select *>(&i);
          auto res_val = select_ptr->concreteEval(concrete_vals);
          auto I = concrete_vals.find(select_ptr);
          if (I == concrete_vals.end()) {
            concrete_vals.emplace(select_ptr, res_val);
          } else {
            concrete_vals[select_ptr] = res_val;
          }
        } else if (dynamic_cast<const Return *>(&i)) {
          auto v_op = i.operands();
          assert(v_op.size() == 1);
          assert(concrete_vals.find(v_op[0]) != concrete_vals.end());
          cout << "Interpreter return result:" << '\n';
          concrete_vals[v_op[0]]->print();
          cur_block = nullptr;
          break;
        } else if (dynamic_cast<const JumpInstr *>(&i)) {
          // cout << "jump inst: " << i << '\n';
          auto br_ptr = dynamic_cast<const Branch *>(&i);
          if (br_ptr) {
            if (!br_ptr->getCondPtr()) { // unconditional branch
              // cout << "unconditional branch" << '\n';
              assert(br_ptr->getTruePtr());
              pred_block = cur_block;
              cur_block = br_ptr->getTruePtr();
              break;
            } else {
              assert(br_ptr->getTruePtr() && br_ptr->getFalsePtr());
              // cout << "conditional branch" << '\n';
              // lookup the concrete value of cond from concrete_vals
              // if true set cur_block to dst_true BB else dst_false

              auto I = concrete_vals.find(br_ptr->getCondPtr());
              assert(I !=
                     concrete_vals
                         .end()); // condition must be evaluated at this point
              auto concrete_cond_val = I->second;
              if (concrete_cond_val->isPoison()) {
                UB_flag = true;
                cout << "branch condition val is poison. This results in UB" << '\n';
                break;
              } else {
                pred_block = cur_block;
                if (concrete_cond_val->getVal().getBoolValue()) {
                  cur_block = br_ptr->getTruePtr();
                } else {
                  cur_block = br_ptr->getFalsePtr();
                }
              }
            }
          }
        } else if (dynamic_cast<const Phi *>(&i)) {
          // cout << "phi inst: " << i << '\n';
          assert(pred_block);
          // cout << "pred block: " << pred_block->getName() << '\n';
          auto phi_ptr = dynamic_cast<const Phi *>(&i);
          const auto &phi_vals = phi_ptr->getValues();
          // read pred_block to determine which phi value to choose
          for (auto &[phi_val_ptr, phi_label] : phi_vals) {
            // What to do if the phi_val is poison?
            if (pred_block->getName() != phi_label) {
              continue;
            }

            auto phi_val_concrete_I = concrete_vals.find(phi_val_ptr);
            assert(phi_val_concrete_I != concrete_vals.end());
            auto phi_concrete_I = concrete_vals.find(phi_ptr);
            if (phi_concrete_I == concrete_vals.end()) {
              auto new_val = new ConcreteVal(*(phi_val_concrete_I->second));
              concrete_vals.emplace(phi_ptr, new_val);
            } else {
              // concrete_vals[phi_ptr].setValPtr(std::make_unique<llvm::APInt>(*(phi_val_concrete_I->second.getValPtr()));
              auto new_phi_val = phi_val_concrete_I->second->getVal();
              concrete_vals[phi_ptr]->setVal(new_phi_val);
            }
            break;
          }
        } else {
          cout << "AliveExec-Error : unsupported instruction. Aborting" << '\n';
          exit(EXIT_FAILURE);
        }
      }
    }
  }

  cout << "---Interpreter done---" << '\n';

  for (auto [val, c_val]:concrete_vals){
    delete c_val;
  }
  concrete_vals.clear();
  if (alive_abort) {
    return;
  }

  for (auto &bb : f.getBBs()) {
    if (!s.startBB(*bb))
      continue;

    for (auto &i : bb->instrs()) {
      if (first && dynamic_cast<const JumpInstr *>(&i))
        s.finishInitializer();
      auto val = s.exec(i);
      auto &name = i.getName();

      if (config::symexec_print_each_value && name[0] == '%')
        dbg() << name << " = " << val << '\n';
    }

    first = false;
  }

  if (config::symexec_print_each_value) {
    dbg() << "domain = " << s.functionDomain()
          << "\nreturn domain = " << s.returnDomain()
          << "\nreturn = " << s.returnVal().first << s.returnMemory() << "\n\n";
  }
}

} // namespace util
