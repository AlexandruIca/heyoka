// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include <heyoka/binary_operator.hpp>
#include <heyoka/detail/assert_nonnull_ret.hpp>
#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/string_conv.hpp>
#include <heyoka/detail/type_traits.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/function.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>
#include <heyoka/variable.hpp>

namespace heyoka
{

// Transform in-place ex by decomposition, appending the
// result of the decomposition to u_vars_defs.
// The return value is the index, in u_vars_defs,
// which corresponds to the decomposed version of ex.
// If the return value is zero, ex was not decomposed.
// NOTE: this will render ex unusable.
std::vector<expression>::size_type taylor_decompose_in_place(expression &&ex, std::vector<expression> &u_vars_defs)
{
    return std::visit(
        [&u_vars_defs](auto &&v) { return taylor_decompose_in_place(std::forward<decltype(v)>(v), u_vars_defs); },
        std::move(ex.value()));
}

namespace detail
{

namespace
{

// Simplify a Taylor decomposition by removing
// common subexpressions.
std::vector<expression> taylor_decompose_cse(std::vector<expression> &v_ex, std::vector<expression>::size_type n_eq)
{
    // A Taylor decomposition is supposed
    // to have n_eq variables at the beginning,
    // n_eq variables at the end and possibly
    // extra variables in the middle.
    assert(v_ex.size() >= n_eq * 2u);

    using idx_t = std::vector<expression>::size_type;

    std::vector<expression> retval;

    // expression -> idx map. This will end up containing
    // all the unique expressions from v_ex, and it will
    // map them to their indices in retval (which will
    // in general differ from their indices in v_ex).
    std::unordered_map<expression, idx_t> ex_map;

    // Map for the renaming of u variables
    // in the expressions.
    std::unordered_map<std::string, std::string> uvars_rename;

    // Add the definitions of the first n_eq
    // variables in terms of u variables.
    // No need to modify anything here.
    for (idx_t i = 0; i < n_eq; ++i) {
        retval.emplace_back(std::move(v_ex[i]));
    }

    for (auto i = n_eq; i < v_ex.size() - n_eq; ++i) {
        auto &ex = v_ex[i];

        // Rename the u variables in ex.
        rename_variables(ex, uvars_rename);

        if (auto it = ex_map.find(ex); it == ex_map.end()) {
            // This is the first occurrence of ex in the
            // decomposition. Add it to retval.
            retval.emplace_back(ex);

            // Add ex to ex_map, mapping it to
            // the index it corresponds to in retval
            // (let's call it j).
            ex_map.emplace(std::move(ex), retval.size() - 1u);

            // Update uvars_rename. This will ensure that
            // occurrences of the variable 'u_i' in the next
            // elements of v_ex will be renamed to 'u_j'.
            [[maybe_unused]] const auto res
                = uvars_rename.emplace("u_" + li_to_string(i), "u_" + li_to_string(retval.size() - 1u));
            assert(res.second);
        } else {
            // ex is a redundant expression. This means
            // that it already appears in retval at index
            // it->second. Don't add anything to retval,
            // and remap the variable name 'u_i' to
            // 'u_{it->second}'.
            [[maybe_unused]] const auto res
                = uvars_rename.emplace("u_" + li_to_string(i), "u_" + li_to_string(it->second));
            assert(res.second);
        }
    }

    // Handle the derivatives of the state variables at the
    // end of the decomposition. We just need to ensure that
    // the u variables in their definitions are renamed with
    // the new indices.
    for (auto i = v_ex.size() - n_eq; i < v_ex.size(); ++i) {
        auto &ex = v_ex[i];

        rename_variables(ex, uvars_rename);

        retval.emplace_back(std::move(ex));
    }

    return retval;
}

#if !defined(NDEBUG)

// Helper to verify a Taylor decomposition.
void verify_taylor_dec(const std::vector<expression> &orig, const std::vector<expression> &dc)
{
    using idx_t = std::vector<expression>::size_type;

    const auto n_eq = orig.size();

    assert(dc.size() >= n_eq * 2u);

    // The first n_eq expressions of u variables
    // must be just variables.
    for (idx_t i = 0; i < n_eq; ++i) {
        assert(std::holds_alternative<variable>(dc[i].value()));
    }

    // From n_eq to dc.size() - n_eq, the expressions
    // must contain variables only in the u_n form,
    // where n < i.
    for (auto i = n_eq; i < dc.size() - n_eq; ++i) {
        for (const auto &var : get_variables(dc[i])) {
            assert(var.rfind("u_", 0) == 0);
            assert(uname_to_index(var) < i);
        }
    }

    // From dc.size() - n_eq to dc.size(), the expressions
    // must be either variables in the u_n form, where n < i,
    // or numbers.
    for (auto i = dc.size() - n_eq; i < dc.size(); ++i) {
        std::visit(
            [i](const auto &v) {
                using type = detail::uncvref_t<decltype(v)>;

                if constexpr (std::is_same_v<type, variable>) {
                    assert(v.name().rfind("u_", 0) == 0);
                    assert(uname_to_index(v.name()) < i);
                } else if (!std::is_same_v<type, number>) {
                    assert(false);
                }
            },
            dc[i].value());
    }

    std::unordered_map<std::string, expression> subs_map;

    // For each u variable, expand its definition
    // in terms of state variables or other u variables,
    // and store it in subs_map.
    for (idx_t i = 0; i < dc.size() - n_eq; ++i) {
        subs_map.emplace("u_" + li_to_string(i), subs(dc[i], subs_map));
    }

    // Reconstruct the right-hand sides of the system
    // and compare them to the original ones.
    for (auto i = dc.size() - n_eq; i < dc.size(); ++i) {
        assert(subs(dc[i], subs_map) == orig[i - (dc.size() - n_eq)]);
    }
}

#endif

} // namespace

} // namespace detail

// Taylor decomposition with automatic deduction
// of variables.
std::vector<expression> taylor_decompose(std::vector<expression> v_ex)
{
    if (v_ex.empty()) {
        throw std::invalid_argument("Cannot decompose a system of zero equations");
    }

    // Determine the variables in the system of equations.
    std::vector<std::string> vars;
    for (const auto &ex : v_ex) {
        auto ex_vars = get_variables(ex);
        vars.insert(vars.end(), std::make_move_iterator(ex_vars.begin()), std::make_move_iterator(ex_vars.end()));
        std::sort(vars.begin(), vars.end());
        vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
    }

    if (vars.size() != v_ex.size()) {
        throw std::invalid_argument("The number of deduced variables for a Taylor decomposition ("
                                    + std::to_string(vars.size()) + ") differs from the number of equations ("
                                    + std::to_string(v_ex.size()) + ")");
    }

    // Cache the number of equations/variables
    // for later use.
    const auto n_eq = v_ex.size();

    // Create the map for renaming the variables to u_i.
    // The renaming will be done in alphabetical order.
    std::unordered_map<std::string, std::string> repl_map;
    for (decltype(vars.size()) i = 0; i < vars.size(); ++i) {
        [[maybe_unused]] const auto eres = repl_map.emplace(vars[i], "u_" + detail::li_to_string(i));
        assert(eres.second);
    }

#if !defined(NDEBUG)
    // Store a copy of the original system for checking later.
    const auto orig_v_ex = v_ex;
#endif

    // Rename the variables in the original equations.
    for (auto &ex : v_ex) {
        rename_variables(ex, repl_map);
    }

    // Init the vector containing the definitions
    // of the u variables. It begins with a list
    // of the original variables of the system.
    std::vector<expression> u_vars_defs;
    for (const auto &var : vars) {
        u_vars_defs.emplace_back(variable{var});
    }

    // Create a copy of the original equations in terms of u variables.
    // We will be reusing this below.
    auto v_ex_copy = v_ex;

    // Run the decomposition on each equation.
    for (decltype(v_ex.size()) i = 0; i < v_ex.size(); ++i) {
        // Decompose the current equation.
        if (const auto dres = taylor_decompose_in_place(std::move(v_ex[i]), u_vars_defs)) {
            // NOTE: if the equation was decomposed
            // (that is, it is not constant or a single variable),
            // we have to update the original definition
            // of the equation in v_ex_copy
            // so that it points to the u variable
            // that now represents it.
            v_ex_copy[i] = expression{variable{"u_" + detail::li_to_string(dres)}};
        }
    }

    // Append the (possibly updated) definitions of the diff equations
    // in terms of u variables.
    for (auto &ex : v_ex_copy) {
        u_vars_defs.emplace_back(std::move(ex));
    }

#if !defined(NDEBUG)
    // Verify the decomposition.
    detail::verify_taylor_dec(orig_v_ex, u_vars_defs);
#endif

    // Simplify the decomposition.
    u_vars_defs = detail::taylor_decompose_cse(u_vars_defs, n_eq);

#if !defined(NDEBUG)
    // Verify the simplified decomposition.
    detail::verify_taylor_dec(orig_v_ex, u_vars_defs);
#endif

    return u_vars_defs;
}

// Taylor decomposition from lhs and rhs
// of a system of equations.
std::vector<expression> taylor_decompose(std::vector<std::pair<expression, expression>> sys)
{
    if (sys.empty()) {
        throw std::invalid_argument("Cannot decompose a system of zero equations");
    }

    // Determine the variables in the system of equations
    // from the lhs of the equations. We need to ensure that:
    // - all the lhs expressions are variables
    //   and there are no duplicates,
    // - all the variables in the rhs expressions
    //   appear in the lhs expressions.
    // Note that not all variables in the lhs
    // need to appear in the rhs.

    // This will eventually contain the list
    // of all variables in the system.
    std::vector<std::string> lhs_vars;
    // Maintain a set as well to check for duplicates.
    std::unordered_set<std::string> lhs_vars_set;
    // The set of variables in the rhs.
    std::unordered_set<std::string> rhs_vars_set;

    for (const auto &p : sys) {
        const auto &lhs = p.first;
        const auto &rhs = p.second;

        // Infer the variable from the current lhs.
        std::visit(
            [&lhs, &lhs_vars, &lhs_vars_set](const auto &v) {
                if constexpr (std::is_same_v<detail::uncvref_t<decltype(v)>, variable>) {
                    // Check if this is a duplicate variable.
                    if (auto res = lhs_vars_set.emplace(v.name()); res.second) {
                        // Not a duplicate, add it to lhs_vars.
                        lhs_vars.emplace_back(v.name());
                    } else {
                        // Duplicate, error out.
                        throw std::invalid_argument(
                            "Error in the Taylor decomposition of a system of equations: the variable '" + v.name()
                            + "' appears in the left-hand side twice");
                    }
                } else {
                    std::ostringstream oss;
                    oss << lhs;

                    throw std::invalid_argument("Error in the Taylor decomposition of a system of equations: the "
                                                "left-hand side contains the expression '"
                                                + oss.str() + "', which is not a variable");
                }
            },
            lhs.value());

        // Update the global list of variables
        // for the rhs.
        for (auto &var : get_variables(rhs)) {
            rhs_vars_set.emplace(std::move(var));
        }
    }

    // Check that all variables in the rhs appear in the lhs.
    for (const auto &var : rhs_vars_set) {
        if (lhs_vars_set.find(var) == lhs_vars_set.end()) {
            throw std::invalid_argument("Error in the Taylor decomposition of a system of equations: the variable '"
                                        + var + "' appears in the right-hand side but not in the left-hand side");
        }
    }

    // Cache the number of equations/variables.
    const auto n_eq = sys.size();
    assert(n_eq == lhs_vars.size());

    // Create the map for renaming the variables to u_i.
    // The renaming will be done following the order of the lhs
    // variables.
    std::unordered_map<std::string, std::string> repl_map;
    for (decltype(lhs_vars.size()) i = 0; i < lhs_vars.size(); ++i) {
        [[maybe_unused]] const auto eres = repl_map.emplace(lhs_vars[i], "u_" + detail::li_to_string(i));
        assert(eres.second);
    }

#if !defined(NDEBUG)
    // Store a copy of the original rhs for checking later.
    std::vector<expression> orig_rhs;
    for (const auto &[_, rhs_ex] : sys) {
        orig_rhs.push_back(rhs_ex);
    }
#endif

    // Rename the variables in the original equations.
    for (auto &[_, rhs_ex] : sys) {
        rename_variables(rhs_ex, repl_map);
    }

    // Init the vector containing the definitions
    // of the u variables. It begins with a list
    // of the original lhs variables of the system.
    std::vector<expression> u_vars_defs;
    for (const auto &var : lhs_vars) {
        u_vars_defs.emplace_back(variable{var});
    }

    // Create a copy of the original equations in terms of u variables.
    // We will be reusing this below.
    auto sys_copy = sys;

    // Run the decomposition on each equation.
    for (decltype(sys.size()) i = 0; i < sys.size(); ++i) {
        // Decompose the current equation.
        if (const auto dres = taylor_decompose_in_place(std::move(sys[i].second), u_vars_defs)) {
            // NOTE: if the equation was decomposed
            // (that is, it is not constant or a single variable),
            // we have to update the original definition
            // of the equation in sys_copy
            // so that it points to the u variable
            // that now represents it.
            sys_copy[i].second = expression{variable{"u_" + detail::li_to_string(dres)}};
        }
    }

    // Append the (possibly updated) definitions of the diff equations
    // in terms of u variables.
    for (auto &[_, rhs] : sys_copy) {
        u_vars_defs.emplace_back(std::move(rhs));
    }

#if !defined(NDEBUG)
    // Verify the decomposition.
    detail::verify_taylor_dec(orig_rhs, u_vars_defs);
#endif

    // Simplify the decomposition.
    u_vars_defs = detail::taylor_decompose_cse(u_vars_defs, n_eq);

#if !defined(NDEBUG)
    // Verify the simplified decomposition.
    detail::verify_taylor_dec(orig_rhs, u_vars_defs);
#endif

    return u_vars_defs;
}

llvm::Value *taylor_init_dbl(llvm_state &s, const expression &e, llvm::Value *arr)
{
    heyoka_assert_nonnull_ret(
        std::visit([&s, arr](const auto &arg) { return taylor_init_dbl(s, arg, arr); }, e.value()));
}

llvm::Value *taylor_init_ldbl(llvm_state &s, const expression &e, llvm::Value *arr)
{
    heyoka_assert_nonnull_ret(
        std::visit([&s, arr](const auto &arg) { return taylor_init_ldbl(s, arg, arr); }, e.value()));
}

llvm::Function *taylor_diff_dbl(llvm_state &s, const expression &e, std::uint32_t idx, const std::string &name,
                                std::uint32_t n_uvars, const std::unordered_map<std::uint32_t, number> &cd_uvars)
{
    auto visitor = [&s, idx, &name, n_uvars, &cd_uvars](const auto &v) -> llvm::Function * {
        using type = detail::uncvref_t<decltype(v)>;

        if constexpr (std::is_same_v<type, binary_operator> || std::is_same_v<type, function>) {
            return taylor_diff_dbl(s, v, idx, name, n_uvars, cd_uvars);
        } else {
            throw std::invalid_argument("Taylor derivatives can be computed only for binary operators or functions");
        }
    };

    heyoka_assert_nonnull_ret(std::visit(visitor, e.value()));
}

llvm::Function *taylor_diff_ldbl(llvm_state &s, const expression &e, std::uint32_t idx, const std::string &name,
                                 std::uint32_t n_uvars, const std::unordered_map<std::uint32_t, number> &cd_uvars)
{
    auto visitor = [&s, idx, &name, n_uvars, &cd_uvars](const auto &v) -> llvm::Function * {
        using type = detail::uncvref_t<decltype(v)>;

        if constexpr (std::is_same_v<type, binary_operator> || std::is_same_v<type, function>) {
            return taylor_diff_ldbl(s, v, idx, name, n_uvars, cd_uvars);
        } else {
            throw std::invalid_argument("Taylor derivatives can be computed only for binary operators or functions");
        }
    };

    heyoka_assert_nonnull_ret(std::visit(visitor, e.value()));
}

namespace detail
{

template <typename T>
template <typename U>
taylor_adaptive_impl<T>::taylor_adaptive_impl(p_tag, U sys, std::vector<T> state, T time, T rtol, T atol,
                                              unsigned opt_level)
    : m_state(std::move(state)), m_time(time), m_rtol(rtol), m_atol(atol),
      // NOTE: init to optimisation level 0 in order
      // to delay the optimisation pass.
      m_llvm{"adaptive taylor integrator", 0u}
{
    // Check input params.
    if (std::any_of(m_state.begin(), m_state.end(), [](const auto &x) { return !std::isfinite(x); })) {
        throw std::invalid_argument(
            "A non-finite value was detected in the initial state of an adaptive Taylor integrator");
    }

    if (m_state.size() != sys.size()) {
        throw std::invalid_argument("Inconsistent sizes detected in the initialization of an adaptive Taylor "
                                    "integrator: the state vector has a size of "
                                    + std::to_string(m_state.size()) + ", while the number of equations is "
                                    + std::to_string(sys.size()));
    }

    if (!std::isfinite(m_time)) {
        throw std::invalid_argument("Cannot initialise an adaptive Taylor integrator with a non-finite initial time");
    }

    if (!std::isfinite(m_rtol) || m_rtol <= 0) {
        throw std::invalid_argument(
            "The relative tolerance in an adaptive Taylor integrator must be finite and positive, but it is "
            + std::to_string(m_rtol) + " instead");
    }

    if (!std::isfinite(m_atol) || m_atol <= 0) {
        throw std::invalid_argument(
            "The absolute tolerance in an adaptive Taylor integrator must be finite and positive, but it is "
            + std::to_string(m_atol) + " instead");
    }

    // Compute the two possible orders for the integration, ensuring that
    // they are at least 2.
    const auto order_r_f = std::max(T(2), std::ceil(-std::log(m_rtol) / 2 + 1));
    const auto order_a_f = std::max(T(2), std::ceil(-std::log(m_atol) / 2 + 1));
    if (!std::isfinite(order_r_f) || !std::isfinite(order_a_f)) {
        throw std::invalid_argument(
            "The computation of the Taylor orders in an adaptive Taylor integrator produced non-finite values");
    }
    // NOTE: static cast is safe because we know that T is at least
    // a double-precision IEEE type.
    if (order_r_f > static_cast<T>(std::numeric_limits<std::uint32_t>::max())
        || order_a_f > static_cast<T>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::overflow_error("The computation of the max Taylor orders in an adaptive Taylor integrator resulted "
                                  "in an overflow condition");
    }
    // Record the Taylor orders.
    m_order_r = static_cast<std::uint32_t>(order_r_f);
    m_order_a = static_cast<std::uint32_t>(order_a_f);

    // Record the number of variables
    // before consuming sys.
    const auto n_vars = sys.size();

    // Add the variable-order functions for computing
    // the jet of normalised derivatives.
    // NOTE: in many cases m_order_r == m_order_a,
    // thus we could probably save some time by
    // creating (and optimising) only one function rather than 2.
    if constexpr (std::is_same_v<T, double>) {
        m_dc = m_llvm.add_taylor_jet_dbl("jet_r", sys, m_order_r);
        m_llvm.add_taylor_jet_dbl("jet_a", std::move(sys), m_order_a);
    } else if constexpr (std::is_same_v<T, long double>) {
        m_dc = m_llvm.add_taylor_jet_ldbl("jet_r", sys, m_order_r);
        m_llvm.add_taylor_jet_ldbl("jet_a", std::move(sys), m_order_a);
    } else {
        static_assert(always_false_v<T>, "Unhandled type.");
    }

    // Fetch the functions we just added.
    auto orig_jet_r = m_llvm.module().getFunction("jet_r");
    assert(orig_jet_r != nullptr);
    auto orig_jet_a = m_llvm.module().getFunction("jet_a");
    assert(orig_jet_a != nullptr);

    // Change their linkage to internal, as we don't
    // need to invoke them from outside the module.
    orig_jet_r->setLinkage(llvm::Function::InternalLinkage);
    orig_jet_a->setLinkage(llvm::Function::InternalLinkage);

    // Add the wrappers with fixed order.
    // The prototypes are both void(T *).
    std::vector<llvm::Type *> f_args(1u, llvm::PointerType::getUnqual(detail::to_llvm_type<T>(m_llvm.context())));
    auto *ft = llvm::FunctionType::get(m_llvm.builder().getVoidTy(), f_args, false);
    assert(ft != nullptr);

    // The relative tolerance function.
    auto *fr = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "jet_f_r", &m_llvm.module());
    assert(fr != nullptr);
    auto *bb = llvm::BasicBlock::Create(m_llvm.context(), "entry", fr);
    assert(bb != nullptr);
    m_llvm.builder().SetInsertPoint(bb);
    auto jet_call = m_llvm.builder().CreateCall(orig_jet_r, {fr->args().begin(), m_llvm.builder().getInt32(m_order_r)});
    assert(jet_call != nullptr);
    jet_call->setTailCall(true);
    m_llvm.builder().CreateRetVoid();
    m_llvm.verify_function("jet_f_r");

