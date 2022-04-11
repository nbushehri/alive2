#pragma once

// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "ir/instr.h"
#include "ir/type.h"
#include "ir/value.h"
#include "util/compiler.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include <iostream>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace IR {
struct FastMathFlags;
}

namespace util {
class Interpreter;
class ConcreteVal {
protected:
  enum Flags { None = 0, Poison = 1 << 0, Undef = 1 << 1 };
  unsigned flags = Flags::None;
  virtual bool equals(ConcreteVal &rhs) {
    return flags == rhs.flags;
  }

public:
  // ConcreteVal(): flags(Flags::None), val() {}
  ConcreteVal(bool poison);
  // ConcreteVal(bool poison, llvm::APInt val);
  // ConcreteVal(bool poison, llvm::APFloat val);
  ConcreteVal(ConcreteVal &l) = default;
  ConcreteVal &operator=(ConcreteVal &l) = default;
  ConcreteVal(ConcreteVal &&r) = default;
  ConcreteVal &operator=(ConcreteVal &&r) = default;
  virtual ~ConcreteVal();
  virtual void setPoison(bool poison);
  virtual void setUndef();
  virtual bool isPoison() const;
  virtual bool isUndef() const;
  virtual void print();
  bool operator==(ConcreteVal &rhs) {
    return equals(rhs);
  }
};

class ConcreteValVoid : public ConcreteVal {
public:
  ConcreteValVoid();
  void print() override;
};

class ConcreteValInt : public ConcreteVal {
private:
  llvm::APInt val;

protected:
  virtual bool equals(ConcreteVal &rhs) override {
    auto int_rhs = static_cast<ConcreteValInt &>(rhs);
    if (val != int_rhs.val)
      return false;
    return ConcreteVal::equals(rhs);
  }

public:
  ConcreteValInt(bool poison, llvm::APInt &&val);
  llvm::APInt getVal() const;
  void setVal(llvm::APInt &v);
  bool getBoolVal() const;
  void print() override;

  static ConcreteVal *evalPoison(ConcreteVal *op1, ConcreteVal *op2,
                                 ConcreteVal *op3);
  static ConcreteVal *evalPoison(ConcreteVal *lhs, ConcreteVal *rhs);
  static ConcreteVal *evalPoison(ConcreteVal *op);
  static ConcreteVal *add(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags);
  static ConcreteVal *sub(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags);
  static ConcreteVal *mul(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags);
  static ConcreteVal *sdiv(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags,
                           bool &UB_flag);
  static ConcreteVal *udiv(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags,
                           bool &UB_flag);
  static ConcreteVal *srem(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags,
                           bool &UB_flag);
  static ConcreteVal *urem(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags,
                           bool &UB_flag);
  static ConcreteVal *sAddSat(ConcreteVal *lhs, ConcreteVal *rhs,
                              unsigned flags);
  static ConcreteVal *uAddSat(ConcreteVal *lhs, ConcreteVal *rhs,
                              unsigned flags);
  static ConcreteVal *sSubSat(ConcreteVal *lhs, ConcreteVal *rhs,
                              unsigned flags);
  static ConcreteVal *uSubSat(ConcreteVal *lhs, ConcreteVal *rhs,
                              unsigned flags);
  static ConcreteVal *sShlSat(ConcreteVal *lhs, ConcreteVal *rhs,
                              unsigned flags);
  static ConcreteVal *uShlSat(ConcreteVal *lhs, ConcreteVal *rhs,
                              unsigned flags);
  static ConcreteVal *andOp(ConcreteVal *lhs, ConcreteVal *rhs);
  static ConcreteVal *orOp(ConcreteVal *lhs, ConcreteVal *rhs);
  static ConcreteVal *xorOp(ConcreteVal *lhs, ConcreteVal *rhs);
  static ConcreteVal *abs(ConcreteVal *lhs, ConcreteVal *rhs);
  static ConcreteVal *lshr(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags);
  static ConcreteVal *ashr(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags);
  static ConcreteVal *shl(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags);
  static ConcreteVal *cttz(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags);
  static ConcreteVal *ctlz(ConcreteVal *lhs, ConcreteVal *rhs, unsigned flags);
  static ConcreteVal *ctpop(ConcreteVal *op);
  static ConcreteVal *bitreverse(ConcreteVal *op);
  static ConcreteVal *bswap(ConcreteVal *op);
  static ConcreteVal *iTrunc(ConcreteVal *op, unsigned tgt_bitwidth);
  static ConcreteVal *zext(ConcreteVal *op, unsigned tgt_bitwidth);
  static ConcreteVal *sext(ConcreteVal *op, unsigned tgt_bitwidth);
  static std::shared_ptr<ConcreteVal> select(ConcreteVal *cond,
                                             std::shared_ptr<ConcreteVal> &a,
                                             std::shared_ptr<ConcreteVal> &b);
  static ConcreteVal *icmp(ConcreteVal *a, ConcreteVal *b, unsigned cond);
};

class ConcreteValFloat : public ConcreteVal {
private:
  llvm::APFloat val;

protected:
  virtual bool equals(ConcreteVal &rhs) override {
    auto float_rhs = static_cast<ConcreteValFloat &>(rhs);
    if (val != float_rhs.val)
      return false;
    return ConcreteVal::equals(rhs);
  }
  static ConcreteVal *evalPoison(ConcreteVal *op1, ConcreteVal *op2,
                                 ConcreteVal *op3);
  static ConcreteVal *evalPoison(ConcreteVal *lhs, ConcreteVal *rhs);
  static ConcreteVal *evalPoison(ConcreteVal *op);
  static ConcreteVal *evalFmath(ConcreteVal *lhs, ConcreteVal *rhs,
                                IR::FastMathFlags fmath);
  static ConcreteVal *evalFmath(ConcreteVal *op, IR::FastMathFlags fmath);
  static void unOPEvalFmath(ConcreteVal *n, IR::FastMathFlags fmath);

public:
  void setVal(llvm::APFloat &v);
  ConcreteValFloat(bool poison, llvm::APFloat &&val);
  llvm::APFloat getVal() const;
  void print() override;

