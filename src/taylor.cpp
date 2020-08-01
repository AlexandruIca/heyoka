// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

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

#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/math_wrappers.hpp>
#include <heyoka/detail/string_conv.hpp>
#include <heyoka/detail/type_traits.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>
#include <heyoka/variable.hpp>

#include <heyoka/detail/simple_timer.hpp>

namespace heyoka
{

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

namespace detail
{

namespace
{

// Add a function to the llvm_state s for the evaluation
// of a polynomial via Estrin's scheme. The polynomial in question
// is the Taylor expansion that updates the state in a Taylor
// integrator at the end of the timestep. nvars is the number
// of variables in the ODE system, order is the Taylor order,
// batch_size the batch size (will be 1 in the scalar
// Taylor integrator, > 1 in the batch integrator).
template <typename T>
void taylor_add_estrin(llvm_state &s, const std::string &name, std::uint32_t nvars, std::uint32_t order,
                       std::uint32_t batch_size)
{
    assert(s.module().getNamedValue(name) == nullptr);

    auto &builder = s.builder();

    // Fetch the SIMD vector size from s.
    const auto vector_size = s.vector_size<T>();

    // Prepare the main function prototype. The arguments are:
    // - an output pointer into which we will be writing
    //   the updated state,
    // - an input pointer with the jet of derivatives
    //   (which also includes the current state at order 0),
    // - an input pointer with the integration timesteps.
    std::vector<llvm::Type *> fargs(3u, llvm::PointerType::getUnqual(to_llvm_type<T>(s.context())));
    // The function does not return anything.
    auto *ft = llvm::FunctionType::get(builder.getVoidTy(), fargs, false);
    assert(ft != nullptr);
    // Now create the function.
    auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, &s.module());
    assert(f != nullptr);

    // Setup the function arguments.
    auto out_ptr = f->args().begin();
    out_ptr->setName("out_ptr");
    out_ptr->addAttr(llvm::Attribute::WriteOnly);
    out_ptr->addAttr(llvm::Attribute::NoCapture);
    out_ptr->addAttr(llvm::Attribute::NoAlias);

    auto jet_ptr = out_ptr + 1;
    jet_ptr->setName("jet_ptr");
    jet_ptr->addAttr(llvm::Attribute::ReadOnly);
    jet_ptr->addAttr(llvm::Attribute::NoCapture);
    jet_ptr->addAttr(llvm::Attribute::NoAlias);

    auto h_ptr = jet_ptr + 1;
    h_ptr->setName("h_ptr");
    h_ptr->addAttr(llvm::Attribute::ReadOnly);
    h_ptr->addAttr(llvm::Attribute::NoCapture);
    h_ptr->addAttr(llvm::Attribute::NoAlias);

    // Create a new basic block to start insertion into.
    auto *bb = llvm::BasicBlock::Create(s.context(), "entry", f);
    assert(bb != nullptr);
    builder.SetInsertPoint(bb);

    // Helper to run the Estrin scheme on the polynomial
    // whose coefficients are stored in cf_vec. It will
    // shrink cf_vec until it contains only 1 term,
    // the result of the evaluation.
    // https://en.wikipedia.org/wiki/Estrin%27s_scheme
    auto run_estrin = [&builder](std::vector<llvm::Value *> &cf_vec, llvm::Value *h) {
        assert(!cf_vec.empty());

        while (cf_vec.size() != 1u) {
            // Fill in the vector of coefficients for the next iteration.
            std::vector<llvm::Value *> new_cf_vec;

            for (decltype(cf_vec.size()) i = 0; i < cf_vec.size(); i += 2u) {
                if (i + 1u == cf_vec.size()) {
                    // We are at the last element of the vector
                    // and the size of the vector is odd. Just append
                    // the existing coefficient.
                    new_cf_vec.push_back(cf_vec[i]);
                } else {
                    new_cf_vec.push_back(builder.CreateFAdd(cf_vec[i], builder.CreateFMul(cf_vec[i + 1u], h)));
                }
            }

            // Replace the vector of coefficients
            // with the new one.
            new_cf_vec.swap(cf_vec);

            // Update h if we are not at the last iteration.
            if (cf_vec.size() != 1u) {
                h = builder.CreateFMul(h, h);
            }
        }
    };

