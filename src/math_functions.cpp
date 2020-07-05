// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/string_conv.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/function.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math_functions.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>
#include <heyoka/variable.hpp>

namespace heyoka
{

namespace detail
{

namespace
{

// NOTE: this code is very similar to the function invocation
// code for builtins in function.cpp. Perhaps in the future
// we can avoid repetition.
template <typename T>
llvm::Value *taylor_init_sin(llvm_state &s, const function &f, llvm::Value *arr)
{
    if (f.args().size() != 1u) {
        throw std::invalid_argument("Inconsistent number of arguments in the Taylor initialization phase for "
                                    "the sine (1 argument was expected, but "
                                    + std::to_string(f.args().size()) + " arguments were provided");
    }

    // Lookup the intrinsic ID.
    const auto intrinsic_ID = llvm::Function::lookupIntrinsicID("llvm.sin");
    if (intrinsic_ID == 0) {
        throw std::invalid_argument("Cannot fetch the ID of the intrinsic 'llvm.sin'");
    }

    // Fetch the function.
    const std::vector<llvm::Type *> arg_types(1, to_llvm_type<T>(s.context()));
    auto callee_f = llvm::Intrinsic::getDeclaration(&s.module(), intrinsic_ID, arg_types);
    if (!callee_f) {
        throw std::invalid_argument("Error getting the declaration of the intrinsic 'llvm.sin'");
    }
    if (!callee_f->empty()) {
        // It does not make sense to have a definition of a builtin.
        throw std::invalid_argument("The intrinsic 'llvm.sin' must be an empty function");
    }

    // Create the function argument. The codegen for the argument
    // comes from taylor_init.
    std::vector<llvm::Value *> args_v(1, invoke_taylor_init<T>(s, f.args()[0], arr));
    assert(args_v[0] != nullptr);

    // Create the function call.
    auto r = s.builder().CreateCall(callee_f, args_v, "taylor_init_sin_tmp");
    assert(r != nullptr);
    r->setTailCall(true);

    return r;
}

// Derivative of sin(number).
template <typename T>
llvm::Function *taylor_diff_sin_impl(llvm_state &s, const number &, std::uint32_t, const std::string &name,
                                     std::uint32_t, const std::unordered_map<std::uint32_t, number> &)
{
    auto &builder = s.builder();

    auto [f, diff_ptr, order] = taylor_diff_common<T>(s, name);

    // The derivative of a constant is always zero.
    builder.CreateRet(invoke_codegen<T>(s, number(0.)));

    s.verify_function(name);

    return f;
}

// Derivative of sin(variable).
template <typename T>
llvm::Function *taylor_diff_sin_impl(llvm_state &s, const variable &var, std::uint32_t idx, const std::string &name,
                                     std::uint32_t n_uvars, const std::unordered_map<std::uint32_t, number> &)
{
    auto &builder = s.builder();

    auto [f, diff_ptr, order] = taylor_diff_common<T>(s, name);

    // Accumulator for the result.
    auto ret_acc = builder.CreateAlloca(to_llvm_type<T>(s.context()), 0, "ret_acc");
    builder.CreateStore(invoke_codegen<T>(s, number(0.)), ret_acc);

    // Initial value for the for-loop. We will be operating
    // in the range [1, order] (i.e., order inclusive).
    auto start_val = builder.getInt32(1);

    // Make the new basic block for the loop header,
    // inserting after current block.
    auto *preheader_bb = builder.GetInsertBlock();
    auto *loop_bb = llvm::BasicBlock::Create(s.context(), "loop", f);

    // Insert an explicit fall through from the current block to the loop_bb.
    builder.CreateBr(loop_bb);

    // Start insertion in loop_bb.
    builder.SetInsertPoint(loop_bb);

    // Start the PHI node with an entry for Start.
    auto *j_var = builder.CreatePHI(builder.getInt32Ty(), 2, "j");
    j_var->addIncoming(start_val, preheader_bb);

    // Loop body.
    // Compute the indices for accessing the derivatives in this loop iteration.
    // The indices are:
    // - (order - j_var) * n_uvars + idx + 1,
    // - j_var * n_uvars + u_idx.
    // NOTE: the +1 is because we are accessing the cosine
    // of the u var, which is conventionally placed
    // right after the sine in the decomposition.
    const auto u_idx = uname_to_index(var.name());
    auto arr_idx0 = builder.CreateAdd(builder.CreateMul(builder.CreateSub(order, j_var), builder.getInt32(n_uvars)),
                                      builder.getInt32(idx + 1u));
    auto arr_idx1 = builder.CreateAdd(builder.CreateMul(j_var, builder.getInt32(n_uvars)), builder.getInt32(u_idx));
    // Convert into pointers.
    auto arr_ptr0 = builder.CreateInBoundsGEP(diff_ptr, arr_idx0, "diff_ptr0");
    auto arr_ptr1 = builder.CreateInBoundsGEP(diff_ptr, arr_idx1, "diff_ptr1");
    // Load the values.
    auto v0 = builder.CreateLoad(arr_ptr0, "diff_load0");
    auto v1 = builder.CreateLoad(arr_ptr1, "diff_load1");
    // Update ret_acc: ret_acc = ret_acc + j*v0*v1.
    builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(ret_acc),
                                           builder.CreateFMul(builder.CreateUIToFP(j_var, to_llvm_type<T>(s.context())),
                                                              builder.CreateFMul(v0, v1))),
                        ret_acc);

