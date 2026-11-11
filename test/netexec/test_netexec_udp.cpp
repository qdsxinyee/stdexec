// test/netexec/test_netexec_udp.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

namespace {

struct capture_scheduler {
    ::msghdr* message{};
    auto receive(netexec::detail::context_base::receive_operation*) -> netexec::detail::submit_result;
};

struct fake_datagram_stream {
    capture_scheduler* scheduler;
    auto id() const -> netexec::detail::socket_id { return netexec::detail::socket_id::invalid; }
    auto get_scheduler() -> capture_scheduler& { return *this->scheduler; }
};

struct receive_operation final : netexec::detail::context_base::receive_operation {
    receive_operation()
        : netexec::detail::context_base::receive_operation(
              netexec::detail::socket_id::invalid, netexec::event_type::none) {}
    auto complete() -> void override {}
    auto error(std::error_code) -> void override {}
    auto cancel() -> void override {}
};

auto capture_scheduler::receive(netexec::detail::context_base::receive_operation* op)
    -> netexec::detail::submit_result {
    this->message = &::std::get<0>(*op);
    return netexec::detail::submit_result::ready;
}

} // namespace

TEST_CASE("netexec - receive_from uses endpoint storage for the source address", "[netexec][udp]") {
    char bytes[16];
    auto buffer = net::buffer(bytes);
    auto endpoint = netexec_test::make_server_endpoint(12345);
    capture_scheduler scheduler;
    fake_datagram_stream stream{&scheduler};
    netexec::detail::receive_from_desc::data<
        fake_datagram_stream,
        net::mutable_buffer,
        net::ip::tcp::endpoint>
        data{stream, buffer, endpoint};
    receive_operation op;

    data.submit(&op);

    REQUIRE(scheduler.message != nullptr);
    CHECK(scheduler.message->msg_iov == data.d_buffers.data());
    CHECK(scheduler.message->msg_iovlen == data.d_buffers.size());
    CHECK(scheduler.message->msg_name == data.d_endpoint.data());
    CHECK(scheduler.message->msg_namelen == data.d_endpoint.size());
}
