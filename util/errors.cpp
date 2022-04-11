// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "util/errors.h"

#include <set>
#include <sstream>
#include <string>

#include "ir/constant.h"
#include "ir/globals.h"

using namespace IR;
using namespace smt;
using namespace std;


namespace util {

Errors::~Errors() {}

void Errors::add(const char *str, bool is_unsound) {
  add(string(str), is_unsound);
}

void Errors::add(string &&str, bool is_unsound) {
  if (is_unsound)
    errs.clear();
  errs.emplace(std::move(str), is_unsound);
}

void Errors::add(AliveException &&e) {
  add(std::move(e.msg), e.is_unsound);
}

bool Errors::isUnsound() const {
  for (auto &[msg, unsound] : errs) {
    if (unsound)
      return true;
  }
  return false;
}

ostream& operator<<(ostream &os, const Errors &errs) {
  for (auto &[msg, unsound] : errs.errs) {
    os << "ERROR: " << msg << '\n';
  }
  return os;
}

bool Errors::addSolverSatApprox(const std::string &approx) {
  stringstream s;
  s << "Couldn't prove the correctness of the transformation\n"
       "Alive2 approximated the semantics of the programs and therefore we\n"
       "cannot conclude whether the bug found is valid or not.\n\n"
       "Approximations done:\n";
  s << approx;
  add(s.str(), false);
  return false;
}

} // namespace util
