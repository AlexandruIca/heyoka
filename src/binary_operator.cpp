// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <array>
#include <cassert>
#include <iterator>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <heyoka/binary_operator.hpp>
#include <heyoka/expression.hpp>

namespace heyoka
{

binary_operator::binary_operator(type t, expression e1, expression e2)
    : m_type(t),
      // NOTE: need to use naked new as make_unique won't work with aggregate
      // initialization.
      m_ops(::new std::array<expression, 2>{std::move(e1), std::move(e2)})
{
}

binary_operator::binary_operator(const binary_operator &other)
    : m_type(other.m_type), m_ops(std::make_unique<std::array<expression, 2>>(*other.m_ops))
{
}

binary_operator::binary_operator(binary_operator &&) noexcept = default;

binary_operator::~binary_operator() = default;

expression &binary_operator::lhs()
{
    assert(m_ops);
    return (*m_ops)[0];
}

expression &binary_operator::rhs()
{
    assert(m_ops);
    return (*m_ops)[1];
}

binary_operator::type &binary_operator::op()
{
    assert(m_type >= type::add && m_type <= type::div);
    return m_type;
}

const expression &binary_operator::lhs() const
{
    assert(m_ops);
    return (*m_ops)[0];
}

const expression &binary_operator::rhs() const
{
    assert(m_ops);
    return (*m_ops)[1];
}

const binary_operator::type &binary_operator::op() const
{
    assert(m_type >= type::add && m_type <= type::div);
    return m_type;
}

std::ostream &operator<<(std::ostream &os, const binary_operator &bo)
{
    os << '(' << bo.lhs() << ' ';

    switch (bo.op()) {
        case binary_operator::type::add:
            os << '+';
            break;
        case binary_operator::type::sub:
            os << '-';
            break;
        case binary_operator::type::mul:
            os << '*';
            break;
        case binary_operator::type::div:
            os << '/';
            break;
    }

    return os << ' ' << bo.rhs() << ')';
}

std::vector<std::string> get_variables(const binary_operator &bo)
{
    auto lhs_vars = get_variables(bo.lhs());
    auto rhs_vars = get_variables(bo.rhs());

    lhs_vars.insert(lhs_vars.end(), std::make_move_iterator(rhs_vars.begin()), std::make_move_iterator(rhs_vars.end()));

    std::sort(lhs_vars.begin(), lhs_vars.end());
    lhs_vars.erase(std::unique(lhs_vars.begin(), lhs_vars.end()), lhs_vars.end());

    return lhs_vars;
}

bool operator==(const binary_operator &o1, const binary_operator &o2)
{
    return o1.op() == o2.op() && o1.lhs() == o2.lhs() && o1.rhs() == o2.rhs();
}

bool operator!=(const binary_operator &o1, const binary_operator &o2)
{
    return !(o1 == o2);
}

expression diff(const binary_operator &bo, const std::string &s)
{
    switch (bo.op()) {
        case binary_operator::type::add:
            return diff(bo.lhs(), s) + diff(bo.rhs(), s);
        case binary_operator::type::sub:
            return diff(bo.lhs(), s) - diff(bo.rhs(), s);
        case binary_operator::type::mul:
            return diff(bo.lhs(), s) * bo.rhs() + bo.lhs() * diff(bo.rhs(), s);
        default:
            return (diff(bo.lhs(), s) * bo.rhs() - bo.lhs() * diff(bo.rhs(), s)) / (bo.rhs() * bo.rhs());
    }
}

double eval_dbl(const binary_operator &bo, const std::unordered_map<std::string, double> &map)
{
    switch (bo.op()) {
        case binary_operator::type::add:
            return eval_dbl(bo.lhs(), map) + eval_dbl(bo.rhs(), map);
        case binary_operator::type::sub:
            return eval_dbl(bo.lhs(), map) - eval_dbl(bo.rhs(), map);
        case binary_operator::type::mul:
            return eval_dbl(bo.lhs(), map) * eval_dbl(bo.rhs(), map);
        default:
            return eval_dbl(bo.lhs(), map) / eval_dbl(bo.rhs(), map);
    }
}

} // namespace heyoka