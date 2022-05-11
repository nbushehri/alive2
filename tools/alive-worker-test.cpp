// Copyright (c) 2021-present The Alive2 Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

#include "util/version.h"
#include "util/worker.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <math.h>
#include <string>
#include <utility>
#include <vector>

#include <jsoncons/byte_string.hpp>
#include <jsoncons/json.hpp>

using namespace util;
using namespace std;
using jsoncons::byte_string_arg;
using jsoncons::byte_string_view;
using jsoncons::decode_base64;
using jsoncons::json_array_arg;
using jsoncons::json_object_arg;
using jsoncons::ojson;

static llvm::cl::OptionCategory alive_cmdargs("Alive2 worker-test options");
static llvm::cl::opt<string> test_filename(llvm::cl::Positional,
                                           llvm::cl::Required,
                                           llvm::cl::desc("<test file>"),
                                           llvm::cl::value_desc("filename"),
                                           llvm::cl::cat(alive_cmdargs));

static ojson ir2bc(llvm::StringRef ir) {
  llvm::SMDiagnostic err;
  llvm::LLVMContext context;
  auto mem_buffer = llvm::MemoryBuffer::getMemBufferCopy(ir);
  auto mod = llvm::parseIR(mem_buffer->getMemBufferRef(), err, context);
  if (!mod) {
    err.print("alive-worker-test", llvm::errs());
    exit(1);
  }
  llvm::SmallVector<char, 0> buffer;
  llvm::raw_svector_ostream stream(buffer);
  llvm::WriteBitcodeToFile(*mod, stream);
  return byte_string_view(reinterpret_cast<const uint8_t *>(buffer.data()),
                          buffer.size());
}

static ojson scalar2json(llvm::yaml::Node &node, llvm::StringRef scalar,
                         bool plain) {
  using namespace llvm::yaml;
  string tag = node.getRawTag().str();
  // https://yaml.org/spec/1.2.1/#id2805071
  if (tag == "!" || (tag.empty() && !plain)) {
    tag = "tag:yaml.org,2002:str";
  } else if (tag == "?" || (tag.empty() && plain)) {
    if (isNull(scalar)) {
      tag = "tag:yaml.org,2002:null";
    } else if (isBool(scalar)) {
      tag = "tag:yaml.org,2002:bool";
    } else if (isNumeric(scalar)) {
      if (scalar.startswith("0x") ||
          (!scalar.contains('.') && !scalar.contains('e') &&
           !scalar.contains('E')))
        tag = "tag:yaml.org,2002:int";
      else
        tag = "tag:yaml.org,2002:float";
    } else {
      tag = "tag:yaml.org,2002:str";
    }
  } else {
    tag = node.getVerbatimTag();
  }

  if (tag == "tag:yaml.org,2002:null") {
    return nullptr;
  } else if (tag == "tag:yaml.org,2002:bool") {
    bool result;
    auto err = ScalarTraits<bool>::input(scalar, nullptr, result);
    if (err.empty())
      return result;
  } else if (tag == "tag:yaml.org,2002:int") {
    uint64_t result_unsigned;
    auto err = ScalarTraits<uint64_t>::input(scalar, nullptr, result_unsigned);
    if (err.empty())
      return result_unsigned;
    int64_t result_signed;
    err = ScalarTraits<int64_t>::input(scalar, nullptr, result_signed);
    if (err.empty())
      return result_signed;
  } else if (tag == "tag:yaml.org,2002:float") {
    if (scalar == ".nan" || scalar == ".NaN" || scalar == ".NAN")
      return NAN;
    if (scalar == ".inf" || scalar == ".Inf" || scalar == ".INF")
      return INFINITY;
    if (scalar == "+.inf" || scalar == "+.Inf" || scalar == "+.INF")
      return INFINITY;
    if (scalar == "-.inf" || scalar == "-.Inf" || scalar == "-.INF")
      return -INFINITY;
    double result_float;
    auto err = ScalarTraits<double>::input(scalar, nullptr, result_float);
    if (err.empty())
      return result_float;
  } else if (tag == "tag:yaml.org,2002:str") {
    return string_view(scalar);
  } else if (tag == "tag:yaml.org,2002:binary") {
    string str;
    for (char c : scalar) {
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\v' ||
          c == '\f')
        continue;
      str.push_back(c);
    }
    vector<uint8_t> bytes;
    auto err = decode_base64(str.begin(), str.end(), bytes);
    if (err.ec != jsoncons::conv_errc::success) {
      cerr << "invalid base64\n";
      exit(1);
    }
    return ojson(byte_string_arg, bytes);
  } else if (tag == "!ir") {
    return ir2bc(scalar);
  }
  cerr << "unsupported scalar \"" << scalar.str() << "\" with tag \"" << tag
       << "\"\n";
  exit(1);
}