    // Compute the next value of the iteration.
    // NOTE: addition works regardless of integral signedness.
    auto *next_j_var = builder.CreateAdd(j_var, builder.getInt32(1), "next_j");

    // Compute the end condition.
    // NOTE: we use the unsigned less-than-or-equal predicate.
    auto *end_cond = builder.CreateICmp(llvm::CmpInst::ICMP_ULE, next_j_var, order, "loopcond");

    // Create the "after loop" block and insert it.
    auto *loop_end_bb = builder.GetInsertBlock();
    auto *after_bb = llvm::BasicBlock::Create(s.context(), "afterloop", f);

    // Insert the conditional branch into the end of loop_end_bb.
    builder.CreateCondBr(end_cond, loop_bb, after_bb);

    // Any new code will be inserted in after_bb.
    builder.SetInsertPoint(after_bb);

    // Add a new entry to the PHI node for the backedge.
    j_var->addIncoming(next_j_var, loop_end_bb);

    // Compute and return the result: ret_acc / order
    builder.CreateRet(
        builder.CreateFDiv(builder.CreateLoad(ret_acc), builder.CreateUIToFP(order, to_llvm_type<T>(s.context()))));

    s.verify_function(name);

    return f;
}

// All the other cases.
template <typename T, typename U>
llvm::Function *taylor_diff_sin_impl(llvm_state &, const U &, std::uint32_t, const std::string &, std::uint32_t,
                                     const std::unordered_map<std::uint32_t, number> &)
{
    throw std::invalid_argument(
        "An invalid argument type was encountered while trying to build the Taylor derivative of a sine");
}

template <typename T>
llvm::Function *taylor_diff_sin(llvm_state &s, const function &func, std::uint32_t idx, const std::string &name,
                                std::uint32_t n_uvars, const std::unordered_map<std::uint32_t, number> &cd_uvars)
{
    if (func.args().size() != 1u) {
        throw std::invalid_argument("Inconsistent number of arguments in the Taylor derivative for "
                                    "the sine (1 argument was expected, but "
                                    + std::to_string(func.args().size()) + " arguments were provided");
    }

    return std::visit([&s, idx, &name, n_uvars, &cd_uvars](
                          const auto &v) { return taylor_diff_sin_impl<T>(s, v, idx, name, n_uvars, cd_uvars); },
                      func.args()[0].value());
}

} // namespace

} // namespace detail

