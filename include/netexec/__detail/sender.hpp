// include/beman/net/detail/sender.hpp                              -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_BEMAN_NET_DETAIL_SENDER
#define INCLUDED_BEMAN_NET_DETAIL_SENDER

#include <netexec/__detail/io_base.hpp>
#include <netexec/__detail/execution.hpp>
#include <netexec/__detail/stop_token.hpp>
#include <atomic>
#include <optional>
#include <type_traits>
#include <utility>

// ----------------------------------------------------------------------------

namespace netexec::detail {
template <stdexec::receiver>
struct sender_state_base;
template <stdexec::receiver>
struct sender_upstream_receiver;
template <typename, typename, stdexec::receiver, stdexec::sender>
struct sender_state;
template <typename, typename, stdexec::sender>
struct sender;
template <typename>
struct sender_cpo;
} // namespace netexec::detail

// ----------------------------------------------------------------------------

template <stdexec::receiver Receiver>
struct netexec::detail::sender_state_base {
    Receiver           d_receiver;
    ::std::atomic<int> d_outstanding{};

    template <stdexec::receiver R>
    sender_state_base(R&& r) : d_receiver(::std::forward<R>(r)) {}
    virtual ~sender_state_base()            = default;
    virtual auto start() & noexcept -> void = 0;
};

template <stdexec::receiver Receiver>
struct netexec::detail::sender_upstream_receiver {
    using receiver_concept = stdexec::receiver_t;
    ::netexec::detail::sender_state_base<Receiver>* d_state;

    auto set_value() && noexcept -> void { this->d_state->start(); }
    template <typename Error>
    auto set_error(Error&& error) && noexcept -> void {
        stdexec::set_error(::std::move(this->d_state->d_receiver), ::std::forward<Error>(error));
    }
    auto set_stopped() && noexcept -> void {
        stdexec::set_stopped(::std::move(this->d_state->d_receiver));
    }
    auto get_env() const noexcept { return stdexec::get_env(this->d_state->d_receiver); }
};

template <typename Desc,
          typename Data,
          stdexec::receiver Receiver,
          stdexec::sender   UpstreamSender>
struct netexec::detail::sender_state : Desc::operation, ::netexec::detail::sender_state_base<Receiver> {
    using operation_state_concept = stdexec::operation_state_t;

    struct cancel_callback : ::netexec::detail::io_base {
        sender_state* d_state;
        cancel_callback(sender_state* s)
            : ::netexec::detail::io_base(::netexec::detail::socket_id(), ::netexec::event_type::none),
              d_state(s) {}
        cancel_callback(cancel_callback&&) = default;
        auto operator()() {
            if (1 < ++this->d_state->d_outstanding) {
                this->d_state->d_data.get_scheduler().cancel(this, this->d_state);
            }
        }
        auto complete() -> void override final {
            if (0u == --this->d_state->d_outstanding) {
                stdexec::set_stopped(::std::move(this->d_state->d_receiver));
            }
        }
        auto error(::std::error_code) -> void override final { this->complete(); }
        auto cancel() -> void override final { this->complete(); }
    };
    using upstream_state_t = decltype(stdexec::connect(
        ::std::declval<UpstreamSender&>(), ::std::declval<sender_upstream_receiver<Receiver>>()));
    using stop_token       = decltype(stdexec::get_stop_token(
        stdexec::get_env(::std::declval<const Receiver&>())));
    using callback         = typename stop_token::template callback_type<cancel_callback>;

    Data                      d_data;
    upstream_state_t          d_state;
    ::std::optional<callback> d_callback;

    template <typename D, stdexec::receiver R>
    sender_state(D&& d, R&& r, UpstreamSender up)
        : Desc::operation(d.id(), d.events()),
          sender_state_base<Receiver>(::std::forward<R>(r)),
          d_data(::std::forward<D>(d)),
          d_state(stdexec::connect(up, sender_upstream_receiver<Receiver>{this})) {}
    auto start() & noexcept -> void override {
        auto token(stdexec::get_stop_token(stdexec::get_env(this->d_receiver)));
        static_assert(not std::same_as<stdexec::never_stop_token, void>);
        ++this->d_outstanding;
        this->d_callback.emplace(token, this);
        if (token.stop_requested()) {
            this->d_callback.reset();
            this->cancel();
            return;
        }
#ifdef _MSC_VER
        // On Windows, non-blocking sockets (e.g. accepted sockets marked via
        // set_nonblocking()) cause add_outstanding() to invoke work() inline.
        // If work() completes synchronously, it calls complete()/error()/cancel()
        // internally, which decrements d_outstanding and resumes the coroutine -
        // potentially destroying `this` before submit() returns.
        // Calling this->complete() again after that would be a use-after-free.
        // On Windows we therefore never touch `this` after submit().
        this->d_data.submit(this);
#else

        if (this->d_data.submit(this) == ::netexec::detail::submit_result::ready) {
            this->complete();
        }

#endif
    }
    auto complete() -> void override final {
        if (0 == --this->d_outstanding) {
            d_callback.reset();
            this->d_data.set_value(*this, ::std::move(this->d_receiver));
        }
    }
    auto error(::std::error_code err) -> void override final {
        if (0 == --this->d_outstanding) {
            d_callback.reset();
            stdexec::set_error(::std::move(this->d_receiver), std::move(err));
        }
    }
    auto cancel() -> void override final {
        if (0 == --this->d_outstanding) {
            stdexec::set_stopped(::std::move(this->d_receiver));
        }
    }
};

template <typename Desc, typename Data, stdexec::sender Upstream>
struct netexec::detail::sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<typename Data::completion_signature,
                                                        stdexec::set_error_t(::std::error_code),
                                                        stdexec::set_stopped_t()>;

    Data     d_data;
    Upstream d_upstream;

    template <stdexec::receiver Receiver>
    auto connect(Receiver&& receiver) const& {
        return ::netexec::detail::sender_state<Desc, Data, ::std::remove_cvref_t<Receiver>, Upstream>(
            this->d_data, ::std::forward<Receiver>(receiver), this->d_upstream);
    }
    template <stdexec::receiver Receiver>
    auto connect(Receiver&& receiver) && {
        return ::netexec::detail::sender_state<Desc, Data, ::std::remove_cvref_t<Receiver>, Upstream>(
            this->d_data, ::std::forward<Receiver>(receiver), this->d_upstream);
    }
};

template <typename Desc>
struct netexec::detail::sender_cpo {
    template <typename Arg0, typename... Args>
        requires(!stdexec::sender<::std::remove_cvref_t<Arg0>>) &&
                ::std::invocable<const sender_cpo, decltype(stdexec::just()), Arg0, Args...>
    auto operator()(Arg0&& arg0, Args&&... args) const {
        return (*this)(stdexec::just(), ::std::forward<Arg0>(arg0), ::std::forward<Args>(args)...);
    }
    template <stdexec::sender Upstream, typename... Args>
    auto operator()(Upstream&& u, Args&&... args) const {
        using Data = Desc::template data<::std::decay_t<Args>...>;
        return ::netexec::detail::sender<Desc, Data, ::std::remove_cvref_t<Upstream>>{
            Data{::std::forward<Args>(args)...}, ::std::forward<Upstream>(u)};
    }
};

// ----------------------------------------------------------------------------

#endif
