// Copyright 2022 Google LLC
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

#ifndef FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_FLAT_MAP_IMPL_H_
#define FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_FLAT_MAP_IMPL_H_

#include <optional>
#include <tuple>
#include <type_traits>

#include "absl/random/bit_gen_ref.h"
#include "absl/random/distributions.h"
#include "absl/types/span.h"
#include "./fuzztest/internal/domains/domain_base.h"
#include "./fuzztest/internal/domains/serialization_helpers.h"
#include "./fuzztest/internal/meta.h"
#include "./fuzztest/internal/serialization.h"
#include "./fuzztest/internal/status.h"
#include "./fuzztest/internal/type_support.h"

namespace fuzztest::internal {

// FlatMap takes a domain factory function (flat mapper) and an input domain
// for each parameter of the factory function. The output domain is what the
// flat mapper returns and the domain that FlatMap represents. I.e., the "output
// domain" is re-created dynamically, as it depends on values created by the
// input domains.
template <typename FlatMapper, typename... InputDomain>
using FlatMapOutputDomain = std::decay_t<
    std::invoke_result_t<FlatMapper, const value_type_t<InputDomain>&...>>;

template <typename FlatMapper, typename... InputDomain>
class FlatMapImpl
    : public DomainBase<
          FlatMapImpl<FlatMapper, InputDomain...>,
          // The user value is the user value of the output domain.
          value_type_t<FlatMapOutputDomain<FlatMapper, InputDomain...>>,
          // The corpus value is a tuple where the first element is the corpus
          // value of the output domain, and the rest is the corpus value of the
          // input domains.
          std::tuple<
              corpus_type_t<FlatMapOutputDomain<FlatMapper, InputDomain...>>,
              corpus_type_t<InputDomain>...>> {
 public:
  using typename FlatMapImpl::DomainBase::corpus_type;
  using typename FlatMapImpl::DomainBase::value_type;

  FlatMapImpl() = default;
  explicit FlatMapImpl(FlatMapper flat_mapper, InputDomain... input_domains)
      : flat_mapper_(std::move(flat_mapper)),
        input_domains_(std::move(input_domains)...) {}

  corpus_type Init(absl::BitGenRef prng) {
    if (auto seed = this->MaybeGetRandomSeed(prng)) return *seed;
    auto input_corpus = std::apply(
        [&](auto&... input_domains) {
          return std::make_tuple(input_domains.Init(prng)...);
        },
        input_domains_);
    auto output_domain = ApplyIndex<sizeof...(InputDomain)>([&](auto... I) {
      return flat_mapper_(
          std::get<I>(input_domains_).GetValue(std::get<I>(input_corpus))...);
    });
    return std::tuple_cat(std::make_tuple(output_domain.Init(prng)),
                          input_corpus);
  }

  void Mutate(corpus_type& val, absl::BitGenRef prng, bool only_shrink) {
    // There is no way to tell whether the current output corpus value is
    // consistent with a new output domain generated by mutated inputs, so
    // mutating the inputs forces re-initialization of the output domain. This
    // means that, when shrinking, we cannot mutate the inputs, as
    // re-initializing would lose the "still crashing" output value.
    bool mutate_inputs = !only_shrink && absl::Bernoulli(prng, 0.1);
    if (mutate_inputs) {
      ApplyIndex<sizeof...(InputDomain)>([&](auto... I) {
        // The first field of `val` is the output corpus value, so skip it.
        (std::get<I>(input_domains_)
             .Mutate(std::get<I + 1>(val), prng, only_shrink),
         ...);
      });
      std::get<0>(val) = GetOutputDomain(val).Init(prng);
      return;
    }
    // For simplicity, we create a new output domain each call to `Mutate`. This
    // means that stateful domains don't work, but this is currently a matter of
    // convenience, not correctness. For example, `Filter` won't automatically
    // find when something is too restrictive.
    // TODO(b/246423623): Support stateful domains.
    GetOutputDomain(val).Mutate(std::get<0>(val), prng, only_shrink);
  }

  value_type GetValue(const corpus_type& v) const {
    return GetOutputDomain(v).GetValue(std::get<0>(v));
  }

  std::optional<corpus_type> FromValue(const value_type&) const {
    // We cannot infer the input corpus from the output value, or even determine
    // from which output domain the output value came.
    return std::nullopt;
  }

  auto GetPrinter() const {
    return FlatMappedPrinter<FlatMapper, InputDomain...>{flat_mapper_,
                                                         input_domains_};
  }

  std::optional<corpus_type> ParseCorpus(const IRObject& obj) const {
    auto input_corpus = ParseWithDomainTuple(input_domains_, obj, /*skip=*/1);
    if (!input_corpus.has_value()) {
      return std::nullopt;
    }
    auto output_domain = ApplyIndex<sizeof...(InputDomain)>([&](auto... I) {
      return flat_mapper_(
          std::get<I>(input_domains_).GetValue(std::get<I>(*input_corpus))...);
    });
    // We know obj.Subs()[0] exists because ParseWithDomainTuple succeeded.
    auto output_corpus = output_domain.ParseCorpus((*obj.Subs())[0]);
    if (!output_corpus.has_value()) {
      return std::nullopt;
    }
    return std::tuple_cat(std::make_tuple(*output_corpus), *input_corpus);
  }

  IRObject SerializeCorpus(const corpus_type& v) const {
    auto domain =
        std::tuple_cat(std::make_tuple(GetOutputDomain(v)), input_domains_);
    return SerializeWithDomainTuple(domain, v);
  }

  absl::Status ValidateCorpusValue(const corpus_type& corpus_value) const {
    // Check input values first.
    absl::Status input_values_validity = absl::OkStatus();
    ApplyIndex<sizeof...(InputDomain)>([&](auto... I) {
      (
          [&] {
            if (!input_values_validity.ok()) return;
            const absl::Status s =
                std::get<I>(input_domains_)
                    .ValidateCorpusValue(std::get<I + 1>(corpus_value));
            input_values_validity =
                Prefix(s, "Invalid value for FlatMap()-ed domain");
          }(),
          ...);
    });
    if (!input_values_validity.ok()) return input_values_validity;
    // Check the output value.
    return GetOutputDomain(corpus_value)
        .ValidateCorpusValue(std::get<0>(corpus_value));
  }

 private:
  using output_domain_t = std::decay_t<
      std::invoke_result_t<FlatMapper, const value_type_t<InputDomain>&...>>;
  output_domain_t GetOutputDomain(const corpus_type& val) const {
    return ApplyIndex<sizeof...(InputDomain)>([&](auto... I) {
      // The first field of `val` is the output corpus value, so skip it.
      return flat_mapper_(
          std::get<I>(input_domains_).GetValue(std::get<I + 1>(val))...);
    });
  }

  FlatMapper flat_mapper_;
  std::tuple<InputDomain...> input_domains_;
};

}  // namespace fuzztest::internal

#endif  // FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_FLAT_MAP_IMPL_H_
