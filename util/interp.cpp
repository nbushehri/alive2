// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "util/interp.h"
#include "ir/function.h"
#include "ir/state.h"
#include "util/concreteval.h"
#include "util/config.h"
#include "util/random.h"
#include <iostream>

using namespace IR;
using namespace std;
using util::config::dbg;

namespace util {

void Interpreter::setUnsupported(std::string reason) {
  unsupported_flag = true;
  if (unsupported_reason.empty())
    unsupported_reason = reason;
}

shared_ptr<ConcreteVal> Interpreter::getInputValue(unsigned index,
                                                   const Input &i) {
  if (i.getType().isIntType()) {
    // Comment this to avoid random int function arguments
    // auto rand_int64 = get_random_int64();
    // cout << "input param random value = " << rand_int64 << "\n";
    return shared_ptr<ConcreteVal>(
        new ConcreteValInt(false, llvm::APInt(i.getType().bits(), 3)));
  } else if (i.getType().isFloatType()) {
    if (i.bits() == 32) {
      return shared_ptr<ConcreteVal>(
          new ConcreteValFloat(false, llvm::APFloat(3.0f)));
    } else if (i.bits() == 64) {
      return shared_ptr<ConcreteVal>(
          new ConcreteValFloat(false, llvm::APFloat(3.0)));
    } else if (i.bits() == 16) {
      return shared_ptr<ConcreteVal>(new ConcreteValFloat(
          false, llvm::APFloat(llvm::APFloatBase::IEEEhalf(), "3.0")));
    } else {
      setUnsupported("unsupported float input type");
      return nullptr;
    }
  } else {
    setUnsupported("unsupported input type");
    return nullptr;
  }
}

ConcreteVal *Interpreter::getPoisonValue(const IR::Type &type) {
  if (&type == &IR::Type::voidTy) {
    return new ConcreteValVoid();
  } else if (type.isIntType()) {
    unsigned bits = type.bits();
    return new ConcreteValInt(true, llvm::APInt(bits, 0));
  } else if (type.isFloatType()) {
    auto semantics =
        getFloatSemantics(static_cast<const IR::FloatType &>(type));
    return new ConcreteValFloat(true, llvm::APFloat::getZero(*semantics));
  } else if (type.isAggregateType()) {
    const auto &aggregate = static_cast<const IR::AggregateType &>(type);
    // Make a vector of poison values, and then mark the vector as a whole as
    // poison.
    vector<shared_ptr<ConcreteVal>> elements;
    for (size_t i = 0; i < aggregate.numElementsConst(); ++i)
      elements.push_back(
          shared_ptr<ConcreteVal>(getPoisonValue(aggregate.getChild(i))));
    return new ConcreteValAggregate(true, move(elements));
  } else if (type.isPtrType()) {
    return new ConcreteValPointer(true, 0, 0);
  } else {
    // unsupported
    setUnsupported("type for poison");
    return nullptr;
  }
}

const llvm::fltSemantics *
Interpreter::getFloatSemantics(const IR::FloatType &type) {
  switch (type.getFpType()) {
  case IR::FloatType::Half:
    return &llvm::APFloat::IEEEhalf();
  case IR::FloatType::Float:
    return &llvm::APFloat::IEEEsingle();
  case IR::FloatType::Double:
    return &llvm::APFloat::IEEEdouble();
  case IR::FloatType::Quad:
    return &llvm::APFloat::IEEEquad();
  default:
    return nullptr;
  }
}

shared_ptr<ConcreteVal> Interpreter::getConstantValue(const Value &i) {
  if (dynamic_cast<const PoisonValue *>(&i)) {
    return shared_ptr<ConcreteVal>(Interpreter::getPoisonValue(i.getType()));
  } else if (dynamic_cast<const UndefValue *>(&i)) {
    // TODO
    return shared_ptr<ConcreteVal>(Interpreter::getPoisonValue(i.getType()));
  } else if (dynamic_cast<const NullPointerValue *>(&i)) {
    return make_shared<ConcreteValPointer>(false, 0, 0);
  } else if (auto const_ptr = dynamic_cast<const IntConst *>(&i)) {
    // need to do this for constants that are larger than i64
    if (const_ptr->getString()) {
      return make_shared<ConcreteValInt>(
          false,
          llvm::APInt(i.getType().bits(), *(const_ptr->getString()), 10));
    } else if (const_ptr->getInt()) {
      return make_shared<ConcreteValInt>(
          false, llvm::APInt(i.getType().bits(), *(const_ptr->getInt())));
    } else {
      UNREACHABLE();
    }
  } else if (auto const_ptr = dynamic_cast<const FloatConst *>(&i)) {
    assert((const_ptr->bits() == 32) || (const_ptr->bits() == 64) ||
           (const_ptr->bits() == 16)); // TODO: add support for other FP types
    if (auto double_float = const_ptr->getDouble()) {
      if (const_ptr->bits() == 32) {
        // Since the original ir was representable using float, casting the
        // double back to float should be safe I think but this looks pretty
        // bad and should think of a better solution
        return make_shared<ConcreteValFloat>(
            false, llvm::APFloat(static_cast<float>(*double_float)));
      } else if (const_ptr->bits() == 64) {
        return make_shared<ConcreteValFloat>(false,
                                             llvm::APFloat(*double_float));
      } else {
        UNREACHABLE();
      }
    } else if (auto string_float = const_ptr->getString()) {
      auto i = llvm::APInt(const_ptr->bits(), *string_float, 10);
      const llvm::fltSemantics *semantics;
      switch (const_ptr->getType().getAsFloatType()->getFpType()) {
      case FloatType::Half:
        semantics = &llvm::APFloat::IEEEhalf();
        break;
      case FloatType::Float:
        semantics = &llvm::APFloat::IEEEsingle();
        break;
      case FloatType::Double:
        semantics = &llvm::APFloat::IEEEdouble();
        break;
      case FloatType::Quad:
        semantics = &llvm::APFloat::IEEEquad();
        break;
      case FloatType::BFloat:
        semantics = &llvm::APFloat::BFloat();
        break;
      case FloatType::Unknown:
      default:
        setUnsupported("unknown float constant type");
        return nullptr;
      }
      return make_shared<ConcreteValFloat>(false, llvm::APFloat(*semantics, i));
    } else {
      UNREACHABLE();
    }
  } else if (auto agg_value = dynamic_cast<const AggregateValue *>(&i)) {
    auto &values = agg_value->getVals();
    size_t values_i = 0;
    vector<shared_ptr<ConcreteVal>> elements;
    auto &type = static_cast<const AggregateType &>(agg_value->getType());
    for (size_t j = 0; j < type.numElementsConst(); ++j) {
      if (type.isPadding(j)) {
        // TODO: initialize with undef
        elements.emplace_back(make_shared<ConcreteValInt>(
            false, llvm::APInt(type.getChild(j).bits(), 0)));
      } else {
        elements.emplace_back(getConstantValue(*values[values_i++]));
      }
    }
    return make_shared<ConcreteValAggregate>(false, move(elements));
  } else if (dynamic_cast<const GlobalVariable *>(&i)) {
    setUnsupported("global variable used as constant");
    return nullptr;
  } else {
    setUnsupported("unknown class used as constant");
    return nullptr;
  }
}

Interpreter::Interpreter() {}

void Interpreter::start(Function &f) {
  // TODO need to check for Value subclasses for inputs and constants
  // i.e. PoisonValue, UndefValue, and etc.
  // initialize inputs with concrete values

  concrete_vals[&Value::voidVal] = make_shared<ConcreteValVoid>();

  unsigned input_i = 0;
  for (auto &i : f.getInputs()) {
    assert(!concrete_vals.count(&i));
    auto new_val = getInputValue(input_i++, *dynamic_cast<const Input *>(&i));
    assert(new_val || unsupported_flag);
    concrete_vals.emplace(&i, new_val);
  }
  for (auto &i : f.getConstants()) {
    assert(!concrete_vals.count(&i));
    auto new_val = getConstantValue(i);
    assert(new_val || unsupported_flag);
    concrete_vals.emplace(&i, new_val);
  }
  for (auto &i : f.getUndefs()) {
    assert(!concrete_vals.count(&i));
    auto new_val = getConstantValue(i);
    assert(new_val || unsupported_flag);
    concrete_vals.emplace(&i, new_val);
  }
  for (auto &i : f.getPredicates()) {
    (void)i;
    setUnsupported("function predicates are not supported");
    return;
  }
  for (auto &i : f.getAggregates()) {
    (void)i;
    setUnsupported("function aggregates are not supported");
    return;
  }

  // TODO add stack for alloca

  for (auto &bb : f.getBBs()) {
    if (&f.getFirstBB() == bb) {
      cur_block = bb;
      break;
    }
  }
}

void Interpreter::step() {
  // Interpreter returns with a message as soon as it encounters undef
  // Hence we only need to deal with defined and poison vals during
  // interpretation
  if (isFinished())
    return;
  auto &i = *(cur_block->instrs().begin() + pos_in_block++);
  // TODO need to print only when in verbose mode
  cout << "cur inst: ";
  i.print(cout);
  cout << '\n';
  // cout << "cur mem: \n";
  // printMemory(cout);
  // cout << "\n";
  if (dynamic_cast<const Return *>(&i)) {
    auto v_op = i.operands();
    assert(v_op.size() == 1);
    assert(concrete_vals.find(v_op[0]) != concrete_vals.end());
    return_value = concrete_vals[v_op[0]].get();
    cout << "Interpreter return result:" << '\n';
    return_value->print();
    cur_block = nullptr;
    return;
  } else if (dynamic_cast<const JumpInstr *>(&i)) {
    // cout << "jump inst: " << i << '\n';
    if (auto br_ptr = dynamic_cast<const Branch *>(&i)) {
      if (!br_ptr->getCondPtr()) { // unconditional branch
        // cout << "unconditional branch" << '\n';
        assert(br_ptr->getTruePtr());
        pred_block = cur_block;
        cur_block = br_ptr->getTruePtr();
        pos_in_block = 0;
        return;
      } else {
        assert(br_ptr->getTruePtr() && br_ptr->getFalsePtr());
        // cout << "conditional branch" << '\n';
        // lookup the concrete value of cond from concrete_vals
        // if true set cur_block to dst_true BB else dst_false

        auto I = concrete_vals.find(br_ptr->getCondPtr());
        assert(
            I !=
            concrete_vals.end()); // condition must be evaluated at this point
        auto concrete_cond_val = I->second.get();
        if (concrete_cond_val->isPoison()) {
          UB_flag = true;
          cout << "branch condition val is poison. This results in UB" << '\n';
          return;
        } else {
          pred_block = cur_block;
          auto condVar = dynamic_cast<ConcreteValInt *>(concrete_cond_val);
          assert(condVar);
          cur_block = condVar->getBoolVal() ? br_ptr->getTruePtr()
                                            : br_ptr->getFalsePtr();
          pos_in_block = 0;
        }
      }
    }
  } else if (auto phi_ptr = dynamic_cast<const Phi *>(&i)) {
    assert(pred_block);
    const auto &phi_vals = phi_ptr->getValues();
    // read pred_block to determine which phi value to choose
    for (auto &[phi_val_ptr, phi_label] : phi_vals) {
      // What to do if the phi_val is poison?
      if (pred_block->getName() != phi_label)
        continue;
      auto phi_val_concrete_I = concrete_vals.find(phi_val_ptr);
      assert(phi_val_concrete_I != concrete_vals.end());
      concrete_vals[phi_ptr] = phi_val_concrete_I->second;
      break;
    }
  } else if (dynamic_cast<const Store *>(&i)) {
    i.concreteEval(*this);
  } else {
    auto res_val = i.concreteEval(*this);
    assert(res_val || unsupported_flag || UB_flag);
    concrete_vals[&i] = res_val;
  }
}

void Interpreter::run(unsigned instr_limit) {
  while (instr_limit--) {
    if (isFinished())
      return;
    step();
  }
}

Interpreter::~Interpreter() {}

void interp(Function &f) {
  cout << "running interp.cpp" << '\n';
  Interpreter interpreter;
  interpreter.start(f);

  cout << "---run Interpreter---" << '\n';
  interpreter.run();
  if (!interpreter.isFinished()) {
    cout << "ERROR: Interpreter reached instruction limit" << '\n';
    exit(EXIT_FAILURE);
  }
  if (interpreter.isUnsupported()) {
    exit(EXIT_FAILURE);
  }

  cout << "---Interpreter done---" << '\n';
}

} // namespace util
