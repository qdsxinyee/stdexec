// netexec/__detail/netexec_detail.hpp                               -*-C++-*-
// Internal metaprogramming utilities for netexec.
// Not intended for direct inclusion by users.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <stdexec/execution.hpp>
#include <exec/task.hpp>
#include <stdexec/__detail/__meta.hpp>
#include <stdexec/__detail/__tuple.hpp>
#include <variant>

namespace netexec::detail {

// ── forward_like ─────────────────────────────────────────────────────────────
template <typename T, typename U>
[[nodiscard]] constexpr auto forward_like(U&& u) noexcept
    -> decltype(stdexec::__forward_like<T>(std::forward<U>(u))) {
    return stdexec::__forward_like<T>(std::forward<U>(u));
}

// ── type_list ─────────────────────────────────────────────────────────────────
template <typename... Ts>
using type_list = stdexec::__mlist<Ts...>;

// ── variant_or_empty ──────────────────────────────────────────────────────────
// std::variant<Ts...> when non-empty; empty sentinel when Ts...={}.
// Uses std::variant (not stdexec::__variant) because callers rely on
// in_place_type_t constructors that stdexec::__variant doesn't expose.
namespace _voe {
    template <typename... Ts> struct _impl { using type = std::variant<Ts...>; };
    template <>               struct _impl<> { struct type {}; };
}
template <typename... Ts>
using variant_or_empty = typename _voe::_impl<Ts...>::type;

// ── decayed_tuple ─────────────────────────────────────────────────────────────
template <typename... Ts>
using decayed_tuple = stdexec::__decayed_tuple<Ts...>;

// ── product_type ─────────────────────────────────────────────────────────────
// Aggregate-initializable storage with .get<I>() member access.
// std::tuple is NOT aggregate-initializable so we need a custom type.
namespace _pt {
    template <std::size_t I, typename T>
    struct _leaf { T _value; };

    template <typename Idx, typename... Ts> struct _impl;

    template <std::size_t... Is, typename... Ts>
    struct _impl<std::index_sequence<Is...>, Ts...> : _leaf<Is, Ts>... {
        template <std::size_t I>
        auto& get() & noexcept {
            return static_cast<_leaf<I, std::tuple_element_t<I, std::tuple<Ts...>>>&>(*this)._value;
        }
        template <std::size_t I>
        const auto& get() const& noexcept {
            return static_cast<const _leaf<I, std::tuple_element_t<I, std::tuple<Ts...>>>&>(*this)._value;
        }
        template <std::size_t I>
        auto&& get() && noexcept {
            return std::move(static_cast<_leaf<I, std::tuple_element_t<I, std::tuple<Ts...>>>&>(*this)._value);
        }
    };
}
template <typename... Ts>
using product_type = _pt::_impl<std::index_sequence_for<Ts...>, Ts...>;

// ── stoppable_callback_for ────────────────────────────────────────────────────
template <typename Token, typename Callback>
using stoppable_callback_for = stdexec::stop_callback_for_t<Token, Callback>;

// ── sender_adaptor — CRTP base for pipeable adaptors ─────────────────────────
template <typename Derived>
using sender_adaptor = stdexec::sender_adaptor_closure<Derived>;

// ── make_sender_adaptor ───────────────────────────────────────────────────────
namespace _msa {
    template <typename Adaptor, typename... Args>
    struct _closure : stdexec::sender_adaptor_closure<_closure<Adaptor, Args...>> {
        Adaptor             _adaptor;
        std::tuple<Args...> _args;
        explicit _closure(Adaptor a, Args... as)
            : _adaptor(std::move(a)), _args(std::move(as)...) {}
        template <stdexec::sender Sender>
        auto operator()(Sender&& sndr) {
            return std::apply([&](auto&... args) {
                return _adaptor(std::forward<Sender>(sndr), args...);
            }, _args);
        }
    };
}
template <typename Adaptor, typename... Args>
auto make_sender_adaptor(Adaptor&& a, Args&&... args) {
    return _msa::_closure<std::remove_cvref_t<Adaptor>, std::remove_cvref_t<Args>...>{
        std::forward<Adaptor>(a), std::forward<Args>(args)...};
}

// ── meta — operations on completion_signatures / type_list ───────────────────
namespace meta {
namespace _impl {
    template <typename CS> struct unique_i;
    template <typename... Sigs>
    struct unique_i<stdexec::completion_signatures<Sigs...>> {
        using type = stdexec::__mapply_q<stdexec::completion_signatures, stdexec::__mmake_set<Sigs...>>;
    };

    template <template <typename> class Fn, typename CS> struct transform_i;
    template <template <typename> class Fn, typename... Sigs>
    struct transform_i<Fn, stdexec::completion_signatures<Sigs...>> {
        using type = stdexec::completion_signatures<Fn<Sigs>...>;
    };
    template <template <typename> class Fn, typename... Ts>
    struct transform_i<Fn, stdexec::__mlist<Ts...>> {
        using type = stdexec::__mlist<Fn<Ts>...>;
    };

    template <template <typename> class Pred, typename CS> struct filter_i;
    template <template <typename> class Pred, typename... Sigs>
    struct filter_i<Pred, stdexec::completion_signatures<Sigs...>> {
        using type = stdexec::__mapply_q<stdexec::completion_signatures,
            stdexec::__minvoke<stdexec::__mconcat<>,
                std::conditional_t<Pred<Sigs>::value,
                    stdexec::__mlist<Sigs>, stdexec::__mlist<>>...>>;
    };
    template <template <typename> class Pred, typename... Ts>
    struct filter_i<Pred, stdexec::__mlist<Ts...>> {
        using type = stdexec::__minvoke<stdexec::__mconcat<>,
            std::conditional_t<Pred<Ts>::value,
                stdexec::__mlist<Ts>, stdexec::__mlist<>>...>;
    };

    template <typename... CS> struct combine_i {
        template <typename X> struct unpack;
        template <typename... Sigs>
        struct unpack<stdexec::completion_signatures<Sigs...>> { using type = stdexec::__mlist<Sigs...>; };
        using all  = stdexec::__minvoke<stdexec::__mconcat<>, typename unpack<CS>::type...>;
        using type = stdexec::__mapply_q<stdexec::completion_signatures, all>;
    };
}

template <typename CS>
using unique    = typename _impl::unique_i<CS>::type;

template <template <typename> class Fn, typename CS>
using transform = typename _impl::transform_i<Fn, CS>::type;

template <template <typename> class Pred, typename CS>
using filter    = typename _impl::filter_i<Pred, CS>::type;

template <typename... CS>
using combine   = typename _impl::combine_i<CS...>::type;

} // namespace meta
} // namespace netexec::detail
