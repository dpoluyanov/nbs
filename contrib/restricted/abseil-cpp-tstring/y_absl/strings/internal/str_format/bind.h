// Copyright 2020 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef Y_ABSL_STRINGS_INTERNAL_STR_FORMAT_BIND_H_
#define Y_ABSL_STRINGS_INTERNAL_STR_FORMAT_BIND_H_

#include <array>
#include <cstdio>
#include <sstream>
#include <util/generic/string.h>

#include "y_absl/base/port.h"
#include "y_absl/strings/internal/str_format/arg.h"
#include "y_absl/strings/internal/str_format/checker.h"
#include "y_absl/strings/internal/str_format/parser.h"
#include "y_absl/types/span.h"
#include "y_absl/utility/utility.h"

namespace y_absl {
Y_ABSL_NAMESPACE_BEGIN

class UntypedFormatSpec;

namespace str_format_internal {

class BoundConversion : public FormatConversionSpecImpl {
 public:
  const FormatArgImpl* arg() const { return arg_; }
  void set_arg(const FormatArgImpl* a) { arg_ = a; }

 private:
  const FormatArgImpl* arg_;
};

// This is the type-erased class that the implementation uses.
class UntypedFormatSpecImpl {
 public:
  UntypedFormatSpecImpl() = delete;

  explicit UntypedFormatSpecImpl(string_view s)
      : data_(s.data()), size_(s.size()) {}
  explicit UntypedFormatSpecImpl(
      const str_format_internal::ParsedFormatBase* pc)
      : data_(pc), size_(~size_t{}) {}

  bool has_parsed_conversion() const { return size_ == ~size_t{}; }

  string_view str() const {
    assert(!has_parsed_conversion());
    return string_view(static_cast<const char*>(data_), size_);
  }
  const str_format_internal::ParsedFormatBase* parsed_conversion() const {
    assert(has_parsed_conversion());
    return static_cast<const str_format_internal::ParsedFormatBase*>(data_);
  }

  template <typename T>
  static const UntypedFormatSpecImpl& Extract(const T& s) {
    return s.spec_;
  }

 private:
  const void* data_;
  size_t size_;
};

template <typename T, FormatConversionCharSet...>
struct MakeDependent {
  using type = T;
};

// Implicitly convertible from `const char*`, `string_view`, and the
// `ExtendedParsedFormat` type. This abstraction allows all format functions to
// operate on any without providing too many overloads.
template <FormatConversionCharSet... Args>
class FormatSpecTemplate
    : public MakeDependent<UntypedFormatSpec, Args...>::type {
  using Base = typename MakeDependent<UntypedFormatSpec, Args...>::type;

  template <bool res>
  struct ErrorMaker {
    constexpr bool operator()(int) const { return res; }
  };

  template <int i, int j>
  static constexpr bool CheckArity(ErrorMaker<true> SpecifierCount = {},
                                   ErrorMaker<i == j> ParametersPassed = {}) {
    static_assert(SpecifierCount(i) == ParametersPassed(j),
                  "Number of arguments passed must match the number of "
                  "conversion specifiers.");
    return true;
  }

  template <FormatConversionCharSet specified, FormatConversionCharSet passed,
            int arg>
  static constexpr bool CheckMatch(
      ErrorMaker<Contains(specified, passed)> MismatchedArgumentNumber = {}) {
    static_assert(MismatchedArgumentNumber(arg),
                  "Passed argument must match specified format.");
    return true;
  }

  template <FormatConversionCharSet... C, size_t... I>
  static bool CheckMatches(y_absl::index_sequence<I...>) {
    bool res[] = {true, CheckMatch<Args, C, I + 1>()...};
    (void)res;
    return true;
  }

 public:
#ifdef Y_ABSL_INTERNAL_ENABLE_FORMAT_CHECKER

  // Honeypot overload for when the string is not constexpr.
  // We use the 'unavailable' attribute to give a better compiler error than
  // just 'method is deleted'.
  FormatSpecTemplate(...)  // NOLINT
      __attribute__((unavailable("Format string is not constexpr.")));

  // Honeypot overload for when the format is constexpr and invalid.
  // We use the 'unavailable' attribute to give a better compiler error than
  // just 'method is deleted'.
  // To avoid checking the format twice, we just check that the format is
  // constexpr. If it is valid, then the overload below will kick in.
  // We add the template here to make this overload have lower priority.
  template <typename = void>
  FormatSpecTemplate(const char* s)  // NOLINT
      __attribute__((
          enable_if(str_format_internal::EnsureConstexpr(s), "constexpr trap"),
          unavailable(
              "Format specified does not match the arguments passed.")));

  template <typename T = void>
  FormatSpecTemplate(string_view s)  // NOLINT
      __attribute__((enable_if(str_format_internal::EnsureConstexpr(s),
                               "constexpr trap")))
      : Base("to avoid noise in the compiler error") {
    static_assert(sizeof(T*) == 0,
                  "Format specified does not match the arguments passed.");
  }

  // Good format overload.
  FormatSpecTemplate(const char* s)  // NOLINT
      __attribute__((enable_if(ValidFormatImpl<Args...>(s), "bad format trap")))
      : Base(s) {}

  FormatSpecTemplate(string_view s)  // NOLINT
      __attribute__((enable_if(ValidFormatImpl<Args...>(s), "bad format trap")))
      : Base(s) {}

#else  // Y_ABSL_INTERNAL_ENABLE_FORMAT_CHECKER

  FormatSpecTemplate(const char* s) : Base(s) {}  // NOLINT
  FormatSpecTemplate(string_view s) : Base(s) {}  // NOLINT

#endif  // Y_ABSL_INTERNAL_ENABLE_FORMAT_CHECKER

  template <FormatConversionCharSet... C>
  FormatSpecTemplate(const ExtendedParsedFormat<C...>& pc)  // NOLINT
      : Base(&pc) {
    CheckArity<sizeof...(C), sizeof...(Args)>();
    CheckMatches<C...>(y_absl::make_index_sequence<sizeof...(C)>{});
  }
};

class Streamable {
 public:
  Streamable(const UntypedFormatSpecImpl& format,
             y_absl::Span<const FormatArgImpl> args)
      : format_(format) {
    if (args.size() <= Y_ABSL_ARRAYSIZE(few_args_)) {
      for (size_t i = 0; i < args.size(); ++i) {
        few_args_[i] = args[i];
      }
      args_ = y_absl::MakeSpan(few_args_, args.size());
    } else {
      many_args_.assign(args.begin(), args.end());
      args_ = many_args_;
    }
  }

