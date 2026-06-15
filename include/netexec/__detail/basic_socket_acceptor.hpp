// include/beman/net/detail/basic_socket_acceptor.hpp               -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef INCLUDED_BEMAN_NET_DETAIL_BASIC_SOCKET_ACCEPTOR
#define INCLUDED_BEMAN_NET_DETAIL_BASIC_SOCKET_ACCEPTOR

#include <netexec/__detail/io_context.hpp>
#include <netexec/__detail/socket_category.hpp>
#include <system_error>

// ----------------------------------------------------------------------------

namespace netexec {
template <typename>
class basic_socket_acceptor;
}

// ----------------------------------------------------------------------------

template <typename AcceptableProtocol>
class netexec::basic_socket_acceptor : public ::netexec::socket_base {
  public:
    using scheduler_type     = ::netexec::io_context::scheduler_type;
    using executor_type      = ::netexec::io_context::executor_type;
    using native_handle_type = ::netexec::detail::native_handle_type;
    using protocol_type      = AcceptableProtocol;
    using endpoint_type      = typename protocol_type::endpoint;
    using socket_type        = typename protocol_type::socket;

  private:
    ::netexec::io_context&       d_context;
    protocol_type                   d_protocol;
    ::netexec::detail::socket_id d_id{};

  private:
    template <typename Fun_t>
    static void dispatch(Fun_t&& fun) {
        ::std::error_code error{};
        fun(error);
        if (error) {
            throw ::std::system_error(error);
        }
    }

  public:
    // explicit basic_socket_acceptor(::netexec::io_context&);
    basic_socket_acceptor(::netexec::io_context&, const protocol_type& protocol);
    basic_socket_acceptor(::netexec::io_context& context, const endpoint_type& endpoint, bool reuse = true)
        : ::netexec::socket_base(),
          d_context(context),
          d_protocol(endpoint.protocol()),
          d_id(::netexec::detail::socket_id::invalid) {
        this->open(endpoint.protocol());
        if (reuse) {
            this->set_option(::netexec::socket_base::reuse_address(true));
        }
        this->bind(endpoint);
        this->listen();
    }
    basic_socket_acceptor(::netexec::io_context&, const protocol_type&, const native_handle_type&);
    basic_socket_acceptor(const basic_socket_acceptor&) = delete;
    basic_socket_acceptor(basic_socket_acceptor&& other)
        : ::netexec::socket_base(),
          d_context(other.d_context),
          d_protocol(other.d_protocol),
          d_id(::std::exchange(other.d_id, ::netexec::detail::socket_id::invalid)) {}
    template <typename OtherProtocol>
    basic_socket_acceptor(::netexec::basic_socket_acceptor<OtherProtocol>&&);
    ~basic_socket_acceptor() {
        //-dk:TODO assert that there is no outstanding work?
        ::std::error_code error{};
        this->close(error);
    }
    basic_socket_acceptor& operator=(const basic_socket_acceptor&) = delete;
    basic_socket_acceptor& operator=(basic_socket_acceptor&&);
    template <typename OtherProtocol>
    basic_socket_acceptor& operator=(::netexec::basic_socket_acceptor<OtherProtocol>&&);

    auto          get_context() -> ::netexec::io_context& { return this->d_context; }
    auto          get_scheduler() noexcept -> scheduler_type { return this->d_context.get_scheduler(); }
    executor_type get_executor() noexcept;
    auto          native_handle() -> native_handle_type { return this->d_context.native_handle(this->d_id); }
    auto          _native_handle() const -> native_handle_type { return this->d_context.native_handle(this->d_id); }
    auto          id() const -> ::netexec::detail::socket_id { return this->d_id; }
    auto          open(const protocol_type& p = protocol_type()) -> void {
        dispatch([this, &p](::std::error_code& error) { this->open(p, error); });
    }
    auto open(const protocol_type& p, ::std::error_code& error) -> void {
        if (this->is_open()) {
            error = ::std::error_code(int(socket_errc::already_open), ::netexec::socket_category());
        }
        this->d_id = this->d_context.make_socket(p.family(), p.type(), p.protocol(), error);
    }
    void               assign(const protocol_type&, const native_handle_type&);
    void               assign(const protocol_type&, const native_handle_type&, ::std::error_code&);
    native_handle_type release();
    native_handle_type release(::std::error_code&);
    auto is_open() const noexcept -> bool { return this->d_id != ::netexec::detail::socket_id::invalid; }
    auto close() -> void {
        dispatch([this](auto& error) { return this->close(error); });
    }
    auto close(::std::error_code& error) -> void {
        //-dk:TODO cancel outstanding work
        if (this->is_open()) {
            this->d_context.release(this->id(), error);
            this->d_id = ::netexec::detail::socket_id::invalid;
        }
    }
    void cancel();
    void cancel(::std::error_code&);
    template <typename SettableSocketOption>
    auto set_option(const SettableSocketOption& option) -> void {
        dispatch([this, option](::std::error_code& error) { this->set_option(option, error); });
    }
    template <typename SettableSocketOption>
    auto set_option(const SettableSocketOption& option, ::std::error_code& error) -> void {
        this->d_context.set_option(this->id(),
                                   option.level(this->d_protocol),
                                   option.name(this->d_protocol),
                                   option.data(this->d_protocol),
                                   option.size(this->d_protocol),
                                   error);
    }

    template <typename GettableSocketOption>
    void get_option(GettableSocketOption&) const;
    template <typename gettableSocketOption>
    void get_option(gettableSocketOption&, ::std::error_code&) const;
    template <typename IoControlCommand>
    void io_control(IoControlCommand&);
    template <typename IoControlCommand>
    void io_control(IoControlCommand&, ::std::error_code&);
    void non_blocking(bool);
    void non_blocking(bool, ::std::error_code&);
    bool non_blocking() const;
    void native_non_blocking(bool);
    void native_non_blocking(bool, ::std::error_code&);
    bool native_non_blocking() const;
    auto bind(const endpoint_type& endpoint) -> void {
        this->dispatch([this, endpoint](::std::error_code& error) { this->bind(endpoint, error); });
    }
    auto bind(const endpoint_type& endpoint, ::std::error_code& error) -> void {
        this->d_context.bind(this->d_id, endpoint, error);
    }
    auto listen(int no = ::netexec::socket_base::max_listen_connections) -> void {
        dispatch([this, no](auto& error) { this->listen(no, error); });
    }
    auto          listen(int no, ::std::error_code& error) -> void { this->d_context.listen(this->d_id, no, error); }
    endpoint_type local_endpoint() const;
    endpoint_type local_endpoint(::std::error_code&) const;
    void          enable_connection_aborted(bool);
    bool          enable_connection_aborted() const;
    socket_type   accept();
    socket_type   accept(::std::error_code&);
    socket_type   accept(io_context&);
    socket_type   accept(io_context&, ::std::error_code&);
    socket_type   accept(endpoint_type&);
    socket_type   accept(endpoint_type&, ::std::error_code&);
    socket_type   accept(::netexec::io_context&, endpoint_type&);
    socket_type   accept(::netexec::io_context&, endpoint_type&, ::std::error_code&);
    void          wait(::netexec::socket_base::wait_type);
    void          wait(::netexec::socket_base::wait_type, ::std::error_code&);
};

// ----------------------------------------------------------------------------

#endif