expression sin(expression e)
{
    std::vector<expression> args;
    args.emplace_back(std::move(e));

    function fc{std::move(args)};
    fc.dbl_name() = "llvm.sin";
    fc.ldbl_name() = "llvm.sin";
    fc.display_name() = "sin";
    fc.ty() = function::type::builtin;
    fc.diff_f() = [](const std::vector<expression> &args, const std::string &s) {
        if (args.size() != 1u) {
            throw std::invalid_argument(
                "Inconsistent number of arguments when taking the derivative of the sine (1 argument was expected, but "
                + std::to_string(args.size()) + " arguments were provided");
        }

        return cos(args[0]) * diff(args[0], s);
    };
    fc.eval_dbl_f() = [](const std::vector<expression> &args, const std::unordered_map<std::string, double> &map) {
        if (args.size() != 1u) {
            throw std::invalid_argument(
                "Inconsistent number of arguments when evaluating the sine (1 argument was expected, but "
                + std::to_string(args.size()) + " arguments were provided");
        }

        return std::sin(eval_dbl(args[0], map));
    };
    fc.eval_batch_dbl_f() = [](std::vector<double> &out, const std::vector<expression> &args,
                               const std::unordered_map<std::string, std::vector<double>> &map) {
        if (args.size() != 1u) {
            throw std::invalid_argument(
                "Inconsistent number of arguments when evaluating the sine in batches (1 argument was expected, but "
                + std::to_string(args.size()) + " arguments were provided");
        }
        eval_batch_dbl(out, args[0], map);
        for (auto &el : out) {
            el = std::sin(el);
        }
    };
    fc.eval_num_dbl_f() = [](const std::vector<double> &args) {
        if (args.size() != 1u) {
            throw std::invalid_argument("Inconsistent number of arguments when computing the numerical value of the "
                                        "sine over doubles (1 argument was expected, but "
                                        + std::to_string(args.size()) + " arguments were provided");
        }

        return std::sin(args[0]);
    };
    fc.deval_num_dbl_f() = [](const std::vector<double> &args, unsigned i) {
        if (args.size() != 1u || i != 0u) {
            throw std::invalid_argument("Inconsistent number of arguments or derivative requested when computing "
                                        "the derivative of std::sin");
        }

        return std::cos(args[0]);
    };
    // NOTE: for sine/cosine we need a non-default decomposition because
    // we always need both sine *and* cosine in the decomposition
    // in order to compute the derivatives.
    fc.taylor_decompose_f() = [](function &&f, std::vector<expression> &u_vars_defs) {
        if (f.args().size() != 1u) {
            throw std::invalid_argument("Inconsistent number of arguments when computing the Taylor decomposition of "
                                        "the sine (1 argument was expected, but "
                                        + std::to_string(f.args().size()) + " arguments were provided");
        }

        // Decompose the argument.
        auto &arg = f.args()[0];
        if (const auto dres = taylor_decompose_in_place(std::move(arg), u_vars_defs)) {
            arg = expression{variable{"u_" + detail::li_to_string(dres)}};
        }

        // Save a copy of the decomposed argument.
        auto f_arg = arg;

        // Append the sine decomposition.
        u_vars_defs.emplace_back(std::move(f));

        // Compute the return value (pointing to the
        // decomposed sine).
        const auto retval = u_vars_defs.size() - 1u;

        // Append the cosine decomposition.
        u_vars_defs.emplace_back(cos(std::move(f_arg)));

        return retval;
    };
    fc.taylor_init_dbl_f() = detail::taylor_init_sin<double>;
    fc.taylor_init_ldbl_f() = detail::taylor_init_sin<long double>;
    fc.taylor_diff_dbl_f() = detail::taylor_diff_sin<double>;
    fc.taylor_diff_ldbl_f() = detail::taylor_diff_sin<long double>;

    return expression{std::move(fc)};
}

