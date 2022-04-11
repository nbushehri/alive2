#pragma once

// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include <ostream>
#include <set>
#include <string>
#include <utility>

#include "ir/state.h"
#include "ir/value.h"
#include "smt/solver.h"

namespace util {

struct AliveException {
  std::string msg;
  bool is_unsound;

public:
  AliveException(std::string &&msg, bool is_unsound)
    : msg(std::move(msg)), is_unsound(is_unsound) {}
};


class Errors {
  std::set<std::pair<std::string, bool>> errs;

public:
  Errors() = default;
  virtual ~Errors();

  void add(const char *str, bool is_unsound);
  void add(std::string &&str, bool is_unsound);
  void add(AliveException &&e);

  // These return true to continue validation.
  virtual bool addSolverError(const smt::Result &r);
  virtual bool addSolverSatApprox(const std::string &approx);
  virtual bool addSolverSat(const IR::State &src_state,
                            const IR::State &tgt_state, const smt::Result &r,
                            const IR::Value *main_var, const std::string &msg,
                            bool check_each_var, const std::string &post_msg);

  explicit operator bool() const { return !errs.empty(); }
  bool isUnsound() const;

  friend std::ostream& operator<<(std::ostream &os, const Errors &e);
};

}