  std::ostream& Print(std::ostream& os) const;

  friend std::ostream& operator<<(std::ostream& os, const Streamable& l) {
    return l.Print(os);
  }

 private:
  const UntypedFormatSpecImpl& format_;
  y_absl::Span<const FormatArgImpl> args_;
  // if args_.size() is 4 or less:
  FormatArgImpl few_args_[4] = {FormatArgImpl(0), FormatArgImpl(0),
                                FormatArgImpl(0), FormatArgImpl(0)};
  // if args_.size() is more than 4:
  std::vector<FormatArgImpl> many_args_;
};

// for testing
TString Summarize(UntypedFormatSpecImpl format,
                      y_absl::Span<const FormatArgImpl> args);
bool BindWithPack(const UnboundConversion* props,
                  y_absl::Span<const FormatArgImpl> pack, BoundConversion* bound);

bool FormatUntyped(FormatRawSinkImpl raw_sink,
                   UntypedFormatSpecImpl format,
                   y_absl::Span<const FormatArgImpl> args);

TString& AppendPack(TString* out, UntypedFormatSpecImpl format,
                        y_absl::Span<const FormatArgImpl> args);

TString FormatPack(const UntypedFormatSpecImpl format,
                       y_absl::Span<const FormatArgImpl> args);

int FprintF(std::FILE* output, UntypedFormatSpecImpl format,
            y_absl::Span<const FormatArgImpl> args);
int SnprintF(char* output, size_t size, UntypedFormatSpecImpl format,
             y_absl::Span<const FormatArgImpl> args);

// Returned by Streamed(v). Converts via '%s' to the TString created
// by std::ostream << v.
template <typename T>
class StreamedWrapper {
 public:
  explicit StreamedWrapper(const T& v) : v_(v) { }

 private:
  template <typename S>
  friend ArgConvertResult<FormatConversionCharSetUnion(
      FormatConversionCharSetInternal::s, FormatConversionCharSetInternal::v)>
  FormatConvertImpl(const StreamedWrapper<S>& v, FormatConversionSpecImpl conv,
                    FormatSinkImpl* out);
  const T& v_;
};

}  // namespace str_format_internal
Y_ABSL_NAMESPACE_END
}  // namespace y_absl

#endif  // Y_ABSL_STRINGS_INTERNAL_STR_FORMAT_BIND_H_