    if (vector_size == 0u) {
        // Scalar mode.
        for (std::uint32_t var_idx = 0; var_idx < nvars; ++var_idx) {
            for (std::uint32_t batch_idx = 0; batch_idx < batch_size; ++batch_idx) {
                // Load the polynomial coefficients from the jet
                // of derivatives.
                std::vector<llvm::Value *> cf_vec;
                for (std::uint32_t o = 0; o < order + 1u; ++o) {
                    auto cf_ptr = builder.CreateInBoundsGEP(
                        jet_ptr, builder.getInt32(o * nvars * batch_size + var_idx * batch_size + batch_idx),
                        "cf_" + li_to_string(var_idx) + "_" + li_to_string(batch_idx) + "_" + li_to_string(o) + "_ptr");
                    cf_vec.emplace_back(builder.CreateLoad(
                        cf_ptr, "cf_" + li_to_string(var_idx) + "_" + li_to_string(batch_idx) + "_" + li_to_string(o)));
                }

                // Load the integration timestep. This is common to all
                // variables and varies only by batch_idx.
                llvm::Value *h = builder.CreateLoad(builder.CreateInBoundsGEP(h_ptr, builder.getInt32(batch_idx),
                                                                              "h_" + li_to_string(batch_idx) + "_ptr"),
                                                    "h_" + li_to_string(batch_idx));

                // Run the Estrin scheme.
                run_estrin(cf_vec, h);

                // Store the result of the evaluation.
                auto res_ptr = builder.CreateInBoundsGEP(out_ptr, builder.getInt32(var_idx * batch_size + batch_idx),
                                                         "res_" + li_to_string(var_idx) + "_" + li_to_string(batch_idx)
                                                             + "_ptr");
                builder.CreateStore(cf_vec[0], res_ptr);
            }
        }
    } else {
        // Vector mode.
        const auto n_sub_batch = batch_size / vector_size;

        for (std::uint32_t var_idx = 0; var_idx < nvars; ++var_idx) {
            for (std::uint32_t batch_idx = 0; batch_idx < n_sub_batch * vector_size; batch_idx += vector_size) {
                std::vector<llvm::Value *> cf_vec;
                for (std::uint32_t o = 0; o < order + 1u; ++o) {
                    auto cf_ptr = builder.CreateInBoundsGEP(
                        jet_ptr, builder.getInt32(o * nvars * batch_size + var_idx * batch_size + batch_idx),
                        "cf_" + li_to_string(var_idx) + "_" + li_to_string(batch_idx) + "_" + li_to_string(o) + "_ptr");
                    cf_vec.emplace_back(load_vector_from_memory(builder, cf_ptr, vector_size,
                                                                "cf_" + li_to_string(var_idx) + "_"
                                                                    + li_to_string(batch_idx) + "_" + li_to_string(o)));
                }

                llvm::Value *h
                    = load_vector_from_memory(builder,
                                              builder.CreateInBoundsGEP(h_ptr, builder.getInt32(batch_idx),
                                                                        "h_" + li_to_string(batch_idx) + "_ptr"),
                                              vector_size, "h_" + li_to_string(batch_idx));

                run_estrin(cf_vec, h);

                auto res_ptr = builder.CreateInBoundsGEP(out_ptr, builder.getInt32(var_idx * batch_size + batch_idx),
                                                         "res_" + li_to_string(var_idx) + "_" + li_to_string(batch_idx)
                                                             + "_ptr");

                detail::store_vector_to_memory(builder, res_ptr, cf_vec[0], vector_size);
            }

            for (std::uint32_t batch_idx = n_sub_batch * vector_size; batch_idx < batch_size; ++batch_idx) {
                std::vector<llvm::Value *> cf_vec;
                for (std::uint32_t o = 0; o < order + 1u; ++o) {
                    auto cf_ptr = builder.CreateInBoundsGEP(
                        jet_ptr, builder.getInt32(o * nvars * batch_size + var_idx * batch_size + batch_idx),
                        "cf_" + li_to_string(var_idx) + "_" + li_to_string(batch_idx) + "_" + li_to_string(o) + "_ptr");
                    cf_vec.emplace_back(builder.CreateLoad(
                        cf_ptr, "cf_" + li_to_string(var_idx) + "_" + li_to_string(batch_idx) + "_" + li_to_string(o)));
                }

                llvm::Value *h = builder.CreateLoad(builder.CreateInBoundsGEP(h_ptr, builder.getInt32(batch_idx),
                                                                              "h_" + li_to_string(batch_idx) + "_ptr"),
                                                    "h_" + li_to_string(batch_idx));

                run_estrin(cf_vec, h);

                auto res_ptr = builder.CreateInBoundsGEP(out_ptr, builder.getInt32(var_idx * batch_size + batch_idx),
                                                         "res_" + li_to_string(var_idx) + "_" + li_to_string(batch_idx)
                                                             + "_ptr");
                builder.CreateStore(cf_vec[0], res_ptr);
            }
        }
    }

    builder.CreateRetVoid();

    s.verify_function(name);
}

} // namespace

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
    if (std::any_of(m_state.begin(), m_state.end(), [](const auto &x) { return !detail::isfinite(x); })) {
        throw std::invalid_argument(
            "A non-finite value was detected in the initial state of an adaptive Taylor integrator");
    }

    if (m_state.size() != sys.size()) {
        throw std::invalid_argument("Inconsistent sizes detected in the initialization of an adaptive Taylor "
                                    "integrator: the state vector has a dimension of "
                                    + std::to_string(m_state.size()) + ", while the number of equations is "
                                    + std::to_string(sys.size()));
    }

    if (!detail::isfinite(m_time)) {
        throw std::invalid_argument("Cannot initialise an adaptive Taylor integrator with a non-finite initial time of "
                                    + detail::li_to_string(m_time));
    }

    if (!detail::isfinite(m_rtol) || m_rtol <= 0) {
        throw std::invalid_argument(
            "The relative tolerance in an adaptive Taylor integrator must be finite and positive, but it is "
            + li_to_string(m_rtol) + " instead");
    }

    if (!detail::isfinite(m_atol) || m_atol <= 0) {
        throw std::invalid_argument(
            "The absolute tolerance in an adaptive Taylor integrator must be finite and positive, but it is "
            + li_to_string(m_atol) + " instead");
    }

    // Compute the two possible orders for the integration, ensuring that
    // they are at least 2.
    const auto order_r_f = std::max(T(2), detail::ceil(-detail::log(m_rtol) / 2 + 1));
    const auto order_a_f = std::max(T(2), detail::ceil(-detail::log(m_atol) / 2 + 1));

