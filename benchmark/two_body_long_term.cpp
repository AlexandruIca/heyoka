// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <heyoka/nbody.hpp>
#include <heyoka/taylor.hpp>

// Two-body problem energy from the state vector.
template <typename T>
T tbp_energy(const std::vector<T> &st)
{
    using std::sqrt;

    auto Dx = st[0] - st[6];
    auto Dy = st[1] - st[7];
    auto Dz = st[2] - st[8];
    auto dist = sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
    auto U = -1 / dist;

    auto v2_0 = st[3] * st[3] + st[4] * st[4] + st[5] * st[5];
    auto v2_1 = st[9] * st[9] + st[10] * st[10] + st[11] * st[11];

    return T(1) / T(2) * (v2_0 + v2_1) + U;
}

template <typename T>
void run_integration()
{
    using std::abs;
    using std::log10;
    using std::pow;

    auto sys = heyoka::make_nbody_sys(2);

    const auto x0 = T(0.12753732455163191);
    const auto y0 = T(1.38595818266122);
    const auto z0 = T(0.35732917545977527);

    const auto vx0 = T(-0.41861303824199964);
    const auto vy0 = T(0.032224544954305295);
    const auto vz0 = T(0.070829797576461351);

    std::vector<T> init_state{x0, y0, z0, vx0, vy0, vz0, -x0, -y0, -z0, -vx0, -vy0, -vz0};

    heyoka::taylor_adaptive<T> ta{std::move(sys), std::move(init_state)};

    const auto &st = ta.get_state();

    const auto en = tbp_energy(st);

    // Base-10 logs of the initial and final saving times.
    const auto start_time = T(0), final_time = log10(T(3E8));
    // Number of snapshots to take.
    const auto n_snaps = 10000u;
    // Build the vector of log10 saving times.
    std::vector<T> save_times;
    save_times.push_back(start_time);
    for (auto i = 1u; i < n_snaps; ++i) {
        save_times.push_back(save_times.back() + (final_time - start_time) / (n_snaps - 1u));
    }
    for (auto &t : save_times) {
        t = pow(T(10), t);
    }

    std::ofstream of("two_body_long_term.txt");
    of.precision(std::numeric_limits<T>::max_digits10);
    auto it = save_times.begin();
    while (ta.get_time() < pow(T(10), final_time)) {
        if (it != save_times.end() && ta.get_time() >= *it) {
            // We are at or past the current saving time, record
            // the energy error.
            of << ta.get_time() << " " << abs((en - tbp_energy(st)) / en) << std::endl;

            // Locate the next saving time (that is, the first saving
            // time which is greater than the current time).
            it = std::upper_bound(it, save_times.end(), ta.get_time());
        }
        ta.step();
    }
}

int main(int argc, char *argv[])
{
    if (argc <= 1) {
        run_integration<double>();
    } else {
        const auto fp_type = std::stoi(std::string(argv[1]));

        switch (fp_type) {
            case 0:
                run_integration<double>();
                break;
            case 1:
                run_integration<long double>();
                break;
            default:
                throw std::invalid_argument("Invalid floating point type selected (" + std::string(argv[1]) + ")");
                break;
        }
    }
}