namespace detail
{

namespace
{

template <typename T>
llvm::Value *taylor_init_cos(llvm_state &s, const function &f, llvm::Value *arr)
{
    if (f.args().size() != 1u) {
        throw std::invalid_argument("Inconsistent number of arguments in the Taylor initialization phase for "
                                    "the cosine (1 argument was expected, but "
                                    + std::to_string(f.args().size()) + " arguments were provided");
    }

    // Lookup the intrinsic ID.
    const auto intrinsic_ID = llvm::Function::lookupIntrinsicID("llvm.cos");
    if (intrinsic_ID == 0) {
        throw std::invalid_argument("Cannot fetch the ID of the intrinsic 'llvm.cos'");
    }

    // Fetch the function.
    const std::vector<llvm::Type *> arg_types(1, to_llvm_type<T>(s.context()));
    auto callee_f = llvm::Intrinsic::getDeclaration(&s.module(), intrinsic_ID, arg_types);
    if (!callee_f) {
        throw std::invalid_argument("Error getting the declaration of the intrinsic 'llvm.cos'");
    }
    if (!callee_f->empty()) {
        // It does not make sense to have a definition of a builtin.
        throw std::invalid_argument("The intrinsic 'llvm.cos' must be an empty function");
    }

    // Create the function argument. The codegen for the argument
    // comes from taylor_init.
    std::vector<llvm::Value *> args_v(1, invoke_taylor_init<T>(s, f.args()[0], arr));
    assert(args_v[0] != nullptr);

    // Create the function call.
    auto r = s.builder().CreateCall(callee_f, args_v, "taylor_init_cos_tmp");
    assert(r != nullptr);
    r->setTailCall(true);

    return r;
}

// Derivative of cos(number).
template <typename T>
llvm::Function *taylor_diff_cos_impl(llvm_state &s, const number &, std::uint32_t, const std::string &name,
                                     std::uint32_t, const std::unordered_map<std::uint32_t, number> &)
{
    auto &builder = s.builder();

    auto [f, diff_ptr, order] = taylor_diff_common<T>(s, name);

    // The derivative of a constant is always zero.
    builder.CreateRet(invoke_codegen<T>(s, number(0.)));

    s.verify_function(name);

    return f;
}

// Derivative of cos(variable).
template <typename T>
llvm::Function *taylor_diff_cos_impl(llvm_state &s, const variable &var, std::uint32_t idx, const std::string &name,
                                     std::uint32_t n_uvars, const std::unordered_map<std::uint32_t, number> &)
{
    auto &builder = s.builder();

    auto [f, diff_ptr, order] = taylor_diff_common<T>(s, name);

    // Accumulator for the result.
    auto ret_acc = builder.CreateAlloca(to_llvm_type<T>(s.context()), 0, "ret_acc");
    builder.CreateStore(invoke_codegen<T>(s, number(0.)), ret_acc);

    // Initial value for the for-loop. We will be operating
    // in the range [1, order] (i.e., order inclusive).
    auto start_val = builder.getInt32(1);

    // Make the new basic block for the loop header,
    // inserting after current block.
    auto *preheader_bb = builder.GetInsertBlock();
    auto *loop_bb = llvm::BasicBlock::Create(s.context(), "loop", f);

    // Insert an explicit fall through from the current block to the loop_bb.
    builder.CreateBr(loop_bb);

    // Start insertion in loop_bb.
    builder.SetInsertPoint(loop_bb);

    // Start the PHI node with an entry for Start.
    auto *j_var = builder.CreatePHI(builder.getInt32Ty(), 2, "j");
    j_var->addIncoming(start_val, preheader_bb);

    // Loop body.
    // Compute the indices for accessing the derivatives in this loop iteration.
    // The indices are:
    // - (order - j_var) * n_uvars + idx - 1,
    // - j_var * n_uvars + u_idx.
    // NOTE: the -1 is because we are accessing the sine
    // of the u var, which is conventionally placed
    // right before the cosine in the decomposition.
    const auto u_idx = uname_to_index(var.name());
    auto arr_idx0 = builder.CreateAdd(builder.CreateMul(builder.CreateSub(order, j_var), builder.getInt32(n_uvars)),
                                      builder.getInt32(idx - 1u));
    auto arr_idx1 = builder.CreateAdd(builder.CreateMul(j_var, builder.getInt32(n_uvars)), builder.getInt32(u_idx));
    // Convert into pointers.
    auto arr_ptr0 = builder.CreateInBoundsGEP(diff_ptr, arr_idx0, "diff_ptr0");
    auto arr_ptr1 = builder.CreateInBoundsGEP(diff_ptr, arr_idx1, "diff_ptr1");
    // Load the values.
    auto v0 = builder.CreateLoad(arr_ptr0, "diff_load0");
    auto v1 = builder.CreateLoad(arr_ptr1, "diff_load1");
    // Update ret_acc: ret_acc = ret_acc + j*v0*v1.
    builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(ret_acc),
                                           builder.CreateFMul(builder.CreateUIToFP(j_var, to_llvm_type<T>(s.context())),
                                                              builder.CreateFMul(v0, v1))),
                        ret_acc);