    // The absolute tolerance function.
    auto *fa = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "jet_f_a", &m_llvm.module());
    assert(fa != nullptr);
    bb = llvm::BasicBlock::Create(m_llvm.context(), "entry", fa);
    assert(bb != nullptr);
    m_llvm.builder().SetInsertPoint(bb);
    jet_call = m_llvm.builder().CreateCall(orig_jet_a, {fa->args().begin(), m_llvm.builder().getInt32(m_order_a)});
    assert(jet_call != nullptr);
    jet_call->setTailCall(true);
    m_llvm.builder().CreateRetVoid();
    m_llvm.verify_function("jet_f_a");

    // Change the optimisation level
    // and run the optimisation pass.
    m_llvm.set_opt_level(opt_level);
    m_llvm.optimise();

    // Store the IR before compiling.
    m_ir = m_llvm.dump();

    // Run the jit.
    m_llvm.compile();

    // Fetch the compiled function for computing
    // the jet of derivatives.
    m_jet_f_r = reinterpret_cast<jet_f_t>(m_llvm.jit_lookup("jet_f_r"));
    m_jet_f_a = reinterpret_cast<jet_f_t>(m_llvm.jit_lookup("jet_f_a"));

    // Init the jet vector. Its maximum size is n_vars * (max_order + 1).
    // NOTE: n_vars must be nonzero because we successfully
    // created a Taylor jet function from sys.
    using jet_size_t = decltype(m_jet.size());
    const auto max_order = std::max(m_order_r, m_order_a);
    if (max_order >= std::numeric_limits<jet_size_t>::max()
        || (static_cast<jet_size_t>(max_order) + 1u) > std::numeric_limits<jet_size_t>::max() / n_vars) {
        throw std::overflow_error("The computation of the size of the jet of derivatives in an adaptive Taylor "
                                  "integrator resulted in an overflow condition");
    }
    m_jet.resize((static_cast<jet_size_t>(max_order) + 1u) * n_vars);

