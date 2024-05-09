// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <sstream>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/strings/str_cat.h"
#include "benchmark/benchmark.h"
#include "components/data_server/cache/cache.h"
#include "components/data_server/cache/key_value_cache.h"
#include "components/query/ast.h"
#include "components/query/driver.h"
#include "components/query/scanner.h"
#include "components/query/sets.h"
#include "components/tools/benchmarks/benchmark_util.h"
#include "components/tools/util/configure_telemetry_tools.h"

#include "roaring.hh"

ABSL_FLAG(int64_t, set_size, 1000, "Number of elements in a set.");
ABSL_FLAG(std::string, query, "(A - B) | (C & D)", "Query to evaluate");
ABSL_FLAG(uint32_t, range_min, 0, "Minimum element in a set");
ABSL_FLAG(uint32_t, range_max, 65536, "Maximum element in a set");

namespace kv_server {
namespace {

using RoaringBitSet = roaring::Roaring;
using StringSet = absl::flat_hash_set<std::string_view>;

constexpr std::array<std::string_view, 4> kSetNames = {"A", "B", "C", "D"};

std::unique_ptr<GetKeyValueSetResult> STRING_SET_RESULT = nullptr;
std::unique_ptr<GetKeyValueSetResult> UINT32_SET_RESULT = nullptr;

template <typename ValueT>
ValueT Lookup(std::string_view);

template <>
StringSet Lookup(std::string_view key) {
  return STRING_SET_RESULT->GetValueSet(key);
}

template <>
RoaringBitSet Lookup(std::string_view key) {
  return UINT32_SET_RESULT->GetUInt32ValueSet(key)->GetValuesBitSet();
}

Driver* GetDriver() {
  static auto* const driver = std::make_unique<Driver>().release();
  return driver;
}

Cache* GetKeyValueCache() {
  static auto* const cache = KeyValueCache::Create().release();
  return cache;
}

void SetUpKeyValueCache(int64_t set_size, uint32_t range_min,
                        uint32_t range_max) {
  kv_server::benchmark::BenchmarkLogContext log_context;
  for (const auto& set_name : kSetNames) {
    auto nums = std::vector<uint32_t>();
    nums.reserve(set_size);
    for (int i = 0; i < set_size; i++) {
      nums.push_back(range_min + (std::rand() % range_max - range_min));
    }
    GetKeyValueCache()->UpdateKeyValueSet(log_context, set_name,
                                          absl::MakeSpan(nums), 1);

    auto strings = std::vector<std::string>();
    std::transform(nums.begin(), nums.end(), std::back_inserter(strings),
                   [](uint32_t elem) { return absl::StrCat(elem); });
    auto string_views = std::vector<std::string_view>();
    std::transform(strings.begin(), strings.end(),
                   std::back_inserter(string_views),
                   [](std::string_view elem) { return elem; });
    GetKeyValueCache()->UpdateKeyValueSet(log_context, set_name,
                                          absl::MakeSpan(string_views), 1);
  }
}

template <typename SetLookupFn>
auto GetSetResult(SetLookupFn&& lookup_fn) {
  RequestContext request_context;
  return lookup_fn(request_context,
                   StringSet(kSetNames.begin(), kSetNames.end()));
}

template <typename ValueT>
void BM_SetUnion(::benchmark::State& state) {
  for (auto _ : state) {
    auto left = Lookup<ValueT>("A");
    auto right = Lookup<ValueT>("B");
    auto result = Union<ValueT>(std::move(left), std::move(right));
    ::benchmark::DoNotOptimize(result);
  }
  state.counters["Ops/s"] =
      ::benchmark::Counter(state.iterations(), ::benchmark::Counter::kIsRate);
}

template <typename ValueT>
void BM_SetDifference(::benchmark::State& state) {
  for (auto _ : state) {
    auto left = Lookup<ValueT>("A");
    auto right = Lookup<ValueT>("B");
    auto result = Difference<ValueT>(std::move(left), std::move(right));
    ::benchmark::DoNotOptimize(result);
  }
  state.counters["Ops/s"] =
      ::benchmark::Counter(state.iterations(), ::benchmark::Counter::kIsRate);
}

template <typename ValueT>
void BM_SetIntersection(::benchmark::State& state) {
  for (auto _ : state) {
    auto left = Lookup<ValueT>("A");
    auto right = Lookup<ValueT>("B");
    auto result = Intersection<ValueT>(std::move(left), std::move(right));
    ::benchmark::DoNotOptimize(result);
  }
  state.counters["Ops/s"] =
      ::benchmark::Counter(state.iterations(), ::benchmark::Counter::kIsRate);
}

template <typename ValueT>
void BM_AstTreeEvaluation(::benchmark::State& state) {
  const auto* ast_tree = GetDriver()->GetRootNode();
  for (auto _ : state) {
    auto result = Eval<ValueT>(*ast_tree, Lookup<ValueT>);
    ::benchmark::DoNotOptimize(result);
  }
  state.counters["QueryEvals/s"] =
      ::benchmark::Counter(state.iterations(), ::benchmark::Counter::kIsRate);
}

}  // namespace
}  // namespace kv_server

BENCHMARK(kv_server::BM_SetUnion<kv_server::RoaringBitSet>);
BENCHMARK(kv_server::BM_SetUnion<kv_server::StringSet>);
BENCHMARK(kv_server::BM_SetDifference<kv_server::RoaringBitSet>);
BENCHMARK(kv_server::BM_SetDifference<kv_server::StringSet>);
BENCHMARK(kv_server::BM_SetIntersection<kv_server::RoaringBitSet>);
BENCHMARK(kv_server::BM_SetIntersection<kv_server::StringSet>);
BENCHMARK(kv_server::BM_AstTreeEvaluation<kv_server::RoaringBitSet>);
BENCHMARK(kv_server::BM_AstTreeEvaluation<kv_server::StringSet>);

using kv_server::ConfigureTelemetryForTools;
using kv_server::GetKeyValueCache;
using kv_server::GetSetResult;
using kv_server::RequestContext;
using kv_server::RoaringBitSet;
using kv_server::SetUpKeyValueCache;
using kv_server::StringSet;

int main(int argc, char** argv) {
  // Initialize the environment and flags
  absl::InitializeLog();
  ::benchmark::Initialize(&argc, argv);
  absl::ParseCommandLine(argc, argv);
  ConfigureTelemetryForTools();
  auto range_min = absl::GetFlag(FLAGS_range_min);
  auto range_max = absl::GetFlag(FLAGS_range_max);
  if (range_max <= range_min) {
    ABSL_LOG(ERROR) << "range_max: " << range_max
                    << " must be greater than range_min: " << range_min;
    return -1;
  }
  // Set up the cache and the ast tree.
  SetUpKeyValueCache(absl::GetFlag(FLAGS_set_size), range_min, range_max);
  kv_server::STRING_SET_RESULT = GetSetResult(
      [](const RequestContext& request_context, const StringSet& keys) {
        return GetKeyValueCache()->GetKeyValueSet(request_context, keys);
      });
  kv_server::UINT32_SET_RESULT = GetSetResult(
      [](const RequestContext& request_context, const StringSet& keys) {
        return GetKeyValueCache()->GetUInt32ValueSet(request_context, keys);
      });

  std::istringstream stream(absl::GetFlag(FLAGS_query));
  kv_server::Scanner scanner(stream);
  kv_server::Parser parser(*kv_server::GetDriver(), scanner);
  parser();

  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}
