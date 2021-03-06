// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <cmath>
#include <initializer_list>
#include <random>
#include <tuple>
#include <vector>

#include <boost/math/constants/constants.hpp>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/expression.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/erf.hpp>
#include <heyoka/math/exp.hpp>
#include <heyoka/math/square.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>

#include "catch.hpp"
#include "test_utils.hpp"

static std::mt19937 rng;

using namespace heyoka;
using namespace heyoka_test;

const auto fp_types = std::tuple<double, long double
#if defined(HEYOKA_HAVE_REAL128)
                                 ,
                                 mppp::real128
#endif
                                 >{};

template <typename T, typename U>
void compare_batch_scalar(std::initializer_list<U> sys, unsigned opt_level, bool high_accuracy, bool compact_mode)
{
    for (auto batch_size : {2u, 4u, 8u, 23u}) {
        llvm_state s{kw::opt_level = opt_level};

        taylor_add_jet<T>(s, "jet_batch", sys, 3, batch_size, high_accuracy, compact_mode);
        taylor_add_jet<T>(s, "jet_scalar", sys, 3, 1, high_accuracy, compact_mode);

        s.compile();

        auto jptr_batch = reinterpret_cast<void (*)(T *, const T *, const T *)>(s.jit_lookup("jet_batch"));
        auto jptr_scalar = reinterpret_cast<void (*)(T *, const T *, const T *)>(s.jit_lookup("jet_scalar"));

        std::vector<T> jet_batch;
        jet_batch.resize(8 * batch_size);
        std::uniform_real_distribution<float> dist(-10.f, 10.f);
        std::generate(jet_batch.begin(), jet_batch.end(), [&dist]() { return T{dist(rng)}; });

        std::vector<T> jet_scalar;
        jet_scalar.resize(8);

        jptr_batch(jet_batch.data(), nullptr, nullptr);

        for (auto batch_idx = 0u; batch_idx < batch_size; ++batch_idx) {
            // Assign the initial values of x and y.
            for (auto i = 0u; i < 2u; ++i) {
                jet_scalar[i] = jet_batch[i * batch_size + batch_idx];
            }

            jptr_scalar(jet_scalar.data(), nullptr, nullptr);

            for (auto i = 2u; i < 8u; ++i) {
                REQUIRE(jet_scalar[i] == approximately(jet_batch[i * batch_size + batch_idx], T(1000)));
            }
        }
    }
}

TEST_CASE("ode test")
{
    using std::abs;
    using std::erf;
    using std::exp;
    auto pi = boost::math::constants::pi<double>();

    for (auto opt_level : {0u, 1u, 2u, 3u}) {
        for (auto cm : {false, true}) {
            for (auto ha : {false, true}) {
                auto [x, s] = make_vars("x", "s");
                taylor_adaptive<double> ta0({prime(x) = erf(1e-2 * x) + x}, {.5}, kw::high_accuracy = ha,
                                            kw::compact_mode = cm, kw::opt_level = opt_level);
                taylor_adaptive<double> ta1(
                    {prime(x) = s + x, prime(s) = (2. / sqrt(pi) * exp(-1e-4 * x * x)) * 1e-2 * (s + x)},
                    {.5, erf(1e-2 * .5)}, kw::high_accuracy = ha, kw::compact_mode = cm, kw::opt_level = opt_level);

                ta0.propagate_until(5.);
                ta1.propagate_until(5.);

                REQUIRE(abs((ta0.get_state()[0] - ta1.get_state()[0]) / ta0.get_state()[0]) < 1e-14);

                const auto v0 = erf(ta0.get_state()[0] * 1e-2);
                const auto v1 = ta1.get_state()[1];

                REQUIRE(abs((v0 - v1) / v0) < 1e-14);
            }
        }
    }
}