    // Check the values of the derivatives
    // for the initial state.

    // Copy the current state to the order zero
    // of the jet of derivatives.
    auto jet_ptr = m_jet.data();
    std::copy(m_state.begin(), m_state.end(), jet_ptr);

    // Compute the jet of derivatives at max order.
    if (m_order_r > m_order_a) {
        m_jet_f_r(jet_ptr);
    } else {
        m_jet_f_a(jet_ptr);
    }

    // Check the computed derivatives, starting from order 1.
    if (std::any_of(jet_ptr + n_vars, jet_ptr + m_jet.size(), [](const T &x) { return !std::isfinite(x); })) {
        throw std::invalid_argument(
            "Non-finite value(s) detected in the jet of derivatives corresponding to the initial "
            "state of an adaptive Taylor integrator");
    }

    // Pre-compute the inverse orders. This spares
    // us a few divisions in the stepping function.
    m_inv_order.resize(static_cast<jet_size_t>(max_order) + 1u);
    for (jet_size_t i = 1; i < max_order + 1u; ++i) {
        m_inv_order[i] = 1 / static_cast<T>(i);
    }

    // Pre-compute the factors by which rho must
    // be multiplied in order to determine the
    // integration timestep.
    m_rhofac_r = 1 / (std::exp(T(1)) * std::exp(T(1))) * std::exp((T(-7) / T(10)) / (m_order_r - 1u));
    m_rhofac_a = 1 / (std::exp(T(1)) * std::exp(T(1))) * std::exp((T(-7) / T(10)) / (m_order_a - 1u));
}

