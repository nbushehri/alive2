#pragma once

// Copyright (c) 2018-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include <string>

namespace util {

std::string get_random_str(unsigned num_chars);
uint32_t get_random_int32();
uint64_t get_random_int64();
float get_random_float();
double get_random_double();

}
