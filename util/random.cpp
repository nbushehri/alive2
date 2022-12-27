// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "util/random.h"
#include <random>
#include <limits>

using namespace std;

static default_random_engine re;
static mt19937 re2;
static uniform_int_distribution<uint32_t> rand_int32{0};
static uniform_int_distribution<uint64_t> rand_int64{0};
static uniform_real_distribution<float> rand_float{0.0, numeric_limits<float>::max()};
static uniform_real_distribution<double> rand_double{0.0, numeric_limits<double>::max()};

static void seed() {
  static bool seeded = false;
  if (!seeded) {
    random_device rd;
    re.seed(rd());
    re2.seed(rd());
    seeded = true;
  }
}

static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

namespace util {

string get_random_str(unsigned num_chars) {
  seed();
  uniform_int_distribution<unsigned> rand(0, sizeof(chars)-2);
  string s;
  for (unsigned i = 0; i < num_chars; ++i) {
    s += chars[rand(re)];
  }
  return s;
}

uint32_t get_random_int32() {
  seed();
  return rand_int32(re);
}

uint64_t get_random_int64() {
  seed();
  return rand_int64(re);
}

float get_random_float() {
  seed();
  return rand_float(re2);
}

double get_random_double() {
  seed();
  return rand_double(re2);
}

}