// Test CSE involving hidden dependencies.
TEST_CASE("taylor erf test simplifications")
{
    using std::erf;
    using std::exp;
    using std::sqrt;

    auto pi = boost::math::constants::pi<double>();

    auto x = "x"_var, y = "y"_var;

    llvm_state s{kw::opt_level = 0u};

    auto dc = taylor_add_jet<double>(s, "jet", {exp(-square(x + y)) + erf(x + y), x}, 2, 1, false, false);

    REQUIRE(dc.size() == 10u);

    s.compile();

    auto jptr = reinterpret_cast<void (*)(double *, const double *, const double *)>(s.jit_lookup("jet"));

    std::vector<double> jet{double{2}, double{3}};
    jet.resize(6);

    jptr(jet.data(), nullptr, nullptr);

    REQUIRE(jet[0] == 2.);
    REQUIRE(jet[1] == 3.);
    REQUIRE(jet[2] == approximately(exp(-(jet[0] + jet[1]) * (jet[0] + jet[1])) + erf(jet[0] + jet[1])));
    REQUIRE(jet[3] == jet[0]);
    REQUIRE(
        jet[4]
        == approximately(.5
                         * (-2. * (jet[0] + jet[1]) * (jet[2] + jet[3]) * exp(-(jet[0] + jet[1]) * (jet[0] + jet[1]))
                            + 2 / sqrt(pi) * exp(-(jet[0] + jet[1]) * (jet[0] + jet[1])) * (jet[2] + jet[3]))));
    REQUIRE(jet[5] == approximately(.5 * jet[2]));
}

