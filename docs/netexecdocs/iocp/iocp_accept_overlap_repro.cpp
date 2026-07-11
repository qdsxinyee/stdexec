// iocp_accept_overlap_repro.cpp
// 最小复现 netexec 旧代码中的 accept 问题：监听 socket 上所有 AcceptEx
// 共用一个 OVERLAPPED，导致第二个 pending 的 accept 把第一个覆盖，
// 最终第一个连接完成时无法找到正确的操作回调。
//
// 编译：
//   cl /EHsc /utf-8 /Feiocp_accept_overlap_repro.exe iocp_accept_overlap_repro.cpp ws2_32.lib
//
// 正常结果：应该看到两个 completion，id 分别为 1 和 2。
// 复现结果：通常只能看到一个 completion（id 为 2），或者两个 completion
// 的 id 都是 2，说明 operation 1 的完成被覆盖/丢失了。

#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>

#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

struct accept_op {
    OVERLAPPED ol{};
    int        id{0}; // 模拟 netexec 中 iocp_overlapped::completion 被覆盖
};

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        std::cerr << "socket failed\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "bind/listen failed\n";
        return 1;
    }

    int addrlen = sizeof(addr);
    ::getsockname(listen_sock, reinterpret_cast<sockaddr*>(&addr), &addrlen);
    int port = ntohs(addr.sin_port);
    std::cout << "listening on 127.0.0.1:" << port << "\n";

    HANDLE iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(listen_sock), iocp, 0, 0);

    LPFN_ACCEPTEX accept_ex = nullptr;
    GUID guid = WSAID_ACCEPTEX;
    DWORD bytes = 0;
    SOCKET tmp = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ::WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
               &accept_ex, sizeof(accept_ex), &bytes, nullptr, nullptr);
    ::closesocket(tmp);

    accept_op shared_op;
    char buf[2 * (sizeof(sockaddr_storage) + 16)]{};
    constexpr DWORD addr_len = sizeof(sockaddr_storage) + 16;

    // 模拟 netexec 旧代码：所有 accept 共用同一个 accept_op（即 iocp_socket_data::accept_ol）
    auto issue_accept = [&](int id, SOCKET accept_sock) {
        // 每次发起新的 accept 前把这个共享结构清零——这正是旧代码会做的
        ::memset(&shared_op, 0, sizeof(shared_op));
        shared_op.id = id;

        ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(accept_sock), iocp, 0, 0);

        DWORD transferred = 0;
        BOOL ok = accept_ex(listen_sock, accept_sock, buf, 0,
                            addr_len, addr_len, &transferred, &shared_op.ol);
        if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
            std::cerr << "AcceptEx #" << id << " failed: " << WSAGetLastError() << "\n";
            return false;
        }
        std::cout << "AcceptEx #" << id << " issued (pending or sync)\n";
        return true;
    };

    // 创建两个 accept socket
    SOCKET accept_sock[2] = {
        ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED),
        ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED)};

    // 连续发起两次 accept，共用同一个 shared_op
    if (!issue_accept(1, accept_sock[0]) || !issue_accept(2, accept_sock[1])) {
        return 1;
    }

    // 两个客户端连接
    std::thread client1([port] {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::closesocket(s);
    });

    std::thread client2([port] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::closesocket(s);
    });

    client1.join();
    client2.join();

    std::vector<int> completed_ids;
    DWORD start = GetTickCount();
    while (completed_ids.size() < 2 && GetTickCount() - start < 3000) {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED poverlapped = nullptr;
        BOOL ok = ::GetQueuedCompletionStatus(iocp, &transferred, &key,
                                              &poverlapped, 1000);
        if (!ok && poverlapped == nullptr) {
            std::cout << "GQS timeout, no more completions\n";
            break;
        }
        if (poverlapped) {
            auto* op = CONTAINING_RECORD(poverlapped, accept_op, ol);
            completed_ids.push_back(op->id);
            std::cout << "completion: id=" << op->id << "\n";
        }
    }

    std::cout << "completed ids: ";
    for (int id : completed_ids) std::cout << id << " ";
    std::cout << "(expected: 1 2)\n";

    bool ok = (completed_ids.size() == 2 && completed_ids[0] == 1 && completed_ids[1] == 2);

    ::closesocket(listen_sock);
    for (auto s : accept_sock) ::closesocket(s);
    ::CloseHandle(iocp);
    WSACleanup();

    return ok ? 0 : 1;
}