template <typename T>
taylor_adaptive_impl<T>::taylor_adaptive_impl(std::vector<expression> sys, std::vector<T> state, T time, T rtol, T atol,
                                              unsigned opt_level)
    : taylor_adaptive_impl(p_tag{}, std::move(sys), std::move(state), time, rtol, atol, opt_level)
{
}

template <typename T>
taylor_adaptive_impl<T>::taylor_adaptive_impl(std::vector<std::pair<expression, expression>> sys, std::vector<T> state,
                                              T time, T rtol, T atol, unsigned opt_level)
    : taylor_adaptive_impl(p_tag{}, std::move(sys), std::move(state), time, rtol, atol, opt_level)
{
}

template <typename T>
taylor_adaptive_impl<T>::taylor_adaptive_impl(taylor_adaptive_impl &&) noexcept = default;

template <typename T>
taylor_adaptive_impl<T> &taylor_adaptive_impl<T>::operator=(taylor_adaptive_impl &&) noexcept = default;

template <typename T>
taylor_adaptive_impl<T>::~taylor_adaptive_impl() = default;

// Implementation detail to make a single integration timestep.
// The size of the timestep is automatically deduced. If
// LimitTimestep is true, then the integration timestep will be
// limited not to be greater than max_delta_t in absolute value.
// If Direction is true then the propagation is done forward
// in time, otherwise backwards. In any case max_delta_t can never
// be negative.
// The function will return a triple, containing
// a flag describing the outcome of the integration,
// the integration timestep that was used and the
// Taylor order that was used.
// NOTE: perhaps there's performance to be gained
// by moving the timestep deduction logic and the
// actual propagation in LLVM (e.g., unrolling
// over the number of variables).
// NOTE: the safer adaptive timestep from
// Jorba still needs to be implemented.
template <typename T>
template <bool LimitTimestep, bool Direction>
std::tuple<typename taylor_adaptive_impl<T>::outcome, T, std::uint32_t>
taylor_adaptive_impl<T>::step_impl([[maybe_unused]] T max_delta_t)
{
    assert(std::isfinite(max_delta_t));
    if constexpr (LimitTimestep) {
        assert(max_delta_t >= 0);
    } else {
        assert(max_delta_t == 0);
    }

    // Store the number of variables in the system.
    const auto nvars = static_cast<std::uint32_t>(m_state.size());

    // Compute the norm infinity in the state vector.
    T max_abs_state = 0;
    for (const auto &x : m_state) {
        // NOTE: can we do this check just once at the end of the loop?
        // Need to reason about NaNs.
        if (!std::isfinite(x)) {
            return std::tuple{outcome::err_nf_state, T(0), std::uint32_t(0)};
        }

        max_abs_state = std::max(max_abs_state, std::abs(x));
    }

    // Fetch the Taylor order for this timestep, which will be
    // either the absolute or relative one depending on the
    // norm infinity of the state vector.
    const auto use_abs_tol = m_rtol * max_abs_state <= m_atol;
    const auto order = use_abs_tol ? m_order_a : m_order_r;
    assert(order >= 2u);

    // Copy the current state to the order zero
    // of the jet of derivatives.
    auto jet_ptr = m_jet.data();
    std::copy(m_state.begin(), m_state.end(), jet_ptr);

    // Compute the jet of derivatives at the given order.
    if (use_abs_tol) {
        m_jet_f_a(jet_ptr);
    } else {
        m_jet_f_r(jet_ptr);
    }

    // Check the computed derivatives, starting from order 1.
    if (std::any_of(jet_ptr + nvars, jet_ptr + (order + 1u) * nvars, [](const T &x) { return !std::isfinite(x); })) {
        return std::tuple{outcome::err_nf_derivative, T(0), std::uint32_t(0)};
    }

    // Now we compute an estimation of the radius of convergence of the Taylor
    // series at orders order and order - 1.

    // First step is to determine the norm infinity of the derivatives
    // at orders order and order - 1.
    T max_abs_diff_o = 0, max_abs_diff_om1 = 0;
    for (std::uint32_t i = 0; i < nvars; ++i) {
        max_abs_diff_om1 = std::max(max_abs_diff_om1, std::abs(jet_ptr[(order - 1u) * nvars + i]));
        max_abs_diff_o = std::max(max_abs_diff_o, std::abs(jet_ptr[order * nvars + i]));
    }

    // Estimate rho at orders order - 1 and order.
    const auto rho_om1 = use_abs_tol ? std::pow(1 / max_abs_diff_om1, m_inv_order[order - 1u])
                                     : std::pow(max_abs_state / max_abs_diff_om1, m_inv_order[order - 1u]);
    const auto rho_o = use_abs_tol ? std::pow(1 / max_abs_diff_o, m_inv_order[order])
                                   : std::pow(max_abs_state / max_abs_diff_o, m_inv_order[order]);
    if (std::isnan(rho_om1) || std::isnan(rho_o)) {
        return std::tuple{outcome::err_nan_rho, T(0), std::uint32_t(0)};
    }

    // From this point on, the only possible
    // outcomes are success or time_limit.
    auto oc = outcome::success;

    // Take the minimum.
    const auto rho_m = std::min(rho_o, rho_om1);

    // Now determine the step size using the formula with safety factors.
    auto h = rho_m * (use_abs_tol ? m_rhofac_a : m_rhofac_r);
    if constexpr (LimitTimestep) {
        // Make sure h does not exceed max_delta_t.
        if (h > max_delta_t) {
            h = max_delta_t;
            oc = outcome::time_limit;
        }
    }
    if constexpr (!Direction) {
        // When propagating backwards, invert the sign of the timestep.
        h = -h;
    }

    // Update the state.
    auto cur_h = h;
    for (std::uint32_t o = 1; o < order + 1u; ++o, cur_h *= h) {
        const auto d_ptr = jet_ptr + o * nvars;

        for (std::uint32_t i = 0; i < nvars; ++i) {
            // NOTE: use FMA wrappers here?
            m_state[i] += cur_h * d_ptr[i];
        }
    }

    // Update the time.
    m_time += h;

    return std::tuple{oc, h, order};
}