TEST_CASE("taylor erf")
{
    auto tester = [](auto fp_x, unsigned opt_level, bool high_accuracy, bool compact_mode) {
        using std::erf;
        using std::exp;
        using std::sqrt;
        using fp_t = decltype(fp_x);
        auto pi = boost::math::constants::pi<fp_t>();
        using Catch::Matchers::Message;

        auto x = "x"_var, y = "y"_var;

        // Number tests.
        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(expression{number{fp_t{2}}}), x + y}, 1, 1, high_accuracy,
                                 compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data(), nullptr, nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(erf(fp_t{2})));
            REQUIRE(jet[3] == approximately(jet[0] + jet[1]));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(par[0]), x + y}, 1, 1, high_accuracy, compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            std::vector<fp_t> pars{fp_t{2}};

            jptr(jet.data(), pars.data(), nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(erf(fp_t{2})));
            REQUIRE(jet[3] == approximately(jet[0] + jet[1]));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(expression{number{fp_t{2}}}), x + y}, 1, 2, high_accuracy,
                                 compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-4}, fp_t{3}, fp_t{5}};
            jet.resize(8);

            jptr(jet.data(), nullptr, nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -4);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 5);

            REQUIRE(jet[4] == approximately(erf(fp_t{2})));
            REQUIRE(jet[5] == approximately(erf(fp_t{2})));

            REQUIRE(jet[6] == approximately(jet[0] + jet[2]));
            REQUIRE(jet[7] == approximately(jet[1] + jet[3]));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(par[1]), x + y}, 1, 2, high_accuracy, compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-4}, fp_t{3}, fp_t{5}};
            jet.resize(8);

            std::vector<fp_t> pars{fp_t{2}, fp_t{2}, fp_t{3}, fp_t{3}};

            jptr(jet.data(), pars.data(), nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -4);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 5);

            REQUIRE(jet[4] == approximately(erf(fp_t{3})));
            REQUIRE(jet[5] == approximately(erf(fp_t{3})));

            REQUIRE(jet[6] == approximately(jet[0] + jet[2]));
            REQUIRE(jet[7] == approximately(jet[1] + jet[3]));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(expression{number{fp_t{2}}}), x + y}, 2, 1, high_accuracy,
                                 compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data(), nullptr, nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(erf(fp_t{2})));
            REQUIRE(jet[3] == approximately(jet[0] + jet[1]));
            REQUIRE(jet[4] == 0);
            REQUIRE(jet[5] == approximately(fp_t{1} / 2 * (jet[2] + jet[3])));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(expression{number{fp_t{2}}}), x + y}, 2, 2, high_accuracy,
                                 compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-4}, fp_t{3}, fp_t{5}};
            jet.resize(12);

            jptr(jet.data(), nullptr, nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -4);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 5);

            REQUIRE(jet[4] == approximately(erf(fp_t{2})));
            REQUIRE(jet[5] == approximately(erf(fp_t{2})));

            REQUIRE(jet[6] == approximately(jet[0] + jet[2]));
            REQUIRE(jet[7] == approximately(jet[1] + jet[3]));

            REQUIRE(jet[8] == 0);
            REQUIRE(jet[9] == 0);

            REQUIRE(jet[10] == approximately(fp_t{1} / 2 * (jet[4] + jet[6])));
            REQUIRE(jet[11] == approximately(fp_t{1} / 2 * (jet[5] + jet[7])));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(expression{number{fp_t{2}}}), x + y}, 3, 3, high_accuracy,
                                 compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-4}, fp_t{-1}, fp_t{3}, fp_t{5}, fp_t{-2}};
            jet.resize(24);

            jptr(jet.data(), nullptr, nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -4);
            REQUIRE(jet[2] == -1);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == 5);
            REQUIRE(jet[5] == -2);

            REQUIRE(jet[6] == approximately(erf(fp_t{2})));
            REQUIRE(jet[7] == approximately(erf(fp_t{2})));
            REQUIRE(jet[8] == approximately(erf(fp_t{2})));

            REQUIRE(jet[9] == approximately(jet[0] + jet[3]));
            REQUIRE(jet[10] == approximately(jet[1] + jet[4]));
            REQUIRE(jet[11] == approximately(jet[2] + jet[5]));

            REQUIRE(jet[12] == 0);
            REQUIRE(jet[13] == 0);
            REQUIRE(jet[14] == 0);

            REQUIRE(jet[15] == approximately(fp_t{1} / 2 * (jet[6] + jet[9])));
            REQUIRE(jet[16] == approximately(fp_t{1} / 2 * (jet[7] + jet[10])));
            REQUIRE(jet[17] == approximately(fp_t{1} / 2 * (jet[8] + jet[11])));

            REQUIRE(jet[18] == 0);
            REQUIRE(jet[19] == 0);
            REQUIRE(jet[20] == 0);

            REQUIRE(jet[21] == approximately(fp_t{1} / 6 * (2 * jet[15] + 2 * jet[18])));
            REQUIRE(jet[22] == approximately(fp_t{1} / 6 * (2 * jet[16] + 2 * jet[19])));
            REQUIRE(jet[23] == approximately(fp_t{1} / 6 * (2 * jet[17] + 2 * jet[20])));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(par[0]), x + y}, 3, 3, high_accuracy, compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-4}, fp_t{-1}, fp_t{3}, fp_t{5}, fp_t{-2}};
            jet.resize(24);

            std::vector<fp_t> pars{fp_t{2}, fp_t{2}, fp_t{2}, fp_t{3}, fp_t{3}, fp_t{3}};

            jptr(jet.data(), pars.data(), nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -4);
            REQUIRE(jet[2] == -1);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == 5);
            REQUIRE(jet[5] == -2);

            REQUIRE(jet[6] == approximately(erf(fp_t{2})));
            REQUIRE(jet[7] == approximately(erf(fp_t{2})));
            REQUIRE(jet[8] == approximately(erf(fp_t{2})));

            REQUIRE(jet[9] == approximately(jet[0] + jet[3]));
            REQUIRE(jet[10] == approximately(jet[1] + jet[4]));
            REQUIRE(jet[11] == approximately(jet[2] + jet[5]));

            REQUIRE(jet[12] == 0);
            REQUIRE(jet[13] == 0);
            REQUIRE(jet[14] == 0);

            REQUIRE(jet[15] == approximately(fp_t{1} / 2 * (jet[6] + jet[9])));
            REQUIRE(jet[16] == approximately(fp_t{1} / 2 * (jet[7] + jet[10])));
            REQUIRE(jet[17] == approximately(fp_t{1} / 2 * (jet[8] + jet[11])));

            REQUIRE(jet[18] == 0);
            REQUIRE(jet[19] == 0);
            REQUIRE(jet[20] == 0);

            REQUIRE(jet[21] == approximately(fp_t{1} / 6 * (2 * jet[15] + 2 * jet[18])));
            REQUIRE(jet[22] == approximately(fp_t{1} / 6 * (2 * jet[16] + 2 * jet[19])));
            REQUIRE(jet[23] == approximately(fp_t{1} / 6 * (2 * jet[17] + 2 * jet[20])));
        }

        // Do the batch/scalar comparison.
        compare_batch_scalar<fp_t>({erf(expression{number{fp_t{2}}}), x + y}, opt_level, high_accuracy, compact_mode);

        // Variable tests.
        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(y), erf(x)}, 1, 1, high_accuracy, compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data(), nullptr, nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(erf(jet[1])));
            REQUIRE(jet[3] == approximately(erf(jet[0])));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(y), erf(x)}, 1, 2, high_accuracy, compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-1}, fp_t{3}, fp_t{-4}};
            jet.resize(8);

            jptr(jet.data(), nullptr, nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -1);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == -4);

            REQUIRE(jet[4] == approximately(erf(jet[2])));
            REQUIRE(jet[5] == approximately(erf(jet[3])));

            REQUIRE(jet[6] == approximately(erf(jet[0])));
            REQUIRE(jet[7] == approximately(erf(jet[1])));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(y), erf(x)}, 2, 1, high_accuracy, compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data(), nullptr, nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(erf(jet[1])));
            REQUIRE(jet[3] == approximately(erf(jet[0])));
            REQUIRE(jet[4] == approximately(fp_t{1} / 2. * ((2. / sqrt(pi) * exp(-jet[1] * jet[1])) * jet[3])));
            REQUIRE(jet[5] == approximately(fp_t{1} / 2. * ((2. / sqrt(pi) * exp(-jet[0] * jet[0])) * jet[2])));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(y), erf(x)}, 2, 2, high_accuracy, compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-1}, fp_t{3}, fp_t{-4}};
            jet.resize(12);

            jptr(jet.data(), nullptr, nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -1);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == -4);

            REQUIRE(jet[4] == approximately(erf(jet[2])));
            REQUIRE(jet[5] == approximately(erf(jet[3])));

            REQUIRE(jet[6] == approximately(erf(jet[0])));
            REQUIRE(jet[7] == approximately(erf(jet[1])));

            REQUIRE(jet[8] == approximately(fp_t{1} / 2 * ((2. / sqrt(pi) * exp(-jet[2] * jet[2])) * jet[6])));
            REQUIRE(jet[9] == approximately(fp_t{1} / 2 * ((2. / sqrt(pi) * exp(-jet[3] * jet[3])) * jet[7])));

            REQUIRE(jet[10] == approximately(fp_t{1} / 2 * ((2. / sqrt(pi) * exp(-jet[0] * jet[0])) * jet[4])));
            REQUIRE(jet[11] == approximately(fp_t{1} / 2 * ((2. / sqrt(pi) * exp(-jet[1] * jet[1])) * jet[5])));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {erf(y), erf(x)}, 3, 3, high_accuracy, compact_mode);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *, const fp_t *, const fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-1}, fp_t{-5}, fp_t{3}, fp_t{-4}, fp_t{6}};
            jet.resize(24);

            jptr(jet.data(), nullptr, nullptr);

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -1);
            REQUIRE(jet[2] == -5);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == -4);
            REQUIRE(jet[5] == 6);

            REQUIRE(jet[6] == approximately(erf(jet[3])));
            REQUIRE(jet[7] == approximately(erf(jet[4])));
            REQUIRE(jet[8] == approximately(erf(jet[5])));

            REQUIRE(jet[9] == approximately(erf(jet[0])));
            REQUIRE(jet[10] == approximately(erf(jet[1])));
            REQUIRE(jet[11] == approximately(erf(jet[2])));

            REQUIRE(jet[12] == approximately(fp_t{1} / 2 * (2. / sqrt(pi) * exp(-jet[3] * jet[3]) * jet[9])));
            REQUIRE(jet[13] == approximately(fp_t{1} / 2 * (2. / sqrt(pi) * exp(-jet[4] * jet[4]) * jet[10])));
            REQUIRE(jet[14] == approximately(fp_t{1} / 2 * (2. / sqrt(pi) * exp(-jet[5] * jet[5]) * jet[11])));

            REQUIRE(jet[15] == approximately(fp_t{1} / 2 * (2. / sqrt(pi) * exp(-jet[0] * jet[0]) * jet[6])));
            REQUIRE(jet[16] == approximately(fp_t{1} / 2 * (2. / sqrt(pi) * exp(-jet[1] * jet[1]) * jet[7])));
            REQUIRE(jet[17] == approximately(fp_t{1} / 2 * (2. / sqrt(pi) * exp(-jet[2] * jet[2]) * jet[8])));

            REQUIRE(jet[18]
                    == approximately(fp_t{1} / 6 * 2. / sqrt(pi)
                                     * (-2. * exp(-jet[3] * jet[3]) * jet[3] * jet[9] * jet[9]
                                        + exp(-jet[3] * jet[3]) * 2. / sqrt(pi) * exp(-jet[0] * jet[0]) * jet[6])));
            REQUIRE(jet[19]
                    == approximately(fp_t{1} / 6 * 2. / sqrt(pi)
                                     * (-2. * exp(-jet[4] * jet[4]) * jet[4] * jet[10] * jet[10]
                                        + exp(-jet[4] * jet[4]) * 2. / sqrt(pi) * exp(-jet[1] * jet[1]) * jet[7])));
            REQUIRE(jet[20]
                    == approximately(fp_t{1} / 6 * 2. / sqrt(pi)
                                     * (-2. * exp(-jet[5] * jet[5]) * jet[5] * jet[11] * jet[11]
                                        + exp(-jet[5] * jet[5]) * 2. / sqrt(pi) * exp(-jet[2] * jet[2]) * jet[8])));

            REQUIRE(jet[21]
                    == approximately(fp_t{1} / 6 * 2. / sqrt(pi)
                                     * (-2. * exp(-jet[0] * jet[0]) * jet[0] * jet[6] * jet[6]
                                        + exp(-jet[0] * jet[0]) * 2. / sqrt(pi) * exp(-jet[3] * jet[3]) * jet[9])));
            REQUIRE(jet[22]
                    == approximately(fp_t{1} / 6 * 2. / sqrt(pi)
                                     * (-2. * exp(-jet[1] * jet[1]) * jet[1] * jet[7] * jet[7]
                                        + exp(-jet[1] * jet[1]) * 2. / sqrt(pi) * exp(-jet[4] * jet[4]) * jet[10])));
            REQUIRE(jet[23]
                    == approximately(fp_t{1} / 6 * 2. / sqrt(pi)
                                     * (-2. * exp(-jet[2] * jet[2]) * jet[2] * jet[8] * jet[8]
                                        + exp(-jet[2] * jet[2]) * 2. / sqrt(pi) * exp(-jet[5] * jet[5]) * jet[11])));
        }

        // Do the batch/scalar comparison.
        compare_batch_scalar<fp_t>({erf(y), erf(x)}, opt_level, high_accuracy, compact_mode);
    };

    for (auto cm : {false, true}) {
        for (auto f : {false, true}) {
            tuple_for_each(fp_types, [&tester, f, cm](auto x) { tester(x, 0, f, cm); });
            tuple_for_each(fp_types, [&tester, f, cm](auto x) { tester(x, 1, f, cm); });
            tuple_for_each(fp_types, [&tester, f, cm](auto x) { tester(x, 2, f, cm); });
            tuple_for_each(fp_types, [&tester, f, cm](auto x) { tester(x, 3, f, cm); });
        }
    }
}
