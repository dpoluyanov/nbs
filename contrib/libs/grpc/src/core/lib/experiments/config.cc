// Copyright 2022 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <grpc/support/port_platform.h>

#include "src/core/lib/experiments/config.h"

#include <string.h>

#include <algorithm>
#include <atomic>
#include <util/generic/string.h>
#include <util/string/cast.h>

#include "y_absl/strings/ascii.h"
#include "y_absl/strings/str_cat.h"
#include "y_absl/strings/str_split.h"
#include "y_absl/strings/string_view.h"

#include <grpc/support/log.h>

#include "src/core/lib/experiments/experiments.h"
#include "src/core/lib/gprpp/crash.h"  // IWYU pragma: keep
#include "src/core/lib/gprpp/global_config.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/gprpp/no_destruct.h"

#ifndef GRPC_EXPERIMENTS_ARE_FINAL
GPR_GLOBAL_CONFIG_DEFINE_STRING(
    grpc_experiments, "",
    "List of grpc experiments to enable (or with a '-' prefix to disable).");

namespace grpc_core {

namespace {
struct Experiments {
  bool enabled[kNumExperiments];
};

struct ForcedExperiment {
  bool forced = false;
  bool value;
};
ForcedExperiment g_forced_experiments[kNumExperiments];

std::atomic<bool> g_loaded;

GPR_ATTRIBUTE_NOINLINE Experiments LoadExperimentsFromConfigVariable() {
  GPR_ASSERT(g_loaded.exchange(true, std::memory_order_relaxed) == false);
  // Set defaults from metadata.
  Experiments experiments;
  for (size_t i = 0; i < kNumExperiments; i++) {
    if (!g_forced_experiments[i].forced) {
      experiments.enabled[i] = g_experiment_metadata[i].default_value;
    } else {
      experiments.enabled[i] = g_forced_experiments[i].value;
    }
  }
  // Get the global config.
  auto experiments_str = GPR_GLOBAL_CONFIG_GET(grpc_experiments);
  // For each comma-separated experiment in the global config:
  for (auto experiment :
       y_absl::StrSplit(y_absl::string_view(experiments_str.get()), ',')) {
    // Strip whitespace.
    experiment = y_absl::StripAsciiWhitespace(experiment);
    // Handle ",," without crashing.
    if (experiment.empty()) continue;
    // Enable unless prefixed with '-' (=> disable).
    bool enable = true;
    if (experiment[0] == '-') {
      enable = false;
      experiment.remove_prefix(1);
    }
    // See if we can find the experiment in the list in this binary.
    bool found = false;
    for (size_t i = 0; i < kNumExperiments; i++) {
      if (experiment == g_experiment_metadata[i].name) {
        experiments.enabled[i] = enable;
        found = true;
        break;
      }
    }
    // If not found log an error, but don't take any other action.
    // Allows us an easy path to disabling experiments.
    if (!found) {
      gpr_log(GPR_ERROR, "Unknown experiment: %s",
              TString(experiment).c_str());
    }
  }
  return experiments;
}
}  // namespace

bool IsExperimentEnabled(size_t experiment_id) {
  // One time initialization:
  static const NoDestruct<Experiments> experiments{
      LoadExperimentsFromConfigVariable()};
  // Normal path: just return the value;
  return experiments->enabled[experiment_id];
}

void PrintExperimentsList() {
  size_t max_experiment_length = 0;
  for (size_t i = 0; i < kNumExperiments; i++) {
    max_experiment_length =
        std::max(max_experiment_length, strlen(g_experiment_metadata[i].name));
  }
  for (size_t i = 0; i < kNumExperiments; i++) {
    gpr_log(GPR_DEBUG, "%s",
            y_absl::StrCat(
                "gRPC EXPERIMENT ", g_experiment_metadata[i].name,
                TString(max_experiment_length -
                                strlen(g_experiment_metadata[i].name) + 1,
                            ' '),
                IsExperimentEnabled(i) ? "ON " : "OFF", " (default:",
                g_experiment_metadata[i].default_value ? "ON" : "OFF",
                g_forced_experiments[i].forced
                    ? y_absl::StrCat(" force:",
                                   g_forced_experiments[i].value ? "ON" : "OFF")
                    : TString(),
                ")")
                .c_str());
  }
}

void ForceEnableExperiment(y_absl::string_view experiment, bool enable) {
  GPR_ASSERT(g_loaded.load(std::memory_order_relaxed) == false);
  for (size_t i = 0; i < kNumExperiments; i++) {
    if (g_experiment_metadata[i].name != experiment) continue;
    if (g_forced_experiments[i].forced) {
      GPR_ASSERT(g_forced_experiments[i].value == enable);
    } else {
      g_forced_experiments[i].forced = true;
      g_forced_experiments[i].value = enable;
    }
    return;
  }
  gpr_log(GPR_INFO, "gRPC EXPERIMENT %s not found to force %s",
          TString(experiment).c_str(), enable ? "enable" : "disable");
}

}  // namespace grpc_core
#else
namespace grpc_core {
void PrintExperimentsList() {}
void ForceEnableExperiment(y_absl::string_view experiment_name, bool) {
  Crash(y_absl::StrCat("ForceEnableExperiment(\"", experiment_name,
                     "\") called in final build"));
}
}  // namespace grpc_core
#endif