static ojson yaml2json(llvm::yaml::Node &node) {
  using namespace llvm::yaml;
  switch (node.getType()) {
  case Node::NK_Null:
    return nullptr;
  case Node::NK_Scalar: {
    bool plain = true;
    auto raw = static_cast<ScalarNode &>(node).getRawValue();
    if (raw.startswith("\"") || raw.startswith("'"))
      plain = false;
    llvm::SmallVector<char, 32> storage;
    auto val = static_cast<ScalarNode &>(node).getValue(storage);
    return scalar2json(node, val, plain);
  }
  case Node::NK_BlockScalar:
    return scalar2json(node, static_cast<BlockScalarNode &>(node).getValue(),
                       false);
  case Node::NK_Sequence: {
    ojson::array result;
    for (auto &item : static_cast<SequenceNode &>(node))
      result.emplace_back(yaml2json(item));
    return result;
  }
  case Node::NK_Mapping: {
    ojson::object result;
    for (auto &item : static_cast<MappingNode &>(node)) {
      auto &key = *item.getKey();
      string key_str;
      if (key.getType() == Node::NK_Scalar) {
        llvm::SmallVector<char, 32> storage;
        auto key_val = static_cast<ScalarNode &>(key).getValue(storage);
        key_str = key_val.str();
      } else if (key.getType() == Node::NK_BlockScalar) {
        key_str = static_cast<BlockScalarNode &>(key).getValue().str();
      } else {
        cerr << "unsupported type in YAML key\n";
        exit(1);
      }
      result.insert_or_assign(key_str, yaml2json(*item.getValue()));
    }
    return result;
  }
  default:
    cerr << "unsupported YAML type\n";
    exit(1);
  }
}

bool results_equal(const ojson &expected, const ojson &actual) {
  if (expected.is_object() && actual.is_object()) {
    // Ignore keys in actual which are missing from expected.
    // That way, tests can ignore some of an object's fields.
    for (const auto &item : expected.object_range()) {
      if (!actual.contains(item.key()))
        return false;
      if (!results_equal(item.value(), actual[item.key()]))
        return false;
    }
    return true;
  } else if (expected.is_array() && actual.is_array()) {
    // Recurse into results_equal, in case there are nested objects.
    if (expected.size() != actual.size())
      return false;
    for (size_t i = 0; i < expected.size(); ++i)
      if (!results_equal(expected[i], actual[i]))
        return false;
    return true;
  } else {
    return expected == actual;
  }
}

int main(int argc, char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm::EnableDebugBuffering = true;
  llvm::llvm_shutdown_obj llvm_shutdown; // Call llvm_shutdown() on exit.

  std::string Usage =
      R"EOF(Alive2 worker test program:
version )EOF";
  Usage += alive_version;
  Usage += R"EOF(
see alive-worker-test --version  for LLVM version info,

This program evaluates Alive2-related calls from a file and checks that the
results are correct.
)EOF";

  llvm::cl::HideUnrelatedOptions(alive_cmdargs);
  llvm::cl::ParseCommandLineOptions(argc, argv, Usage);

  auto buf_or_err = llvm::MemoryBuffer::getFileOrSTDIN(test_filename);
  if (!buf_or_err)
    return 1;
  llvm::SourceMgr source_mgr;
  llvm::yaml::Stream stream((*buf_or_err)->getBuffer(), source_mgr);
  for (auto &document : stream) {
    ojson job = yaml2json(*document.getRoot());
    ojson actual = evaluateAliveFunc(job);
    const ojson &expected = job["expected"];
    if (results_equal(expected, actual)) {
      cerr << "test " << job["name"] << " passed\n";
    } else {
      jsoncons::json_options options;
      options.nan_to_num(".nan");
      options.inf_to_num(".inf");
      options.neginf_to_num("-.inf");
      cerr << "expected: " << jsoncons::print(expected, options) << "\n";
      cerr << "actual: " << jsoncons::print(actual, options) << "\n";
      cerr << "test " << job["name"] << " failed\n";
      return 1;
    }
  }

  return 0;
}