    // Compute the next value of the iteration.
    // NOTE: addition works regardless of integral signedness.
    auto *next_j_var = builder.CreateAdd(j_var, builder.getInt32(1), "next_j");

    // Compute the end condition.
    // NOTE: we use the unsigned less-than-or-equal predicate.
    auto *end_cond = builder.CreateICmp(llvm::CmpInst::ICMP_ULE, next_j_var, order, "loopcond");

    // Create the "after loop" block and insert it.
    auto *loop_end_bb = builder.GetInsertBlock();
    auto *after_bb = llvm::BasicBlock::Create(s.context(), "afterloop", f);

    // Insert the conditional branch into the end of loop_end_bb.
    builder.CreateCondBr(end_cond, loop_bb, after_bb);

    // Any new code will be inserted in after_bb.
    builder.SetInsertPoint(after_bb);

    // Add a new entry to the PHI node for the backedge.
    j_var->addIncoming(next_j_var, loop_end_bb);

    // Compute and return the result: -ret_acc / order
    builder.CreateRet(builder.CreateFNeg(
        builder.CreateFDiv(builder.CreateLoad(ret_acc), builder.CreateUIToFP(order, to_llvm_type<T>(s.context())))));

    s.verify_function(name);

    return f;
}

// All the other cases.
template <typename T, typename U>
llvm::Function *taylor_diff_cos_impl(llvm_state &, const U &, std::uint32_t, const std::string &, std::uint32_t,
                                     const std::unordered_map<std::uint32_t, number> &)
{
    throw std::invalid_argument(
        "An invalid argument type was encountered while trying to build the Taylor derivative of a cosine");
}

template <typename T>
llvm::Function *taylor_diff_cos(llvm_state &s, const function &func, std::uint32_t idx, const std::string &name,
                                std::uint32_t n_uvars, const std::unordered_map<std::uint32_t, number> &cd_uvars)
{
    if (func.args().size() != 1u) {
        throw std::invalid_argument("Inconsistent number of arguments in the Taylor derivative for "
                                    "the cosine (1 argument was expected, but "
                                    + std::to_string(func.args().size()) + " arguments were provided");
    }

    return std::visit([&s, idx, &name, n_uvars, &cd_uvars](
                          const auto &v) { return taylor_diff_cos_impl<T>(s, v, idx, name, n_uvars, cd_uvars); },
                      func.args()[0].value());
}

} // namespace

} // namespace detail

