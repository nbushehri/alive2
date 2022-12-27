#pragma once

// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "util/compiler.h"
#include "ir/value.h"
#include "ir/type.h"
#include "ir/instr.h"
#include <memory>
#include <variant>
#include <iostream>
#include <vector>
#include <utility>

namespace IR {
  struct FastMathFlags;
}

namespace util {
  class ConcreteVal{
  protected:
    enum Flags {
      None = 0 , Poison = 1 << 0, Undef = 1 << 1
    };
    unsigned flags = Flags::None;
    //bool poison;
    //std::variant<llvm::APInt, llvm::APFloat> val;
  public:
    //ConcreteVal(): flags(Flags::None), val() {}
    ConcreteVal(bool poison);
    //ConcreteVal(bool poison, llvm::APInt val);
    //ConcreteVal(bool poison, llvm::APFloat val);
    ConcreteVal(ConcreteVal& l) = default;
    ConcreteVal& operator=(ConcreteVal& l) = default;
    ConcreteVal( ConcreteVal&& r) = default;
    ConcreteVal& operator=(ConcreteVal&& r) = default;
    virtual ~ConcreteVal();
    virtual void setPoison(bool poison);
    virtual void setUndef();
    virtual bool isPoison();
    virtual bool isUndef();
    //void setVal(ConcreteVal& v);
    //void setVal(llvm::APInt& v);
    //void setVal(llvm::APFloat& v);
    //virtual ConcreteVal& getVal();
    //virtual llvm::APFloat& getValFloat() = 0;
    virtual void print();
  };

  class ConcreteValInt : public ConcreteVal {
    private:
    llvm::APInt val;
    public:
    //ConcreteValInt(bool poison, llvm::APInt val);
    ConcreteValInt(bool poison, llvm::APInt &&val);
    llvm::APInt getVal();
    void setVal(llvm::APInt& v);
    bool getBoolVal();
    void print() override;