template <typename T>
std::tuple<typename taylor_adaptive_impl<T>::outcome, T, std::uint32_t> taylor_adaptive_impl<T>::step()
{
    return step_impl<false, true>(0);
}

template <typename T>
std::tuple<typename taylor_adaptive_impl<T>::outcome, T, std::uint32_t> taylor_adaptive_impl<T>::step_backward()
{
    return step_impl<false, false>(0);
}

template <typename T>
std::tuple<typename taylor_adaptive_impl<T>::outcome, T, std::uint32_t> taylor_adaptive_impl<T>::step(T max_delta_t)
{
    if (!std::isfinite(max_delta_t)) {
        throw std::invalid_argument(
            "A non-finite max_delta_t was passed to the step() function of an adaptive Taylor integrator");
    }

    if (max_delta_t >= 0) {
        return step_impl<true, true>(max_delta_t);
    } else {
        return step_impl<true, false>(-max_delta_t);
    }
}

template <typename T>
std::tuple<typename taylor_adaptive_impl<T>::outcome, T, T, std::uint32_t, std::uint32_t, std::size_t>
taylor_adaptive_impl<T>::propagate_for(T delta_t, std::size_t max_steps)
{
    return propagate_until(m_time + delta_t, max_steps);
}

template <typename T>
std::tuple<typename taylor_adaptive_impl<T>::outcome, T, T, std::uint32_t, std::uint32_t, std::size_t>
taylor_adaptive_impl<T>::propagate_until(T t, std::size_t max_steps)
{
    if (!std::isfinite(t)) {
        throw std::invalid_argument(
            "A non-finite time was passed to the propagate_until() function of an adaptive Taylor integrator");
    }

    // Initial values for the counter,
    // the min/max abs of the integration
    // timesteps, and min/max Taylor orders.
    std::size_t step_counter = 0;
    T min_h = std::numeric_limits<T>::infinity(), max_h = 0;
    std::uint32_t min_order = std::numeric_limits<std::uint32_t>::max(), max_order = 0;

    if (t == m_time) {
        return std::tuple{outcome::time_limit, min_h, max_h, min_order, max_order, step_counter};
    }

    if ((t > m_time && !std::isfinite(t - m_time)) || (t < m_time && !std::isfinite(m_time - t))) {
        throw std::overflow_error("The time limit passed to the propagate_until() function is too large and it "
                                  "results in an overflow condition");
    }

    if (t > m_time) {
        while (true) {
            const auto [res, h, t_order] = step_impl<true, true>(t - m_time);

            if (res != outcome::success && res != outcome::time_limit) {
                return std::tuple{res, min_h, max_h, min_order, max_order, step_counter};
            }

            // Update the number of steps
            // completed successfully.
            ++step_counter;

            // Update min/max Taylor orders.
            min_order = std::min(min_order, t_order);
            max_order = std::max(max_order, t_order);

            // Break out if the time limit is reached,
            // *before* updating the min_h/max_h values.
            if (t <= m_time) {
                break;
            }

            // Update min_h/max_h.
            assert(h >= 0);
            min_h = std::min(min_h, h);
            max_h = std::max(max_h, h);

            // Check the max number of steps stopping criterion.
            if (max_steps != 0u && step_counter == max_steps) {
                return std::tuple{outcome::step_limit, min_h, max_h, min_order, max_order, step_counter};
            }
        }
    } else {
        while (true) {
            const auto [res, h, t_order] = step_impl<true, false>(m_time - t);

            if (res != outcome::success && res != outcome::time_limit) {
                return std::tuple{res, min_h, max_h, min_order, max_order, step_counter};
            }

            ++step_counter;

            min_order = std::min(min_order, t_order);
            max_order = std::max(max_order, t_order);

            if (t >= m_time) {
                break;
            }

            min_h = std::min(min_h, std::abs(h));
            max_h = std::max(max_h, std::abs(h));

            if (max_steps != 0u && step_counter == max_steps) {
                return std::tuple{outcome::step_limit, min_h, max_h, min_order, max_order, step_counter};
            }
        }
    }

    return std::tuple{outcome::time_limit, min_h, max_h, min_order, max_order, step_counter};
}