expression cos(expression e)
{
    std::vector<expression> args;
    args.emplace_back(std::move(e));

    function fc{std::move(args)};
    fc.dbl_name() = "llvm.cos";
    fc.ldbl_name() = "llvm.cos";
    fc.display_name() = "cos";
    fc.ty() = function::type::builtin;
    fc.diff_f() = [](const std::vector<expression> &args, const std::string &s) {
        if (args.size() != 1u) {
            throw std::invalid_argument("Inconsistent number of arguments when taking the derivative of the cosine (1 "
                                        "argument was expected, but "
                                        + std::to_string(args.size()) + " arguments were provided");
        }

        return -sin(args[0]) * diff(args[0], s);
    };
    fc.eval_dbl_f() = [](const std::vector<expression> &args, const std::unordered_map<std::string, double> &map) {
        if (args.size() != 1u) {
            throw std::invalid_argument("Inconsistent number of arguments when evaluating the cosine from doubles (1 "
                                        "argument was expected, but "
                                        + std::to_string(args.size()) + " arguments were provided");
        }

        return std::cos(eval_dbl(args[0], map));
    };
    fc.eval_batch_dbl_f() = [](std::vector<double> &out, const std::vector<expression> &args,
                               const std::unordered_map<std::string, std::vector<double>> &map) {
        if (args.size() != 1u) {
            throw std::invalid_argument("Inconsistent number of arguments when evaluating the cosine in batches of "
                                        "doubles (1 argument was expected, but "
                                        + std::to_string(args.size()) + " arguments were provided");
        }
        eval_batch_dbl(out, args[0], map);
        for (auto &el : out) {
            el = std::cos(el);
        }
    };
    fc.eval_num_dbl_f() = [](const std::vector<double> &args) {
        if (args.size() != 1u) {
            throw std::invalid_argument("Inconsistent number of arguments when computing the numerical value of the "
                                        "cosine over doubles (1 argument was expected, but "
                                        + std::to_string(args.size()) + " arguments were provided");
        }

        return std::cos(args[0]);
    };
    fc.deval_num_dbl_f() = [](const std::vector<double> &args, unsigned i) {
        if (args.size() != 1u || i != 0u) {
            throw std::invalid_argument(
                "Inconsistent number of arguments or derivative requested when computing the derivative of std::cos");
        }

        return -std::sin(args[0]);
    };
    fc.taylor_decompose_f() = [](function &&f, std::vector<expression> &u_vars_defs) {
        if (f.args().size() != 1u) {
            throw std::invalid_argument("Inconsistent number of arguments when computing the Taylor decomposition of "
                                        "the cosine (1 argument was expected, but "
                                        + std::to_string(f.args().size()) + " arguments were provided");
        }

        // Decompose the argument.
        auto &arg = f.args()[0];
        if (const auto dres = taylor_decompose_in_place(std::move(arg), u_vars_defs)) {
            arg = expression{variable{"u_" + detail::li_to_string(dres)}};
        }

        // Append the sine decomposition.
        u_vars_defs.emplace_back(sin(arg));

        // Append the cosine decomposition.
        u_vars_defs.emplace_back(std::move(f));

        return u_vars_defs.size() - 1u;
    };
    fc.taylor_init_dbl_f() = detail::taylor_init_cos<double>;
    fc.taylor_init_ldbl_f() = detail::taylor_init_cos<long double>;
    fc.taylor_diff_dbl_f() = detail::taylor_diff_cos<double>;
    fc.taylor_diff_ldbl_f() = detail::taylor_diff_cos<long double>;

    return expression{std::move(fc)};
}

expression log(expression e)
{
    std::vector<expression> args;
    args.emplace_back(std::move(e));

    function fc{std::move(args)};
    fc.dbl_name() = "llvm.log";
    fc.ldbl_name() = "llvm.log";
    fc.display_name() = "log";
    fc.ty() = function::type::builtin;
    fc.diff_f() = [](const std::vector<expression> &args, const std::string &s) {
        if (args.size() != 1u) {
            throw std::invalid_argument(
                "Inconsistent number of arguments when taking the derivative of the logarithm (1 "
                "arguments were expected, but "
                + std::to_string(args.size()) + " arguments were provided");
        }

        return expression{number(1.)} / args[0] * diff(args[0], s);
    };

    fc.eval_dbl_f() = [](const std::vector<expression> &args, const std::unordered_map<std::string, double> &map) {
        if (args.size() != 1u) {
            throw std::invalid_argument(
                "Inconsistent number of arguments when evaluating the logarithm from doubles (1 "
                "argument was expected, but "
                + std::to_string(args.size()) + " arguments were provided");
        }

        return std::log(eval_dbl(args[0], map));
    };
    fc.eval_batch_dbl_f() = [](std::vector<double> &out, const std::vector<expression> &args,
                               const std::unordered_map<std::string, std::vector<double>> &map) {
        if (args.size() != 1u) {
            throw std::invalid_argument("Inconsistent number of arguments when evaluating the logarithm in batches of "
                                        "doubles (1 argument was expected, but "
                                        + std::to_string(args.size()) + " arguments were provided");
        }
        eval_batch_dbl(out, args[0], map);
        for (auto &el : out) {
            el = std::log(el);
        }
    };
    fc.eval_num_dbl_f() = [](const std::vector<double> &args) {
        if (args.size() != 1u) {
            throw std::invalid_argument("Inconsistent number of arguments when computing the numerical value of the "
                                        "logarithm over doubles (1 argument was expected, but "
                                        + std::to_string(args.size()) + " arguments were provided");
        }

        return std::log(args[0]);
    };
    fc.deval_num_dbl_f() = [](const std::vector<double> &args, unsigned i) {
        if (args.size() != 1u || i != 0u) {
            throw std::invalid_argument(
                "Inconsistent number of arguments or derivative requested when computing the derivative of std::log");
        }

        return 1. / args[0];
    };

    return expression{std::move(fc)};
}