    static ConcreteVal* evalPoison(ConcreteVal* op1, ConcreteVal* op2, ConcreteVal* op3);
    static ConcreteVal* evalPoison(ConcreteVal* lhs, ConcreteVal* rhs);
    static ConcreteVal* evalPoison(ConcreteVal* op);
    static ConcreteVal* add(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* sub(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* mul(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* sdiv(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags, bool& UB_flag);
    static ConcreteVal* udiv(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags, bool& UB_flag);
    static ConcreteVal* srem(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags, bool& UB_flag);
    static ConcreteVal* urem(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags, bool& UB_flag);
    static ConcreteVal* sAddSat(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* uAddSat(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* sSubSat(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* uSubSat(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* sShlSat(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* uShlSat(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* andOp(ConcreteVal* lhs, ConcreteVal* rhs);
    static ConcreteVal* orOp(ConcreteVal* lhs, ConcreteVal* rhs);
    static ConcreteVal* xorOp(ConcreteVal* lhs, ConcreteVal* rhs);
    static ConcreteVal* abs(ConcreteVal* lhs, ConcreteVal* rhs);
    static ConcreteVal* lshr(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* ashr(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* shl(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* cttz(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* ctlz(ConcreteVal* lhs, ConcreteVal* rhs, unsigned flags);
    static ConcreteVal* ctpop(ConcreteVal* op);
    static ConcreteVal* bitreverse(ConcreteVal* op);
    static ConcreteVal* bswap(ConcreteVal* op);
    static ConcreteVal* iTrunc(ConcreteVal* op, unsigned tgt_bitwidth);
    static ConcreteVal* zext(ConcreteVal* op, unsigned tgt_bitwidth);
    static ConcreteVal* sext(ConcreteVal* op, unsigned tgt_bitwidth);
    static ConcreteVal* select(ConcreteVal* cond, ConcreteVal* a, ConcreteVal* b);
    static ConcreteVal* icmp(ConcreteVal* a, ConcreteVal* b, unsigned cond);
  }; 

  class ConcreteValFloat : public ConcreteVal {
    private:
    llvm::APFloat val;

    static ConcreteVal* evalPoison(ConcreteVal* op1, ConcreteVal* op2, ConcreteVal* op3);
    static ConcreteVal* evalPoison(ConcreteVal* lhs, ConcreteVal* rhs);
    static ConcreteVal* evalPoison(ConcreteVal* op);
    static ConcreteVal* evalFmath(ConcreteVal* lhs, ConcreteVal* rhs, IR::FastMathFlags fmath);
    static ConcreteVal* evalFmath(ConcreteVal* op, IR::FastMathFlags fmath);
    static void unOPEvalFmath(ConcreteVal* n, IR::FastMathFlags fmath);
    public:
    void setVal(llvm::APFloat& v);
    //ConcreteValInt(bool poison, llvm::APInt val);
    ConcreteValFloat(bool poison, llvm::APFloat &&val);
    llvm::APFloat getVal();
    void print() override;

    static ConcreteVal* fadd(ConcreteVal* lhs, ConcreteVal* rhs, IR::FastMathFlags fmath);
    static ConcreteVal* fabs(ConcreteVal* op, IR::FastMathFlags fmath);
    static ConcreteVal* fneg(ConcreteVal* op, const IR::FastMathFlags& fmath);
    static ConcreteVal* ceil(ConcreteVal* op, const IR::FastMathFlags& fmath);
    static ConcreteVal* floor(ConcreteVal* op, const IR::FastMathFlags& fmath);
    static ConcreteVal* round(ConcreteVal* op, const IR::FastMathFlags& fmath);
    static ConcreteVal* roundEven(ConcreteVal* op, const IR::FastMathFlags& fmath);
    static ConcreteVal* trunc(ConcreteVal* op, const IR::FastMathFlags& fmath);
    static ConcreteVal* fsub(ConcreteVal* lhs, ConcreteVal* rhs, IR::FastMathFlags fmath);
    static ConcreteVal* fmul(ConcreteVal* lhs, ConcreteVal* rhs, IR::FastMathFlags fmath);
    static ConcreteVal* fdiv(ConcreteVal* lhs, ConcreteVal* rhs, IR::FastMathFlags fmath);
    static ConcreteVal* frem(ConcreteVal* lhs, ConcreteVal* rhs, IR::FastMathFlags fmath);
    static ConcreteVal* fmax(ConcreteVal* lhs, ConcreteVal* rhs, IR::FastMathFlags fmath);
    static ConcreteVal* fmin(ConcreteVal* lhs, ConcreteVal* rhs, IR::FastMathFlags fmath);
    static ConcreteVal* fmaximum(ConcreteVal* lhs, ConcreteVal* rhs, IR::FastMathFlags fmath);
    static ConcreteVal* fminimum(ConcreteVal* lhs, ConcreteVal* rhs, IR::FastMathFlags fmath);
    static ConcreteVal* fma(ConcreteVal* a, ConcreteVal* b, ConcreteVal* c);
    static ConcreteVal* select(ConcreteVal* cond, ConcreteVal* a, ConcreteVal* b);
  };

  class ConcreteValVect : public ConcreteVal {
  private:
  std::vector<ConcreteVal*> elements;
  public:
    
    ConcreteValVect(bool poison, std::vector<ConcreteVal*> &&elements);
    ConcreteValVect(bool poison, const IR::Value* vect_val);
    //ConcreteValVect(ConcreteValVect &l);
    //ConcreteValVect& operator=(ConcreteValVect &l);
    //ConcreteValVect( ConcreteValVect &&r);
    //ConcreteValVect& operator=(ConcreteValVect &&r);
    //ConcreteValVect(bool poison, std::vector<ConcreteVal*> &elements);
    ConcreteValVect& getVal();
    virtual ~ConcreteValVect();
    static std::vector<ConcreteVal*> make_elements(const IR::Value* vect_val);
    static std::unique_ptr<std::vector<ConcreteVal*>> make_elements_unique(IR::Value* vect_val);
    void print() override;
  };

  
  
}