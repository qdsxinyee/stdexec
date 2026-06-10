// include/beman/net/detail/into_expected.hpp                         -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_INTO_EXPECTED
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_INTO_EXPECTED
#include <netexec/__detail/netexec_detail.hpp>

#include <concepts>
#if 202202 <= __cpp_lib_expected
#include <expected>
#endif
#include <variant>
#include <type_traits>

// ----------------------------------------------------------------------------

namespace netexec::detail {

#if 202202 <= __cpp_lib_expected
template <typename T, typename E>
using expected = std::expected<T, E>;
template <typename T>
using unexpected = std::unexpected<T>;
#else
template <typename T>
class unexpected {
  public:
    template <typename F>
    explicit unexpected(F&& f) noexcept : _error(std::forward<F>(f)) {}
    auto error() const& noexcept -> const T& { return this->_error; }
    auto error() && noexcept -> T&& { return std::move(this->_error); }

  private:
    std::remove_cvref_t<T> _error;
};
template <typename T>
unexpected(T&&) -> unexpected<std::remove_cvref_t<T>>;

template <typename T, typename E>
class expected {
  public:
    template <typename F>
    explicit expected(::netexec::detail::unexpected<F>&& e) noexcept
        : _value(std::in_place_index<1>, std::move(e).error()) {}
    template <typename F>
    explicit expected(const ::netexec::detail::unexpected<F>& e) noexcept
        : _value(std::in_place_index<1>, e.error()) {}
    template <typename... S>
    explicit expected(S&&... s) noexcept : _value(std::in_place_index<0>, std::forward<S>(s)...) {}

    explicit operator bool() const noexcept { return this->_value.index() == 0; }
    const T& value() const& { return std::get<0>(this->_value); }
    const E& error() const& { return std::get<1>(this->_value); }
    T&       value() & { return std::get<0>(this->_value); }
    E&       error() & { return std::get<1>(this->_value); }
    T&&      value() && { return std::get<0>(std::move(this->_value)); }
    E&&      error() && { return std::get<1>(std::move(this->_value)); }

  private:
    std::variant<std::remove_cvref_t<T>, std::remove_cvref_t<E>> _value;
};
#endif

struct into_expected_t : stdexec::sender_adaptor_closure<into_expected_t> {
    template <typename Sender, typename Env>
    using expected_t =
        netexec::detail::expected<stdexec::value_types_of_t<Sender, Env, std::tuple, std::type_identity_t>,
                                     stdexec::error_types_of_t<Sender, Env, std::type_identity_t>>;

    template <stdexec::sender Sender, stdexec::receiver Receiver>
    struct state {
        struct receiver {
            using receiver_concept = stdexec::receiver_t;
            using env_t            = stdexec::env_of_t<Receiver>;
            Receiver* _receiver;
            auto      get_env() const noexcept { return stdexec::get_env(*this->_receiver); }
            template <typename... Args>
            auto set_value(Args&&... args) && noexcept {
                stdexec::set_value(std::move(*this->_receiver),
                                            expected_t<Sender, env_t>(std::forward<Args>(args)...));
            }
            template <typename Error>
            auto set_error(Error&& error) && noexcept {
                stdexec::set_value(
                    std::move(*this->_receiver),
                    expected_t<Sender, env_t>(
                        netexec::detail::unexpected<std::remove_cvref_t<Error>>(std::forward<Error>(error))));
            }
            auto set_stopped() && noexcept { stdexec::set_stopped(std::move(*this->_receiver)); }
        };
        using operation_state_concept = stdexec::operation_state_t;
        using inner_state_t           = stdexec::connect_result_t<Sender, receiver>;

        Receiver      _receiver;
        inner_state_t _state;

        state(auto&& sndr, Receiver&& r)
            : _receiver(std::forward<Receiver>(r)),
              _state(stdexec::connect(std::forward<Sender>(sndr), receiver{&this->_receiver})) {}

        auto start() & noexcept -> void { stdexec::start(this->_state); }
    };
    template <stdexec::sender Sender>
    struct sender {
        using sender_concept = stdexec::sender_t;

        Sender _sender;

        template <typename Env>
        auto get_completion_signatures(const Env&) const {
            return stdexec::completion_signatures<stdexec::set_value_t(expected_t<Sender, Env>),
                                                           stdexec::set_stopped_t()>();
        }
        template <stdexec::receiver Receiver>
        auto connect(Receiver&& receiver) {
            return state<Sender, std::remove_cvref_t<Receiver>>(std::move(_sender), std::forward<Receiver>(receiver));
        }
    };

    template <stdexec::sender Sender>
    auto operator()(Sender&& sndr) const {
        return sender<std::remove_cvref_t<Sender>>{std::forward<Sender>(sndr)};
    }
};

inline constexpr into_expected_t into_expected{};
} // namespace netexec::detail

namespace netexec {
using into_expected_t = netexec::detail::into_expected_t;
inline constexpr into_expected_t into_expected{};
} // namespace netexec

// ----------------------------------------------------------------------------

#endif