template <typename T>
void taylor_adaptive_impl<T>::set_time(T t)
{
    if (!std::isfinite(t)) {
        throw std::invalid_argument("Non-finite time " + std::to_string(t)
                                    + " passed to the set_time() function of an adaptive Taylor integrator");
    }

    m_time = t;
}

template <typename T>
void taylor_adaptive_impl<T>::set_state(const std::vector<T> &state)
{
    if (&state == &m_state) {
        // Check that state and m_state are not the same object,
        // otherwise std::copy() cannot be used.
        return;
    }

    if (state.size() != m_state.size()) {
        throw std::invalid_argument(
            "The state vector passed to the set_state() function of an adaptive Taylor integrator has a size of "
            + std::to_string(state.size()) + ", which is inconsistent with the size of the current state vector ("
            + std::to_string(m_state.size()) + ")");
    }

    if (std::any_of(state.begin(), state.end(), [](const T &x) { return !std::isfinite(x); })) {
        throw std::invalid_argument(
            "A non-finite state vector was passed to the set_state() function of an adaptive Taylor integrator");
    }

    // Do the copy.
    std::copy(state.begin(), state.end(), m_state.begin());
}

template <typename T>
const std::string &taylor_adaptive_impl<T>::get_ir() const
{
    return m_ir;
}

template <typename T>
const std::vector<expression> &taylor_adaptive_impl<T>::get_decomposition() const
{
    return m_dc;
}

// Explicit instantiation of the implementation classes.
template class taylor_adaptive_impl<double>;
template class taylor_adaptive_impl<long double>;

} // namespace detail

} // namespace heyoka
