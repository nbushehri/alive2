#pragma once

// Copyright (c) 2021-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include <jsoncons/json.hpp>

namespace util {
jsoncons::ojson evaluateAliveFunc(const jsoncons::ojson &job);
}