expression pow(expression e1, expression e2)
{
    std::vector<expression> args;
    args.emplace_back(std::move(e1));
    args.emplace_back(std::move(e2));

    function fc{std::move(args)};
    fc.dbl_name() = "llvm.pow";
    fc.ldbl_name() = "llvm.pow";
    fc.display_name() = "pow";
    fc.ty() = function::type::builtin;
    fc.diff_f() = [](const std::vector<expression> &args, const std::string &s) {
        if (args.size() != 2u) {
            throw std::invalid_argument(
                "Inconsistent number of arguments when taking the derivative of the exponentiation (2 "
                "arguments were expected, but "
                + std::to_string(args.size()) + " arguments were provided");
        }
        return args[1] * pow(args[0], args[1] - expression{number(1.)}) * diff(args[0], s)
               + pow(args[0], args[1]) * log(args[0]) * diff(args[1], s);
    };

    fc.eval_dbl_f() = [](const std::vector<expression> &args, const std::unordered_map<std::string, double> &map) {
        if (args.size() != 2u) {
            throw std::invalid_argument(
                "Inconsistent number of arguments when evaluating the exponentiation from doubles (2 "
                "arguments were expected, but "
                + std::to_string(args.size()) + " arguments were provided");
        }
        return std::pow(eval_dbl(args[0], map), eval_dbl(args[1], map));
    };
    fc.eval_batch_dbl_f() = [](std::vector<double> &out, const std::vector<expression> &args,
                               const std::unordered_map<std::string, std::vector<double>> &map) {
        if (args.size() != 2u) {
            throw std::invalid_argument("Inconsistent number of arguments when evaluating the exponentiation in "
                                        "batches of doubles (2 arguments were expected, but "
                                        + std::to_string(args.size()) + " arguments were provided");
        }
        auto out0 = out; // is this allocation needed?
        eval_batch_dbl(out0, args[0], map);
        eval_batch_dbl(out, args[1], map);
        for (decltype(out.size()) i = 0u; i < out.size(); ++i) {
            out[i] = std::pow(out0[i], out[i]);
        }
    };
    fc.eval_num_dbl_f() = [](const std::vector<double> &args) {
        if (args.size() != 2u) {
            throw std::invalid_argument("Inconsistent number of arguments when computing the numerical value of the "
                                        "exponentiation over doubles (1 argument was expected, but "
                                        + std::to_string(args.size()) + " arguments were provided");
        }

        return std::pow(args[0], args[1]);
    };
    fc.deval_num_dbl_f() = [](const std::vector<double> &args, unsigned i) {
        if (args.size() != 2u || i > 1u) {
            throw std::invalid_argument(
                "Inconsistent number of arguments or derivative requested when computing the derivative of std::pow");
        }
        return args[1] * std::pow(args[0], args[1] - 1.) + std::log(args[0]) * std::pow(args[0], args[1]);
    };

    return expression{std::move(fc)};
}

} // namespace heyoka
