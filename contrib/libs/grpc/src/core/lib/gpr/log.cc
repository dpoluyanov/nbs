//
//
// Copyright 2015 gRPC authors.
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
//
//

#include <grpc/support/port_platform.h>

#include <stdio.h>
#include <string.h>

#include "y_absl/strings/str_cat.h"

#include <grpc/support/alloc.h>
#include <grpc/support/atm.h>
#include <grpc/support/log.h>

#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/crash.h"
#include "src/core/lib/gprpp/global_config.h"

#ifndef GPR_DEFAULT_LOG_VERBOSITY_STRING
#define GPR_DEFAULT_LOG_VERBOSITY_STRING "ERROR"
#endif  // !GPR_DEFAULT_LOG_VERBOSITY_STRING

GPR_GLOBAL_CONFIG_DEFINE_STRING(grpc_verbosity,
                                GPR_DEFAULT_LOG_VERBOSITY_STRING,
                                "Default gRPC logging verbosity")
GPR_GLOBAL_CONFIG_DEFINE_STRING(grpc_stacktrace_minloglevel, "",
                                "Messages logged at the same or higher level "
                                "than this will print stacktrace")

static constexpr gpr_atm GPR_LOG_SEVERITY_UNSET = GPR_LOG_SEVERITY_ERROR + 10;
static constexpr gpr_atm GPR_LOG_SEVERITY_NONE = GPR_LOG_SEVERITY_ERROR + 11;

void gpr_default_log(gpr_log_func_args* args);
static gpr_atm g_log_func = reinterpret_cast<gpr_atm>(gpr_default_log);
static gpr_atm g_min_severity_to_print = GPR_LOG_SEVERITY_UNSET;
static gpr_atm g_min_severity_to_print_stacktrace = GPR_LOG_SEVERITY_UNSET;

void gpr_unreachable_code(const char* reason, const char* file, int line) {
  grpc_core::Crash(y_absl::StrCat("UNREACHABLE CODE: ", reason),
                   grpc_core::SourceLocation(file, line));
}

void gpr_assertion_failed(const char* filename, int line, const char* message) {
  grpc_core::Crash(y_absl::StrCat("ASSERTION FAILED: ", message),
                   grpc_core::SourceLocation(filename, line));
}

const char* gpr_log_severity_string(gpr_log_severity severity) {
  switch (severity) {
    case GPR_LOG_SEVERITY_DEBUG:
      return "D";
    case GPR_LOG_SEVERITY_INFO:
      return "I";
    case GPR_LOG_SEVERITY_ERROR:
      return "E";
  }
  GPR_UNREACHABLE_CODE(return "UNKNOWN");
}

int gpr_should_log(gpr_log_severity severity) {
  return static_cast<gpr_atm>(severity) >=
                 gpr_atm_no_barrier_load(&g_min_severity_to_print)
             ? 1
             : 0;
}

int gpr_should_log_stacktrace(gpr_log_severity severity) {
  return static_cast<gpr_atm>(severity) >=
                 gpr_atm_no_barrier_load(&g_min_severity_to_print_stacktrace)
             ? 1
             : 0;
}

void gpr_log_message(const char* file, int line, gpr_log_severity severity,
                     const char* message) {
  if (gpr_should_log(severity) == 0) {
    return;
  }

  gpr_log_func_args lfargs;
  memset(&lfargs, 0, sizeof(lfargs));
  lfargs.file = file;
  lfargs.line = line;
  lfargs.severity = severity;
  lfargs.message = message;
  reinterpret_cast<gpr_log_func>(gpr_atm_no_barrier_load(&g_log_func))(&lfargs);
}

void gpr_set_log_verbosity(gpr_log_severity min_severity_to_print) {
  gpr_atm_no_barrier_store(&g_min_severity_to_print,
                           (gpr_atm)min_severity_to_print);
}

static gpr_atm parse_log_severity(const char* str, gpr_atm error_value) {
  if (gpr_stricmp(str, "DEBUG") == 0) {
    return GPR_LOG_SEVERITY_DEBUG;
  } else if (gpr_stricmp(str, "INFO") == 0) {
    return GPR_LOG_SEVERITY_INFO;
  } else if (gpr_stricmp(str, "ERROR") == 0) {
    return GPR_LOG_SEVERITY_ERROR;
  } else if (gpr_stricmp(str, "NONE") == 0) {
    return GPR_LOG_SEVERITY_NONE;
  } else {
    return error_value;
  }
}

void gpr_log_verbosity_init() {
  // init verbosity when it hasn't been set
  if ((gpr_atm_no_barrier_load(&g_min_severity_to_print)) ==
      GPR_LOG_SEVERITY_UNSET) {
    grpc_core::UniquePtr<char> verbosity =
        GPR_GLOBAL_CONFIG_GET(grpc_verbosity);
    gpr_atm min_severity_to_print = GPR_LOG_SEVERITY_ERROR;
    if (strlen(verbosity.get()) > 0) {
      min_severity_to_print =
          parse_log_severity(verbosity.get(), min_severity_to_print);
    }
    gpr_atm_no_barrier_store(&g_min_severity_to_print, min_severity_to_print);
  }
  // init stacktrace_minloglevel when it hasn't been set
  if ((gpr_atm_no_barrier_load(&g_min_severity_to_print_stacktrace)) ==
      GPR_LOG_SEVERITY_UNSET) {
    grpc_core::UniquePtr<char> stacktrace_minloglevel =
        GPR_GLOBAL_CONFIG_GET(grpc_stacktrace_minloglevel);
    gpr_atm min_severity_to_print_stacktrace = GPR_LOG_SEVERITY_NONE;
    if (strlen(stacktrace_minloglevel.get()) > 0) {
      min_severity_to_print_stacktrace = parse_log_severity(
          stacktrace_minloglevel.get(), min_severity_to_print_stacktrace);
    }
    gpr_atm_no_barrier_store(&g_min_severity_to_print_stacktrace,
                             min_severity_to_print_stacktrace);
  }
}

void gpr_set_log_function(gpr_log_func f) {
  gpr_atm_no_barrier_store(&g_log_func, (gpr_atm)(f ? f : gpr_default_log));
}
