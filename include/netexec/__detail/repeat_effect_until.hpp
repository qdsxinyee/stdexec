// include/beman/net/detail/repeat_effect_until.hpp                   -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_INCLUDE_BEMAN_NET_DETAIL_REPEAT_EFFECT_UNTIL
#define INCLUDED_INCLUDE_BEMAN_NET_DETAIL_REPEAT_EFFECT_UNTIL
#include <netexec/__detail/netexec_detail.hpp>

#include <optional>
#include <type_traits>
#include <utility>

// ----------------------------------------------------------------------------

namespace netexec::detail {
struct repeat_effect_until_t : stdexec::sender_adaptor_closure<repeat_effect_until_t> {
    template <stdexec::sender Upstream, stdexec::sender Body, typename Predicate>
    auto operator()(Upstream&& upstream, Body&& body, Predicate&& predicate) const {
        return sender<std::remove_cvref_t<Upstream>, std::remove_cvref_t<Body>, std::remove_cvref_t<Predicate>>{
            std::forward<Upstream>(upstream), std::forward<Body>(body), std::forward<Predicate>(predicate)};
    }

    template <stdexec::sender Upstream,
              stdexec::sender Body,
              typename Predicate,
              stdexec::receiver Receiver>
    struct state {
        struct receiver {
            using receiver_concept = stdexec::receiver_t;
            state* _state;
            auto   get_env() const noexcept -> stdexec::env_of_t<Receiver>;
            auto   set_value() && noexcept -> void;
            template <typename Error>
            auto set_error(Error&& e) && noexcept -> void;
            auto set_stopped() && noexcept -> void;
        };
        using operation_state_concept = stdexec::operation_state_t;
        using upstream_state          = stdexec::connect_result_t<Upstream, receiver>;
        using body_state              = stdexec::connect_result_t<Body, receiver>;
        struct connector {
            template <stdexec::sender Sndr, stdexec::receiver Rcvr>
            connector(Sndr&& sndr, Rcvr&& rcvr)
                : _state(stdexec::connect(std::forward<Sndr>(sndr), std::forward<Rcvr>(rcvr))) {}
            connector(connector&&) = delete;
            body_state _state;
        };

        Body                     _body;
        Predicate                _predicate;
        Receiver                 _receiver;
        upstream_state           _up_state;
        std::optional<connector> _body_state{};

        template <stdexec::sender Up,
                  stdexec::sender By,
                  typename Pred,
                  stdexec::receiver Rcvr>
        state(Up&& up, By&& by, Pred&& pred, Rcvr&& rcvr) noexcept
            : _body(std::forward<By>(by)),
              _predicate(std::forward<Pred>(pred)),
              _receiver(std::forward<Rcvr>(rcvr)),
              _up_state(stdexec::connect(std::forward<Upstream>(up), receiver{this})) {}
        auto start() & noexcept -> void { stdexec::start(_up_state); }
        auto run_next() & noexcept -> void {
            this->_body_state.reset();
            if (this->_predicate()) {
                stdexec::set_value(std::move(this->_receiver));
            } else {
                this->_body_state.emplace(std::forward<Body>(this->_body), receiver{this});
                stdexec::start(this->_body_state->_state);
            }
        }
    };
    template <stdexec::sender Upstream, stdexec::sender Body, typename Predicate>
    struct sender {
        using sender_concept = stdexec::sender_t;
        using completion_signatures =
            stdexec::completion_signatures<stdexec::set_value_t(),
                                                    //-dk:TODO add error types of upstream and body
                                                    //-dk:TODO add stopped only if upstream or body can be stopped
                                                    stdexec::set_stopped_t()>;

        Upstream  upstream;
        Body      body;
        Predicate predicate;

        template <stdexec::receiver Receiver>
        auto connect(Receiver&& receiver) {
            return state<Upstream, Body, Predicate, std::remove_cvref_t<Receiver>>{std::move(this->upstream),
                                                                                   std::move(this->body),
                                                                                   std::move(this->predicate),
                                                                                   std::forward<Receiver>(receiver)};
        }
    };
};
} // namespace netexec::detail

namespace netexec {
using repeat_effect_until_t = netexec::detail::repeat_effect_until_t;
inline constexpr repeat_effect_until_t repeat_effect_until{};
} // namespace netexec

// ----------------------------------------------------------------------------

template <stdexec::sender Upstream,
          stdexec::sender Body,
          typename Predicate,
          stdexec::receiver Receiver>
auto netexec::detail::repeat_effect_until_t::state<Upstream, Body, Predicate, Receiver>::receiver::get_env()
    const noexcept -> stdexec::env_of_t<Receiver> {
    return stdexec::get_env(this->_state->_receiver);
}
template <stdexec::sender Upstream,
          stdexec::sender Body,
          typename Predicate,
          stdexec::receiver Receiver>
auto netexec::detail::repeat_effect_until_t::state<Upstream, Body, Predicate, Receiver>::receiver::
    set_value() && noexcept -> void {
    this->_state->run_next();
}
template <stdexec::sender Upstream,
          stdexec::sender Body,
          typename Predicate,
          stdexec::receiver Receiver>
template <typename Error>
auto netexec::detail::repeat_effect_until_t::state<Upstream, Body, Predicate, Receiver>::receiver::set_error(
    Error&& e) && noexcept -> void {
    stdexec::set_error(std::move(this->_state->_receiver), std::forward<Error>(e));
}
template <stdexec::sender Upstream,
          stdexec::sender Body,
          typename Predicate,
          stdexec::receiver Receiver>
auto netexec::detail::repeat_effect_until_t::state<Upstream, Body, Predicate, Receiver>::receiver::
    set_stopped() && noexcept -> void {
    stdexec::set_stopped(std::move(this->_state->_receiver));
}

// ----------------------------------------------------------------------------

#endif
