// iocp_nonblocking_inline.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Minimal IOCP echo server with NON-BLOCKING sockets and inline completion.
//
// Core idea:
//   * Sockets are non-blocking.
//   * If WSARecv/WSASend completes synchronously (rc == 0), the completion is
//     handled immediately in the same call stack.
//   * If it returns WSA_IO_PENDING, the operation is queued to the IOCP port.
//   * This avoids a kernel round-trip for operations that are immediately
//     satisfiable, but requires careful handling to avoid use-after-free.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>

#include <iostream>

#pragma comment(lib, "ws2_32.lib")

enum class op_type { accept, recv, send };

struct per_io_data {
    OVERLAPPED ol{};
    op_type    type;
    SOCKET     socket{INVALID_SOCKET};
    WSABUF     wsaBuf{};
    char       buffer[1024]{};
};

static HANDLE g_iocp{nullptr};
static LPFN_ACCEPTEX g_acceptEx{nullptr};
static SOCKET g_listen{INVALID_SOCKET};
static SOCKET g_acceptSock{INVALID_SOCKET};
static char   g_acceptBuf[2 * (sizeof(sockaddr_in) + 16)]{};

static void set_nonblocking(SOCKET s) {
    u_long mode = 1; // 1 == non-blocking
    if (::ioctlsocket(s, FIONBIO, &mode) == SOCKET_ERROR) {
        std::cerr << "ioctlsocket(FIONBIO, 1) failed: " << ::WSAGetLastError() << "\n";
    }
}

// Forward declarations.
static bool post_recv(per_io_data* io);
static bool post_send(per_io_data* io, DWORD len);

// Completion logic shared between inline path and IOCP path.
static void on_recv_complete(per_io_data* io, DWORD transferred) {
    if (transferred == 0) {
        ::closesocket(io->socket);
        delete io;
        return;
    }
    post_send(io, transferred);
}

static void on_send_complete(per_io_data* io, DWORD transferred) {
    (void)transferred;
    post_recv(io);
}

static bool post_recv(per_io_data* io) {
    io->type = op_type::recv;
    io->wsaBuf.buf = io->buffer;
    io->wsaBuf.len = sizeof(io->buffer);
    DWORD flags = 0;
    DWORD bytes = 0;
    int rc = ::WSARecv(io->socket, &io->wsaBuf, 1, &bytes, &flags, &io->ol, nullptr);

    if (rc == 0) {
        // Synchronous completion - handle inline.
        on_recv_complete(io, bytes);
        return true;
    }

    int err = ::WSAGetLastError();
    if (err == WSA_IO_PENDING) {
        return true; // will complete through IOCP
    }

    std::cerr << "WSARecv failed: " << err << "\n";
    return false;
}

static bool post_send(per_io_data* io, DWORD len) {
    io->type = op_type::send;
    io->wsaBuf.buf = io->buffer;
    io->wsaBuf.len = len;
    DWORD bytes = 0;
    int rc = ::WSASend(io->socket, &io->wsaBuf, 1, &bytes, 0, &io->ol, nullptr);

    if (rc == 0) {
        // Synchronous completion - handle inline.
        on_send_complete(io, bytes);
        return true;
    }

    int err = ::WSAGetLastError();
    if (err == WSA_IO_PENDING) {
        return true;
    }

    std::cerr << "WSASend failed: " << err << "\n";
    return false;
}

static bool post_accept() {
    g_acceptSock = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (g_acceptSock == INVALID_SOCKET) {
        return false;
    }
    set_nonblocking(g_acceptSock);

    auto* io = new per_io_data{};
    io->type = op_type::accept;
    DWORD bytes = 0;
    BOOL ok = g_acceptEx(g_listen,
                         g_acceptSock,
                         g_acceptBuf,
                         0,
                         sizeof(sockaddr_in) + 16,
                         sizeof(sockaddr_in) + 16,
                         &bytes,
                         &io->ol);
    if (!ok) {
        int err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING) {
            std::cerr << "AcceptEx failed: " << err << "\n";
            delete io;
            return false;
        }
    }
    return true;
}

int main() {
    WSADATA wsa{};
    ::WSAStartup(MAKEWORD(2, 2), &wsa);

    g_iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);

    SOCKET tmp = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    GUID guid = WSAID_ACCEPTEX;
    DWORD bytes = 0;
    ::WSAIoctl(tmp,
               SIO_GET_EXTENSION_FUNCTION_POINTER,
               &guid,
               sizeof(guid),
               &g_acceptEx,
               sizeof(g_acceptEx),
               &bytes,
               nullptr,
               nullptr);
    ::closesocket(tmp);

    g_listen = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    set_nonblocking(g_listen);
    ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_listen), g_iocp, 0, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(12345);
    addr.sin_addr.s_addr = INADDR_ANY;
    ::bind(g_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(g_listen, SOMAXCONN);

    post_accept();
    std::cout << "[non-blocking inline] IOCP echo server listening on 12345\n";

    while (true) {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ol = nullptr;
        BOOL ok = ::GetQueuedCompletionStatus(g_iocp, &transferred, &key, &ol, INFINITE);
        if (!ok || ol == nullptr) {
            continue;
        }

        auto* io = reinterpret_cast<per_io_data*>(ol);

        if (io->type == op_type::accept) {
            ::setsockopt(g_acceptSock,
                         SOL_SOCKET,
                         SO_UPDATE_ACCEPT_CONTEXT,
                         reinterpret_cast<char*>(&g_listen),
                         sizeof(SOCKET));
            ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_acceptSock), g_iocp, 0, 0);

            per_io_data* recv_io = new per_io_data{};
            recv_io->socket = g_acceptSock;
            post_recv(recv_io);

            post_accept();
            delete io;
        }
        else if (io->type == op_type::recv) {
            on_recv_complete(io, transferred);
        }
        else if (io->type == op_type::send) {
            on_send_complete(io, transferred);
        }
    }
}
