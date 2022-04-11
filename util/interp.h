#pragma once

// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "util/concreteval.h"

namespace IR {
class BasicBlock;
class Function;
class Input;
class Value;
} // namespace IR

namespace util {

class ConcreteVal;

using DataByteVal = std::pair<uint8_t, uint8_t>;// <8-bit nonpoison_mask, 8-bit value>

struct ConcreteByte {

  DataByteVal byte_val;
  ConcreteValPointer ptr_val;
  unsigned pointer_byte_offset;
  bool is_pointer;

  // creates an uninitialized value byte i.e. [0, 0, 0]
  ConcreteByte()
      : byte_val(DataByteVal(0,0)), pointer_byte_offset(0), is_pointer(false) {}

  ConcreteByte(ConcreteValPointer &&_value)
      : ptr_val(std::move(_value)), pointer_byte_offset(0), is_pointer(true) {}

  ConcreteByte(DataByteVal &&_value)
      : byte_val(std::move(_value)), pointer_byte_offset(0), is_pointer(false) {}

  auto &intValue() {
    assert(!is_pointer);
    return byte_val;
  }

  auto &pointerValue() {
    assert(is_pointer);
    return ptr_val;
  }

  bool operator==(ConcreteByte &rhs) {
    if (is_pointer != rhs.is_pointer)
      return false;
    if (is_pointer) {
      if (pointer_byte_offset != rhs.pointer_byte_offset)
        return false;
      return (pointerValue() == rhs.pointerValue());
    } else {
      return (intValue() == rhs.intValue());
    }
    UNREACHABLE();
  }

  void print(std::ostream &os) const {
    if (is_pointer) {
      auto is_poison = ptr_val.isPoison();
      auto nonpoison_mask = is_poison ? 0 : 255;
      auto ptr_bid = ptr_val.getBid();
      auto ptr_offset = ptr_val.getOffset();
      os << nonpoison_mask << ",[" << ptr_bid << "," << ptr_offset << ","
         << pointer_byte_offset << "]";
    } else 
      os << (uint64_t)byte_val.first << "," << (uint64_t)byte_val.second << "]";
  }

};

struct ConcreteBlock {
  uint64_t size;
  uint64_t address;
  uint64_t align;
  std::map<uint64_t, ConcreteByte> bytes;
  ConcreteByte default_byte{};

  ConcreteBlock() {}

  void print(std::ostream &os) const {
    os << "size:" << size << ", align:" << align;
    if (size == 0)
      assert(bytes.empty());
    else {
      os << ", bytes:[";

      for (auto &[index, cb] : bytes) {
        os << "[" << index << ",";
        cb.print(os);
      }
      os << "]";
    }
  }

  ConcreteByte &getByte(uint64_t index, bool &ub) {
    if (index >= size) {
      ub = true;
      return default_byte;
    }
    else 
      ub = false;
    
    if (!bytes.contains(index)) {
      auto new_byte = default_byte;
      bytes.emplace(index, std::move(new_byte));
    }
    
    return bytes[index];
  }

  bool operator==(ConcreteBlock &rhs) {
    std::cout << "operator== concreteBlock\n";
    if ((size != rhs.size) || (address != rhs.address) || (align != rhs.align))
      return false;
    if (bytes.size() != rhs.bytes.size())
      return false;
    for (auto &[index, cb] : bytes) {
      if (!rhs.bytes.contains(index))
        return false;
      if (cb != rhs.bytes[index]) {
        return false;
      }
    }
    return true;
  }
};

class Interpreter {
public:
  Interpreter();
  virtual ~Interpreter();
  void start(IR::Function &f);
  void step();
  void run(unsigned instr_limit = 100);
  void setUnsupported(std::string reason);
  virtual std::shared_ptr<ConcreteVal> getInputValue(unsigned index,
                                                     const IR::Input &input);
  std::shared_ptr<ConcreteVal> getConstantValue(const IR::Value &i);
  ConcreteVal *getPoisonValue(const IR::Type &type);
  static const llvm::fltSemantics *getFloatSemantics(const IR::FloatType &type);

  bool isReturned() const {
    return !cur_block;
  }

  bool isUndefined() const {
    return UB_flag;
  }

  bool isUnsupported() const {
    return unsupported_flag;
  }

  bool isFinished() const {
    return isReturned() || isUndefined() || isUnsupported();
  }

  void printMemory(std::ostream &os) const {
    os << "non-local blocks:\n";
    os << "[";
    for (auto &block : mem_blocks) {
      os << "{";
      block.print(os);
      os << "}";
    }
    os << "]\n";
    for (auto &l_block : local_mem_blocks) {
      os << "{";
      l_block.print(os);
      os << "}";
    }
    os << "]";

  }

  ConcreteBlock &getBlock(uint64_t index, bool is_local=false) {
    if (!is_local) {
      assert(index < mem_blocks.size() && "non_local block doesn't exist");
      return mem_blocks[index];
    }
    assert(index < local_mem_blocks.size() && "local block doesn't exist");
    return local_mem_blocks[index];
  }

  std::map<const IR::Value *, std::shared_ptr<ConcreteVal>> concrete_vals;
  const IR::BasicBlock *pred_block = nullptr;
  const IR::BasicBlock *cur_block = nullptr;
  unsigned pos_in_block = 0;
  bool UB_flag = false;
  bool unsupported_flag = false;
  std::string unsupported_reason;
  ConcreteVal *return_value = nullptr;
  std::vector<ConcreteBlock> mem_blocks;
  std::vector<ConcreteBlock> local_mem_blocks;
};

void interp(IR::Function &f);

} // namespace util
