// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <cassert>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <vector>

#include <fmt/format.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/time.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>

namespace heyoka
{

namespace detail
{

time_impl::time_impl() : func_base("time", std::vector<expression>{}) {}

void time_impl::to_stream(std::ostream &os) const
{
    os << 't';
}

namespace
{

template <typename T>
llvm::Value *time_taylor_diff_impl(llvm_state &s, llvm::Value *time_ptr, std::uint32_t order, std::uint32_t batch_size)
{
    auto &builder = s.builder();

    // NOTE: no need for normalisation of the derivative,
    // as the only nonzero retvals are for orders 0 and 1
    // for which the normalised derivatives coincide with
    // the non-normalised derivatives.
    switch (order) {
        case 0u:
            return load_vector_from_memory(builder, time_ptr, batch_size);
        case 1u:
            return vector_splat(builder, codegen<T>(s, number{1.}), batch_size);
        default:
            return vector_splat(builder, codegen<T>(s, number{0.}), batch_size);
    }
}

} // namespace

llvm::Value *time_impl::taylor_diff_dbl(llvm_state &s, const std::vector<std::uint32_t> &,
                                        const std::vector<llvm::Value *> &, llvm::Value *, llvm::Value *time_ptr,
                                        std::uint32_t, std::uint32_t order, std::uint32_t,
                                        std::uint32_t batch_size) const
{
    return time_taylor_diff_impl<double>(s, time_ptr, order, batch_size);
}

llvm::Value *time_impl::taylor_diff_ldbl(llvm_state &s, const std::vector<std::uint32_t> &,
                                         const std::vector<llvm::Value *> &, llvm::Value *, llvm::Value *time_ptr,
                                         std::uint32_t, std::uint32_t order, std::uint32_t,
                                         std::uint32_t batch_size) const
{
    return time_taylor_diff_impl<long double>(s, time_ptr, order, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *time_impl::taylor_diff_f128(llvm_state &s, const std::vector<std::uint32_t> &,
                                         const std::vector<llvm::Value *> &, llvm::Value *, llvm::Value *time_ptr,
                                         std::uint32_t, std::uint32_t order, std::uint32_t,
                                         std::uint32_t batch_size) const
{
    return time_taylor_diff_impl<mppp::real128>(s, time_ptr, order, batch_size);
}

#endif

namespace
{

// NOTE: perhaps later on this can become a generic implementation
// for nullary functions, in the same mold as
// taylor_c_diff_func_unary_num_det().
template <typename T>
llvm::Function *taylor_c_diff_time_impl(llvm_state &s, std::uint32_t batch_size)
{
    auto &module = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the floating-point type.
    auto val_t = to_llvm_vector_type<T>(context, batch_size);

    // Compose the function name.
    using namespace fmt::literals;
    const auto fname = "heyoka_taylor_diff_time_{}"_format(taylor_mangle_suffix(val_t));

    // The function arguments:
    // - diff order,
    // - idx of the u variable whose diff is being computed,
    // - diff array,
    // - par ptr,
    // - time ptr.
    std::vector<llvm::Type *> fargs{
        llvm::Type::getInt32Ty(context), llvm::Type::getInt32Ty(context), llvm::PointerType::getUnqual(val_t),
        llvm::PointerType::getUnqual(to_llvm_type<T>(context)), llvm::PointerType::getUnqual(to_llvm_type<T>(context))};

    // Try to see if we already created the function.
    auto f = module.getFunction(fname);

    if (f == nullptr) {
        // The function was not created before, do it now.

        // Fetch the current insertion block.
        auto orig_bb = builder.GetInsertBlock();

        // The return type is val_t.
        auto *ft = llvm::FunctionType::get(val_t, fargs, false);
        // Create the function
        f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fname, &module);
        assert(f != nullptr);

        // Fetch the necessary function arguments.
        auto ord = f->args().begin();
        auto t_ptr = f->args().begin() + 4;

        // Create a new basic block to start insertion into.
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", f));

        // Create the return value.
        auto retval = builder.CreateAlloca(val_t);

        // NOTE: no need for normalisation of the derivative,
        // as the only nonzero retvals are for orders 0 and 1
        // for which the normalised derivatives coincide with
        // the non-normalised derivatives.
        llvm_if_then_else(
            s, builder.CreateICmpEQ(ord, builder.getInt32(0)),
            [&]() {
                // If the order is zero, return the time itself.
                builder.CreateStore(load_vector_from_memory(builder, t_ptr, batch_size), retval);
            },
            [&]() {
                llvm_if_then_else(
                    s, builder.CreateICmpEQ(ord, builder.getInt32(1)),
                    [&]() {
                        // If the order is one, return 1.
                        builder.CreateStore(vector_splat(builder, codegen<T>(s, number{1.}), batch_size), retval);
                    },
                    [&]() {
                        // If order > 1, return zero.
                        builder.CreateStore(vector_splat(builder, codegen<T>(s, number{0.}), batch_size), retval);
                    });
            });

        // Return the result.
        builder.CreateRet(builder.CreateLoad(retval));

        // Verify.
        s.verify_function(f);

        // Restore the original insertion block.
        builder.SetInsertPoint(orig_bb);
    } else {
        // The function was created before. Check if the signatures match.
        // NOTE: there could be a mismatch if the derivative function was created
        // and then optimised - optimisation might remove arguments which are compile-time
        // constants.
        if (!compare_function_signature(f, val_t, fargs)) {
            throw std::invalid_argument(
                "Inconsistent function signature for the Taylor derivative of time() in compact mode detected");
        }
    }

    return f;
}

} // namespace

llvm::Function *time_impl::taylor_c_diff_func_dbl(llvm_state &s, std::uint32_t, std::uint32_t batch_size) const
{
    return taylor_c_diff_time_impl<double>(s, batch_size);
}

llvm::Function *time_impl::taylor_c_diff_func_ldbl(llvm_state &s, std::uint32_t, std::uint32_t batch_size) const
{
    return taylor_c_diff_time_impl<long double>(s, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Function *time_impl::taylor_c_diff_func_f128(llvm_state &s, std::uint32_t, std::uint32_t batch_size) const
{
    return taylor_c_diff_time_impl<mppp::real128>(s, batch_size);
}

#endif

} // namespace detail

const expression time{func{detail::time_impl{}}};

} // namespace heyoka