    if (!detail::isfinite(order_r_f) || !detail::isfinite(order_a_f)) {
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

    // Add the functions for computing
    // the jet of normalised derivatives.
    m_dc = m_llvm.add_taylor_jet_batch<T>("jet_r", sys, m_order_r, 1);
    if (m_order_r != m_order_a) {
        // NOTE: add the absolute tolerance jet function only
        // if the relative and absolute orders differ.
        m_llvm.add_taylor_jet_batch<T>("jet_a", std::move(sys), m_order_a, 1);
    }

    // Add the functions to update the state vector.
    // NOTE: static cast is safe because we successfully
    // added the functions for the derivatives.
    taylor_add_estrin<T>(m_llvm, "estrin_r", static_cast<std::uint32_t>(n_vars), m_order_r, 1);
    if (m_order_r != m_order_a) {
        taylor_add_estrin<T>(m_llvm, "estrin_a", static_cast<std::uint32_t>(n_vars), m_order_a, 1);
    }

    // Change the optimisation level
    // and run the optimisation pass.
    m_llvm.opt_level() = opt_level;
    m_llvm.optimise();

    // Run the jit.
    m_llvm.compile();

    // Fetch the compiled function for computing
    // the jet of derivatives.
    m_jet_f_r = m_llvm.fetch_taylor_jet_batch<T>("jet_r");
    if (m_order_r == m_order_a) {
        m_jet_f_a = m_jet_f_r;
    } else {
        m_jet_f_a = m_llvm.fetch_taylor_jet_batch<T>("jet_a");
    }

    // Fetch the function for updating the state vector
    // at the end of the integration timestep.
    m_update_f_r = reinterpret_cast<s_update_f_t>(m_llvm.jit_lookup("estrin_r"));
    if (m_order_r == m_order_a) {
        m_update_f_a = m_update_f_r;
    } else {
        m_update_f_a = reinterpret_cast<s_update_f_t>(m_llvm.jit_lookup("estrin_a"));
    }

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
    if (std::any_of(jet_ptr + n_vars, jet_ptr + m_jet.size(), [](const T &x) { return !detail::isfinite(x); })) {
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
    m_rhofac_r = 1 / (detail::exp(T(1)) * detail::exp(T(1))) * detail::exp((T(-7) / T(10)) / (m_order_r - 1u));
    m_rhofac_a = 1 / (detail::exp(T(1)) * detail::exp(T(1))) * detail::exp((T(-7) / T(10)) / (m_order_a - 1u));
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
taylor_adaptive_impl<T>::taylor_adaptive_impl(const taylor_adaptive_impl &other)
    // NOTE: make a manual copy of all members, apart from the function pointers.
    : m_state(other.m_state), m_time(other.m_time), m_rtol(other.m_rtol), m_atol(other.m_atol),
      m_order_r(other.m_order_r), m_order_a(other.m_order_a), m_inv_order(other.m_inv_order),
      m_rhofac_r(other.m_rhofac_r), m_rhofac_a(other.m_rhofac_a), m_llvm(other.m_llvm), m_jet(other.m_jet),
      m_dc(other.m_dc)
{
    // Fetch the compiled functions for computing
    // the jet of derivatives.
    m_jet_f_r = m_llvm.fetch_taylor_jet_batch<T>("jet_r");
    if (m_order_r == m_order_a) {
        m_jet_f_a = m_jet_f_r;
    } else {
        m_jet_f_a = m_llvm.fetch_taylor_jet_batch<T>("jet_a");
    }

    // Same for the functions for the state update.
    m_update_f_r = reinterpret_cast<s_update_f_t>(m_llvm.jit_lookup("estrin_r"));
    if (m_order_r == m_order_a) {
        m_update_f_a = m_update_f_r;
    } else {
        m_update_f_a = reinterpret_cast<s_update_f_t>(m_llvm.jit_lookup("estrin_a"));
    }
}

template <typename T>
taylor_adaptive_impl<T>::taylor_adaptive_impl(taylor_adaptive_impl &&) noexcept = default;

template <typename T>
taylor_adaptive_impl<T> &taylor_adaptive_impl<T>::operator=(const taylor_adaptive_impl &other)
{
    if (this != &other) {
        *this = taylor_adaptive_impl(other);
    }

    return *this;
}

template <typename T>
taylor_adaptive_impl<T> &taylor_adaptive_impl<T>::operator=(taylor_adaptive_impl &&) noexcept = default;

template <typename T>
taylor_adaptive_impl<T>::~taylor_adaptive_impl() = default;

// Implementation detail to make a single integration timestep.
// The magnitude of the timestep is automatically deduced, but it will
// always be not greater than abs(max_delta_t). The propagation
// is done forward in time if max_delta_t >= 0, backwards in
// time otherwise.
//
// The function will return a triple, containing
// a flag describing the outcome of the integration,
// the integration timestep that was used and the
// Taylor order that was used.
//
// NOTE: the safer adaptive timestep from
// Jorba still needs to be implemented.
template <typename T>
std::tuple<taylor_outcome, T, std::uint32_t> taylor_adaptive_impl<T>::step_impl(T max_delta_t)
{
    assert(!detail::isnan(max_delta_t));

    // Cache abs(max_delta_t).
    const auto abs_max_delta_t = detail::abs(max_delta_t);

    // Propagate backwards?
    const auto backwards = max_delta_t < T(0);

    // Cache the number of variables in the system.
    const auto nvars = static_cast<std::uint32_t>(m_state.size());

    // Compute the norm infinity in the state vector.
    T max_abs_state(0);
    for (const auto &x : m_state) {
        // NOTE: can we do this check just once at the end of the loop?
        // Need to reason about NaNs.
        if (!detail::isfinite(x)) {
            return std::tuple{taylor_outcome::err_nf_state, T(0), std::uint32_t(0)};
        }

        max_abs_state = std::max(max_abs_state, detail::abs(x));
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

    // Now we compute an estimation of the radius of convergence of the Taylor
    // series at orders order and order - 1.

    // First step is to determine the norm infinity of the derivatives
    // at orders order and order - 1.
    T max_abs_diff_o(0), max_abs_diff_om1(0);
    for (std::uint32_t i = 0; i < nvars; ++i) {
        const auto diff_om1 = jet_ptr[(order - 1u) * nvars + i];
        const auto diff_o = jet_ptr[order * nvars + i];

        if (!detail::isfinite(diff_om1) || !detail::isfinite(diff_o)) {
            // Non-finite derivatives detected, return failure.
            return std::tuple{taylor_outcome::err_nf_derivative, T(0), std::uint32_t(0)};
        }

        // Update the max abs.
        max_abs_diff_om1 = std::max(max_abs_diff_om1, detail::abs(diff_om1));
        max_abs_diff_o = std::max(max_abs_diff_o, detail::abs(diff_o));
    }

    // Estimate rho at orders order - 1 and order.
    const auto rho_om1 = use_abs_tol ? detail::pow(1 / max_abs_diff_om1, m_inv_order[order - 1u])
                                     : detail::pow(max_abs_state / max_abs_diff_om1, m_inv_order[order - 1u]);
    const auto rho_o = use_abs_tol ? detail::pow(1 / max_abs_diff_o, m_inv_order[order])
                                   : detail::pow(max_abs_state / max_abs_diff_o, m_inv_order[order]);
    if (detail::isnan(rho_om1) || detail::isnan(rho_o)) {
        return std::tuple{taylor_outcome::err_nan_rho, T(0), std::uint32_t(0)};
    }

    // From this point on, the only possible
    // outcomes are success or time_limit.
    auto oc = taylor_outcome::success;

    // Take the minimum.
    const auto rho_m = std::min(rho_o, rho_om1);

    // Now determine the step size using the formula with safety factors.
    auto h = rho_m * (use_abs_tol ? m_rhofac_a : m_rhofac_r);

    // Make sure h does not exceed abs(max_delta_t).
    if (h > abs_max_delta_t) {
        h = abs_max_delta_t;
        oc = taylor_outcome::time_limit;
    }

    if (backwards) {
        // When propagating backwards, invert the sign of the timestep.
        h = -h;
    }

    if (use_abs_tol) {
        m_update_f_a(m_state.data(), jet_ptr, &h);
    } else {
        m_update_f_r(m_state.data(), jet_ptr, &h);
    }

    // Update the time.
    m_time += h;

    return std::tuple{oc, h, order};
}

template <typename T>
std::tuple<taylor_outcome, T, std::uint32_t> taylor_adaptive_impl<T>::step()
{
    // NOTE: time limit +inf means integration forward in time
    // and no time limit.
    return step_impl(std::numeric_limits<T>::infinity());
}

template <typename T>
std::tuple<taylor_outcome, T, std::uint32_t> taylor_adaptive_impl<T>::step_backward()
{
    return step_impl(-std::numeric_limits<T>::infinity());
}

template <typename T>
std::tuple<taylor_outcome, T, std::uint32_t> taylor_adaptive_impl<T>::step(T max_delta_t)
{
    if (detail::isnan(max_delta_t)) {
        throw std::invalid_argument(
            "A NaN max_delta_t was passed to the step() function of an adaptive Taylor integrator");
    }

    return step_impl(max_delta_t);
}

template <typename T>
std::tuple<taylor_outcome, T, T, std::uint32_t, std::uint32_t, std::size_t>
taylor_adaptive_impl<T>::propagate_for(T delta_t, std::size_t max_steps)
{
    return propagate_until(m_time + delta_t, max_steps);
}

template <typename T>
std::tuple<taylor_outcome, T, T, std::uint32_t, std::uint32_t, std::size_t>
taylor_adaptive_impl<T>::propagate_until(T t, std::size_t max_steps)
{
    if (!detail::isfinite(t)) {
        throw std::invalid_argument(
            "A non-finite time was passed to the propagate_until() function of an adaptive Taylor integrator");
    }

    // Initial values for the counter,
    // the min/max abs of the integration
    // timesteps, and min/max Taylor orders.
    std::size_t step_counter = 0;
    T min_h = std::numeric_limits<T>::infinity(), max_h(0);
    std::uint32_t min_order = std::numeric_limits<std::uint32_t>::max(), max_order = 0;

    if (t == m_time) {
        return std::tuple{taylor_outcome::time_limit, min_h, max_h, min_order, max_order, step_counter};
    }

    if ((t > m_time && !detail::isfinite(t - m_time)) || (t < m_time && !detail::isfinite(m_time - t))) {
        throw std::overflow_error("The time limit passed to the propagate_until() function is too large and it "
                                  "results in an overflow condition");
    }

    if (t > m_time) {
        while (true) {
            const auto [res, h, t_order] = step_impl(t - m_time);

            if (res != taylor_outcome::success && res != taylor_outcome::time_limit) {
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
            if (m_time >= t) {
                break;
            }

            // Update min_h/max_h.
            assert(h >= 0);
            min_h = std::min(min_h, h);
            max_h = std::max(max_h, h);

            // Check the max number of steps stopping criterion.
            if (max_steps != 0u && step_counter == max_steps) {
                return std::tuple{taylor_outcome::step_limit, min_h, max_h, min_order, max_order, step_counter};
            }
        }
    } else {
        while (true) {
            const auto [res, h, t_order] = step_impl(t - m_time);

            if (res != taylor_outcome::success && res != taylor_outcome::time_limit) {
                return std::tuple{res, min_h, max_h, min_order, max_order, step_counter};
            }

            ++step_counter;

            min_order = std::min(min_order, t_order);
            max_order = std::max(max_order, t_order);

            if (m_time <= t) {
                break;
            }

            assert(h < 0);
            min_h = std::min(min_h, -h);
            max_h = std::max(max_h, -h);

            if (max_steps != 0u && step_counter == max_steps) {
                return std::tuple{taylor_outcome::step_limit, min_h, max_h, min_order, max_order, step_counter};
            }
        }
    }

    return std::tuple{taylor_outcome::time_limit, min_h, max_h, min_order, max_order, step_counter};
}

template <typename T>
void taylor_adaptive_impl<T>::set_time(T t)
{
    if (!detail::isfinite(t)) {
        throw std::invalid_argument("Non-finite time " + detail::li_to_string(t)
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

    if (std::any_of(state.begin(), state.end(), [](const T &x) { return !detail::isfinite(x); })) {
        throw std::invalid_argument(
            "A non-finite state vector was passed to the set_state() function of an adaptive Taylor integrator");
    }

    // Do the copy.
    std::copy(state.begin(), state.end(), m_state.begin());
}

template <typename T>
std::string taylor_adaptive_impl<T>::get_ir() const
{
    return m_llvm.dump_ir();
}

template <typename T>
const std::vector<expression> &taylor_adaptive_impl<T>::get_decomposition() const
{
    return m_dc;
}

// Explicit instantiation of the implementation classes.
template class taylor_adaptive_impl<double>;
template class taylor_adaptive_impl<long double>;

#if defined(HEYOKA_HAVE_REAL128)

template class taylor_adaptive_impl<mppp::real128>;

#endif

} // namespace detail

namespace detail
{

template <typename T>
template <typename U>
taylor_adaptive_batch_impl<T>::taylor_adaptive_batch_impl(p_tag, U sys, std::vector<T> states, std::vector<T> times,
                                                          T rtol, T atol, std::uint32_t batch_size, unsigned opt_level)
    : m_batch_size(batch_size), m_states(std::move(states)), m_times(std::move(times)), m_rtol(rtol), m_atol(atol),
      // NOTE: init to optimisation level 0 in order
      // to delay the optimisation pass.
      m_llvm{"adaptive batch taylor integrator", 0u}
{
    // Check input params.
    if (m_batch_size == 0u) {
        throw std::invalid_argument("The batch size in an adaptive Taylor integrator cannot be zero");
    }

    if (std::any_of(m_states.begin(), m_states.end(), [](const auto &x) { return !detail::isfinite(x); })) {
        throw std::invalid_argument(
            "A non-finite value was detected in the initial state of an adaptive Taylor integrator");
    }

    if (m_states.size() % m_batch_size != 0u) {
        throw std::invalid_argument("Invalid size detected in the initialization of an adaptive Taylor "
                                    "integrator: the state vector has a size of "
                                    + std::to_string(m_states.size()) + ", which is not a multiple of the batch size ("
                                    + std::to_string(m_batch_size) + ")");
    }

    if (m_states.size() / m_batch_size != sys.size()) {
        throw std::invalid_argument("Inconsistent sizes detected in the initialization of an adaptive Taylor "
                                    "integrator: the state vector has a dimension of "
                                    + std::to_string(m_states.size() / m_batch_size)
                                    + ", while the number of equations is " + std::to_string(sys.size()));
    }

    if (std::any_of(m_times.begin(), m_times.end(), [](const auto &x) { return !detail::isfinite(x); })) {
        throw std::invalid_argument(
            "A non-finite initial time was detected in the initialisation of an adaptive Taylor integrator");
    }

    if (!detail::isfinite(m_rtol) || m_rtol <= 0) {
        throw std::invalid_argument(
            "The relative tolerance in an adaptive Taylor integrator must be finite and positive, but it is "
            + li_to_string(m_rtol) + " instead");
    }

    if (!detail::isfinite(m_atol) || m_atol <= 0) {
        throw std::invalid_argument(
            "The absolute tolerance in an adaptive Taylor integrator must be finite and positive, but it is "
            + li_to_string(m_atol) + " instead");
    }

    // Compute the two possible orders for the integration, ensuring that
    // they are at least 2.
    const auto order_r_f = std::max(T(2), detail::ceil(-detail::log(m_rtol) / 2 + 1));
    const auto order_a_f = std::max(T(2), detail::ceil(-detail::log(m_atol) / 2 + 1));

    if (!detail::isfinite(order_r_f) || !detail::isfinite(order_a_f)) {
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

    // Add the functions for computing
    // the jet of normalised derivatives.
    m_dc = m_llvm.add_taylor_jet_batch<T>("jet_r", sys, m_order_r, batch_size);
    if (m_order_r != m_order_a) {
        // NOTE: add the absolute tolerance jet function only
        // if the relative and absolute orders differ.
        m_llvm.add_taylor_jet_batch<T>("jet_a", std::move(sys), m_order_a, batch_size);
    }

    // Add the functions to update the state vector.
    // NOTE: static cast is safe because we successfully
    // added the functions for the derivatives.
    taylor_add_estrin<T>(m_llvm, "estrin_r", static_cast<std::uint32_t>(n_vars), m_order_r, batch_size);
    if (m_order_r != m_order_a) {
        taylor_add_estrin<T>(m_llvm, "estrin_a", static_cast<std::uint32_t>(n_vars), m_order_a, batch_size);
    }

    // Change the optimisation level
    // and run the optimisation pass.
    m_llvm.opt_level() = opt_level;
    m_llvm.optimise();

    // Run the jit.
    m_llvm.compile();

    // Fetch the compiled functions for computing
    // the jet of derivatives.
    m_jet_f_r = m_llvm.fetch_taylor_jet_batch<T>("jet_r");
    if (m_order_r == m_order_a) {
        m_jet_f_a = m_jet_f_r;
    } else {
        m_jet_f_a = m_llvm.fetch_taylor_jet_batch<T>("jet_a");
    }

    // Fetch the function for updating the state vector
    // at the end of the integration timestep.
    m_update_f_r = reinterpret_cast<s_update_f_t>(m_llvm.jit_lookup("estrin_r"));
    if (m_order_r == m_order_a) {
        m_update_f_a = m_update_f_r;
    } else {
        m_update_f_a = reinterpret_cast<s_update_f_t>(m_llvm.jit_lookup("estrin_a"));
    }

    // Init the jet vector. Its maximum size is n_vars * (max_order + 1) * batch_size.
    // NOTE: n_vars must be nonzero because we successfully
    // created a Taylor jet function from sys.
    using jet_size_t = decltype(m_jet.size());
    const auto max_order = std::max(m_order_r, m_order_a);
    if (max_order >= std::numeric_limits<jet_size_t>::max()
        || (static_cast<jet_size_t>(max_order) + 1u) > std::numeric_limits<jet_size_t>::max() / n_vars
        || (static_cast<jet_size_t>(max_order) + 1u) * n_vars > std::numeric_limits<jet_size_t>::max() / batch_size) {
        throw std::overflow_error("The computation of the size of the jet of derivatives in an adaptive Taylor "
                                  "integrator resulted in an overflow condition");
    }
    m_jet.resize((static_cast<jet_size_t>(max_order) + 1u) * n_vars * batch_size);

    // Check the values of the derivatives
    // for the initial state.

    // Copy the current state to the order zero
    // of the jet of derivatives.
    auto jet_ptr = m_jet.data();
    std::copy(m_states.begin(), m_states.end(), jet_ptr);

    // Compute the jet of derivatives at max order.
    if (m_order_r > m_order_a) {
        m_jet_f_r(jet_ptr);
    } else {
        m_jet_f_a(jet_ptr);
    }

    // Check the computed derivatives, starting from order 1.
    if (std::any_of(jet_ptr + (n_vars * batch_size), jet_ptr + m_jet.size(),
                    [](const T &x) { return !detail::isfinite(x); })) {
        throw std::invalid_argument(
            "Non-finite value(s) detected in the jet of derivatives corresponding to the initial "
            "state of an adaptive batch Taylor integrator");
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
    m_rhofac_r = 1 / (detail::exp(T(1)) * detail::exp(T(1))) * detail::exp((T(-7) / T(10)) / (m_order_r - 1u));
    m_rhofac_a = 1 / (detail::exp(T(1)) * detail::exp(T(1))) * detail::exp((T(-7) / T(10)) / (m_order_a - 1u));

    // Prepare the temporary variables for use in the
    // stepping functions.
    m_max_abs_states.resize(static_cast<jet_size_t>(m_batch_size));
    m_use_abs_tol.resize(static_cast<decltype(m_use_abs_tol.size())>(m_batch_size));
    m_max_abs_diff_om1.resize(static_cast<jet_size_t>(m_batch_size));
    m_max_abs_diff_o.resize(static_cast<jet_size_t>(m_batch_size));
    m_rho_om1.resize(static_cast<jet_size_t>(m_batch_size));
    m_rho_o.resize(static_cast<jet_size_t>(m_batch_size));
    m_h.resize(static_cast<jet_size_t>(m_batch_size));
    m_pinf.resize(static_cast<jet_size_t>(m_batch_size), std::numeric_limits<T>::infinity());
    m_minf.resize(static_cast<jet_size_t>(m_batch_size), -std::numeric_limits<T>::infinity());
}

template <typename T>
taylor_adaptive_batch_impl<T>::taylor_adaptive_batch_impl(std::vector<expression> sys, std::vector<T> states,
                                                          std::vector<T> times, T rtol, T atol,
                                                          std::uint32_t batch_size, unsigned opt_level)
    : taylor_adaptive_batch_impl(p_tag{}, std::move(sys), std::move(states), std::move(times), rtol, atol, batch_size,
                                 opt_level)
{
}

template <typename T>
taylor_adaptive_batch_impl<T>::taylor_adaptive_batch_impl(std::vector<std::pair<expression, expression>> sys,
                                                          std::vector<T> states, std::vector<T> times, T rtol, T atol,
                                                          std::uint32_t batch_size, unsigned opt_level)
    : taylor_adaptive_batch_impl(p_tag{}, std::move(sys), std::move(states), std::move(times), rtol, atol, batch_size,
                                 opt_level)
{
}

template <typename T>
taylor_adaptive_batch_impl<T>::taylor_adaptive_batch_impl(const taylor_adaptive_batch_impl &other)
    // NOTE: make a manual copy of all members, apart from the function pointers.
    : m_batch_size(other.m_batch_size), m_states(other.m_states), m_times(other.m_times), m_rtol(other.m_rtol),
      m_atol(other.m_atol), m_order_r(other.m_order_r), m_order_a(other.m_order_a), m_inv_order(other.m_inv_order),
      m_rhofac_r(other.m_rhofac_r), m_rhofac_a(other.m_rhofac_a), m_llvm(other.m_llvm), m_jet(other.m_jet),
      m_dc(other.m_dc), m_max_abs_states(other.m_max_abs_states), m_use_abs_tol(other.m_use_abs_tol),
      m_max_abs_diff_om1(other.m_max_abs_diff_om1), m_max_abs_diff_o(other.m_max_abs_diff_o),
      m_rho_om1(other.m_rho_om1), m_rho_o(other.m_rho_o), m_h(other.m_h), m_pinf(other.m_pinf), m_minf(other.m_minf)
{
    // Fetch the compiled functions for computing
    // the jet of derivatives.
    m_jet_f_r = m_llvm.fetch_taylor_jet_batch<T>("jet_r");
    if (m_order_r == m_order_a) {
        m_jet_f_a = m_jet_f_r;
    } else {
        m_jet_f_a = m_llvm.fetch_taylor_jet_batch<T>("jet_a");
    }

    // Same for the functions for the state update.
    m_update_f_r = reinterpret_cast<s_update_f_t>(m_llvm.jit_lookup("estrin_r"));
    if (m_order_r == m_order_a) {
        m_update_f_a = m_update_f_r;
    } else {
        m_update_f_a = reinterpret_cast<s_update_f_t>(m_llvm.jit_lookup("estrin_a"));
    }
}

template <typename T>
taylor_adaptive_batch_impl<T>::taylor_adaptive_batch_impl(taylor_adaptive_batch_impl &&) noexcept = default;

template <typename T>
taylor_adaptive_batch_impl<T> &taylor_adaptive_batch_impl<T>::operator=(const taylor_adaptive_batch_impl &other)
{
    if (this != &other) {
        *this = taylor_adaptive_batch_impl(other);
    }

    return *this;
}

template <typename T>
taylor_adaptive_batch_impl<T> &
taylor_adaptive_batch_impl<T>::operator=(taylor_adaptive_batch_impl &&) noexcept = default;

template <typename T>
taylor_adaptive_batch_impl<T>::~taylor_adaptive_batch_impl() = default;

// Implementation detail to make a single integration timestep.
// The magnitude of the timestep is automatically deduced for each
// state vector, but it will always be not greater than
// the absolute value of the corresponding element in max_delta_ts.
// For each state vector, the propagation
// is done forward in time if max_delta_t >= 0, backwards in
// time otherwise.
//
// The function will write to res a triple for each state
// vector, containing a flag describing the outcome of the integration,
// the integration timestep that was used and the
// Taylor order that was used.
//
// NOTE: the safer adaptive timestep from
// Jorba still needs to be implemented.
template <typename T>
void taylor_adaptive_batch_impl<T>::step_impl(std::vector<std::tuple<taylor_outcome, T, std::uint32_t>> &res,
                                              const std::vector<T> &max_delta_ts)
{
    // Check preconditions.
    assert(std::none_of(max_delta_ts.begin(), max_delta_ts.end(), [](const auto &x) { return detail::isnan(x); }));
    assert(max_delta_ts.size() == m_batch_size);

    // Cache locally the batch size.
    const auto batch_size = m_batch_size;

    // Prepare res.
    res.resize(batch_size);
    std::fill(res.begin(), res.end(), std::tuple{taylor_outcome::success, T(0), std::uint32_t(0)});

    // Cache the number of variables in the system.
    assert(m_states.size() % batch_size == 0u);
    const auto nvars = static_cast<std::uint32_t>(m_states.size() / batch_size);

    // Compute the norm infinity of each state vector.
    assert(m_max_abs_states.size() == batch_size);
    std::fill(m_max_abs_states.begin(), m_max_abs_states.end(), T(0));
    for (std::uint32_t i = 0; i < nvars; ++i) {
        for (std::uint32_t batch_idx = 0; batch_idx < batch_size; ++batch_idx) {
            const auto s_idx = i * batch_size + batch_idx;

            if (detail::isfinite(m_states[s_idx])) {
                m_max_abs_states[batch_idx] = std::max(m_max_abs_states[batch_idx], detail::abs(m_states[s_idx]));
            } else {
                // Mark the current state vector as non-finite in res.
                // NOTE: the timestep and order have already
                // been set to zero via the fill() above.
                std::get<0>(res[batch_idx]) = taylor_outcome::err_nf_state;
            }
        }
    }

    // Compute the Taylor order for this timestep.
    // For each state vector, we determine the Taylor
    // order based on the norm infinity, and we take the
    // maximum.
    // NOTE: this means that we might end up using a higher
    // order than necessary in some elements of the batch.
    assert(m_use_abs_tol.size() == batch_size);
    std::uint32_t max_order = 0;
    for (std::uint32_t batch_idx = 0; batch_idx < batch_size; ++batch_idx) {
        if (std::get<0>(res[batch_idx]) != taylor_outcome::success) {
            // If the current state vector is not finite, skip it
            // for the purpose of determining the max order.
            continue;
        }

        const auto use_abs_tol = m_rtol * m_max_abs_states[batch_idx] <= m_atol;
        const auto cur_order = use_abs_tol ? m_order_a : m_order_r;
        max_order = std::max(max_order, cur_order);

        // Record whether we are using absolute or relative
        // tolerance for this element of the batch.
        m_use_abs_tol[batch_idx] = use_abs_tol;
    }

    if (max_order == 0u) {
        // If max_order is still zero, it means that all state vectors
        // contain non-finite values. Exit.
        return;
    }

    assert(max_order >= 2u);

    // Copy the current state to the order zero
    // of the jet of derivatives.
    auto jet_ptr = m_jet.data();
    std::copy(m_states.begin(), m_states.end(), jet_ptr);

    // Compute the jet of derivatives.
    // NOTE: this will be computed to the max order.
    if (max_order == m_order_a) {
        m_jet_f_a(jet_ptr);
    } else {
        m_jet_f_r(jet_ptr);
    }

    // Now we compute an estimation of the radius of convergence of the Taylor
    // series at orders 'order' and 'order - 1'. We start by computing
    // the norm infinity of the derivatives at orders 'order - 1' and
    // 'order'.
    assert(m_max_abs_diff_om1.size() == batch_size);
    assert(m_max_abs_diff_o.size() == batch_size);
    std::fill(m_max_abs_diff_om1.begin(), m_max_abs_diff_om1.end(), T(0));
    std::fill(m_max_abs_diff_o.begin(), m_max_abs_diff_o.end(), T(0));
    for (std::uint32_t i = 0; i < nvars; ++i) {
        for (std::uint32_t batch_idx = 0; batch_idx < batch_size; ++batch_idx) {
            if (std::get<0>(res[batch_idx]) != taylor_outcome::success) {
                // If the current state is not finite or resulted in non-finite
                // derivatives, skip it.
                continue;
            }

            // Establish if we are using absolute or relative
            // tolerance for this state vector.
            const auto use_abs_tol = m_use_abs_tol[batch_idx];

            // Determine the order for the current state vector.
            const auto cur_order = use_abs_tol ? m_order_a : m_order_r;

            // Load the values of the derivatives.
            const auto diff_om1 = jet_ptr[(cur_order - 1u) * nvars * batch_size + i * batch_size + batch_idx];
            const auto diff_o = jet_ptr[cur_order * nvars * batch_size + i * batch_size + batch_idx];

            if (!detail::isfinite(diff_om1) || !detail::isfinite(diff_o)) {
                // If the current state resulted in non-finite derivatives,
                // mark it and skip it.
                std::get<0>(res[batch_idx]) = taylor_outcome::err_nf_derivative;

                continue;
            }

            // Update the max abs.
            m_max_abs_diff_om1[batch_idx] = std::max(m_max_abs_diff_om1[batch_idx], detail::abs(diff_om1));
            m_max_abs_diff_o[batch_idx] = std::max(m_max_abs_diff_o[batch_idx], detail::abs(diff_o));
        }
    }

    // Estimate rho at orders 'order - 1' and 'order',
    // and compute the integration timestep.
    assert(m_rho_om1.size() == batch_size);
    assert(m_rho_o.size() == batch_size);
    assert(m_h.size() == batch_size);
    for (std::uint32_t batch_idx = 0; batch_idx < batch_size; ++batch_idx) {
        if (std::get<0>(res[batch_idx]) != taylor_outcome::success) {
            // If the current state is non finite or it resulted
            // in non-finite derivatives, set the timestep to
            // zero and skip it.
            m_h[batch_idx] = 0;

            continue;
        }

        // Establish if we are using absolute or relative
        // tolerance for this state vector.
        const auto use_abs_tol = m_use_abs_tol[batch_idx];

        // Determine the order for the current state vector.
        const auto cur_order = use_abs_tol ? m_order_a : m_order_r;

        // Compute the rhos.
        const auto rho_om1 = use_abs_tol ? detail::pow(1 / m_max_abs_diff_om1[batch_idx], m_inv_order[cur_order - 1u])
                                         : detail::pow(m_max_abs_states[batch_idx] / m_max_abs_diff_om1[batch_idx],
                                                       m_inv_order[cur_order - 1u]);
        const auto rho_o = use_abs_tol ? detail::pow(1 / m_max_abs_diff_o[batch_idx], m_inv_order[cur_order])
                                       : detail::pow(m_max_abs_states[batch_idx] / m_max_abs_diff_o[batch_idx],
                                                     m_inv_order[cur_order]);

        if (detail::isnan(rho_om1) || detail::isnan(rho_o)) {
            // Mark the presence of NaN rho in res.
            std::get<0>(res[batch_idx]) = taylor_outcome::err_nan_rho;

            // Set the timestep to zero.
            m_h[batch_idx] = 0;
        } else {
            // Compute the minimum.
            const auto rho_m = std::min(rho_o, rho_om1);

            // Compute the timestep.
            auto h = rho_m * (use_abs_tol ? m_rhofac_a : m_rhofac_r);

            // Make sure h does not exceed abs(max_delta_t).
            const auto abs_delta_t = detail::abs(max_delta_ts[batch_idx]);
            if (h > abs_delta_t) {
                h = abs_delta_t;
                std::get<0>(res[batch_idx]) = taylor_outcome::time_limit;
            }

            if (max_delta_ts[batch_idx] < T(0)) {
                // When propagating backwards, invert the sign of the timestep.
                h = -h;
            }

            // Store the integration timestep
            // for the current state vector.
            m_h[batch_idx] = h;
        }
    }

    // Update the state.
    // NOTE: this will update the state using the max order.
    if (max_order == m_order_a) {
        m_update_f_a(m_states.data(), jet_ptr, m_h.data());
    } else {
        m_update_f_r(m_states.data(), jet_ptr, m_h.data());
    }

    // Update the times, store the timesteps and order in res.
    for (std::uint32_t batch_idx = 0; batch_idx < batch_size; ++batch_idx) {
        if (std::get<0>(res[batch_idx]) != taylor_outcome::success) {
            // If some failure mode was detected, don't update
            // the times or the return values.
            continue;
        }

        m_times[batch_idx] += m_h[batch_idx];
        std::get<1>(res[batch_idx]) = m_h[batch_idx];
        std::get<2>(res[batch_idx]) = m_use_abs_tol[batch_idx] ? m_order_a : m_order_r;
    }
}

template <typename T>
void taylor_adaptive_batch_impl<T>::step(std::vector<std::tuple<taylor_outcome, T, std::uint32_t>> &res)
{
    return step_impl(res, m_pinf);
}

template <typename T>
void taylor_adaptive_batch_impl<T>::step_backward(std::vector<std::tuple<taylor_outcome, T, std::uint32_t>> &res)
{
    return step_impl(res, m_minf);
}

template <typename T>
void taylor_adaptive_batch_impl<T>::set_times(const std::vector<T> &t)
{
    if (&t == &m_times) {
        // Check that t and m_times are not the same object,
        // otherwise std::copy() cannot be used.
        return;
    }

    if (t.size() != m_times.size()) {
        throw std::invalid_argument("Inconsistent sizes when setting the times in a batch Taylor integrator: the new "
                                    "times vector has a size of "
                                    + std::to_string(t.size()) + ", while the existing times vector has a size of "
                                    + std::to_string(m_times.size()));
    }

    if (std::any_of(t.begin(), t.end(), [](const auto &x) { return !detail::isfinite(x); })) {
        throw std::invalid_argument(
            "A non-finite time value was detected while setting the times in a batch Taylor integrator");
    }

    // Do the copy.
    std::copy(t.begin(), t.end(), m_times.begin());
}

template <typename T>
void taylor_adaptive_batch_impl<T>::set_states(const std::vector<T> &states)
{
    if (&states == &m_states) {
        // Check that states and m_states are not the same object,
        // otherwise std::copy() cannot be used.
        return;
    }

    if (states.size() != m_states.size()) {
        throw std::invalid_argument("The states vector passed to the set_states() function of an adaptive batch Taylor "
                                    "integrator has a size of "
                                    + std::to_string(states.size())
                                    + ", which is inconsistent with the size of the current states vector ("
                                    + std::to_string(m_states.size()) + ")");
    }

    if (std::any_of(states.begin(), states.end(), [](const T &x) { return !detail::isfinite(x); })) {
        throw std::invalid_argument("A non-finite states vector was passed to the set_states() function of an adaptive "
                                    "batch Taylor integrator");
    }

    // Do the copy.
    std::copy(states.begin(), states.end(), m_states.begin());
}

template <typename T>
std::string taylor_adaptive_batch_impl<T>::get_ir() const
{
    return m_llvm.dump_ir();
}

template <typename T>
const std::vector<expression> &taylor_adaptive_batch_impl<T>::get_decomposition() const
{
    return m_dc;
}

// Explicit instantiation of the batch implementation classes.
template class taylor_adaptive_batch_impl<double>;
template class taylor_adaptive_batch_impl<long double>;

#if defined(HEYOKA_HAVE_REAL128)

template class taylor_adaptive_batch_impl<mppp::real128>;

#endif

} // namespace detail

} // namespace heyoka
