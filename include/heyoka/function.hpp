// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef HEYOKA_FUNCTION_HPP
#define HEYOKA_FUNCTION_HPP

#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Attributes.h>

#include <heyoka/detail/fwd_decl.hpp>
#include <heyoka/detail/visibility.hpp>

namespace heyoka
{

class HEYOKA_DLL_PUBLIC function
{
public:
    enum class type { internal, external, builtin };

    using diff_t = std::function<expression(const std::vector<expression> &, const std::string &)>;
    using eval_dbl_t
        = std::function<double(const std::vector<expression> &, const std::unordered_map<std::string, double> &)>;
    using eval_batch_dbl_t
        = std::function<void(const std::vector<expression> &, const std::unordered_map<std::string, std::vector<double>> &,
                             std::vector<double> &)>;

private:
    bool m_disable_verify = false;
    std::string m_dbl_name, m_ldbl_name, m_display_name;
    std::unique_ptr<std::vector<expression>> m_args;
    std::vector<llvm::Attribute::AttrKind> m_attributes;
    type m_ty = type::internal;
    diff_t m_diff_f;
    eval_dbl_t m_eval_dbl_f;
    eval_batch_dbl_t m_eval_batch_dbl_f;

public:
    explicit function(std::vector<expression>);
    function(const function &);
    function(function &&) noexcept;
    ~function();

    bool &disable_verify();
    std::string &dbl_name();
    std::string &ldbl_name();
    std::string &display_name();
    std::vector<expression> &args();
    std::vector<llvm::Attribute::AttrKind> &attributes();
    type &ty();
    diff_t &diff_f();
    eval_dbl_t &eval_dbl_f();
    eval_batch_dbl_t &eval_batch_dbl_f();

    const bool &disable_verify() const;
    const std::string &dbl_name() const;
    const std::string &ldbl_name() const;
    const std::string &display_name() const;
    const std::vector<expression> &args() const;
    const std::vector<llvm::Attribute::AttrKind> &attributes() const;
    const type &ty() const;
    const diff_t &diff_f() const;
    const eval_dbl_t &eval_dbl_f() const;
    const eval_batch_dbl_t &eval_batch_dbl_f() const;
};

HEYOKA_DLL_PUBLIC std::ostream &operator<<(std::ostream &, const function &);

HEYOKA_DLL_PUBLIC std::vector<std::string> get_variables(const function &);

HEYOKA_DLL_PUBLIC bool operator==(const function &, const function &);
HEYOKA_DLL_PUBLIC bool operator!=(const function &, const function &);

HEYOKA_DLL_PUBLIC expression diff(const function &, const std::string &);

HEYOKA_DLL_PUBLIC double eval_dbl(const function &, const std::unordered_map<std::string, double> &);

HEYOKA_DLL_PUBLIC void eval_batch_dbl(const function &, const std::unordered_map<std::string, std::vector<double>>&,
                                      std::vector<double> &);

HEYOKA_DLL_PUBLIC void update_connections(const function &, std::vector<std::vector<unsigned>> &, unsigned &);

HEYOKA_DLL_PUBLIC llvm::Value *codegen_dbl(llvm_state &, const function &);

} // namespace heyoka

#endif