  static ConcreteVal *fadd(ConcreteVal *lhs, ConcreteVal *rhs,
                           IR::FastMathFlags fmath);
  static ConcreteVal *fabs(ConcreteVal *op, IR::FastMathFlags fmath);
  static ConcreteVal *fneg(ConcreteVal *op, const IR::FastMathFlags &fmath);
  static ConcreteVal *ceil(ConcreteVal *op, const IR::FastMathFlags &fmath);
  static ConcreteVal *floor(ConcreteVal *op, const IR::FastMathFlags &fmath);
  static ConcreteVal *round(ConcreteVal *op, const IR::FastMathFlags &fmath);
  static ConcreteVal *roundEven(ConcreteVal *op,
                                const IR::FastMathFlags &fmath);
  static ConcreteVal *trunc(ConcreteVal *op, const IR::FastMathFlags &fmath);
  static ConcreteVal *fsub(ConcreteVal *lhs, ConcreteVal *rhs,
                           IR::FastMathFlags fmath);
  static ConcreteVal *fmul(ConcreteVal *lhs, ConcreteVal *rhs,
                           IR::FastMathFlags fmath);
  static ConcreteVal *fdiv(ConcreteVal *lhs, ConcreteVal *rhs,
                           IR::FastMathFlags fmath);
  static ConcreteVal *frem(ConcreteVal *lhs, ConcreteVal *rhs,
                           IR::FastMathFlags fmath);
  static ConcreteVal *fmax(ConcreteVal *lhs, ConcreteVal *rhs,
                           IR::FastMathFlags fmath);
  static ConcreteVal *fmin(ConcreteVal *lhs, ConcreteVal *rhs,
                           IR::FastMathFlags fmath);
  static ConcreteVal *fmaximum(ConcreteVal *lhs, ConcreteVal *rhs,
                               IR::FastMathFlags fmath);
  static ConcreteVal *fminimum(ConcreteVal *lhs, ConcreteVal *rhs,
                               IR::FastMathFlags fmath);
  static ConcreteVal *fma(ConcreteVal *a, ConcreteVal *b, ConcreteVal *c);
};

// XXX: padding values are included!
class ConcreteValAggregate : public ConcreteVal {
private:
  std::vector<std::shared_ptr<ConcreteVal>> elements;

public:
  ConcreteValAggregate(bool poison,
                       std::vector<std::shared_ptr<ConcreteVal>> &&elements);
  ~ConcreteValAggregate();
  const std::vector<std::shared_ptr<ConcreteVal>> &getVal() const;
  void print() override;
};

class ConcreteValPointer : public ConcreteVal {
private:
  unsigned bid;
  std::int64_t offset;
  bool is_local{false};
  static bool icmp_cmp(llvm::APInt &lhs, llvm::APInt &rhs, unsigned cond);

protected:
  virtual bool equals(ConcreteVal &rhs) override {
    auto ptr_rhs = static_cast<ConcreteValPointer &>(rhs);
    if (bid != ptr_rhs.bid || offset != ptr_rhs.offset)
      return false;
    return ConcreteVal::equals(rhs);
  }

public:
  ConcreteValPointer();
  ConcreteValPointer(bool poison, unsigned bid, std::int64_t offset);
  unsigned getBid() const;
  bool getIsLocal() const;
  void setBid(unsigned bid);
  void setIsLocal(bool is_local);
  std::int64_t getOffset() const;
  void setOffset(std::int64_t offset);
  void print() override;

  static ConcreteVal *evalPoison(ConcreteVal *lhs, ConcreteVal *rhs);
  static ConcreteVal *icmp(ConcreteVal *a, ConcreteVal *b, unsigned cond, unsigned pcmode, Interpreter &interpreter);
};
} // namespace util
