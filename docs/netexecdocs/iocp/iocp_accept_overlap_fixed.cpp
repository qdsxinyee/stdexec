// iocp_accept_overlap_fixed.cpp
// 对 iocp_accept_overlap_repro.cpp 的修复版本：
// 每个 AcceptEx 使用独立分配的 accept_op / OVERLAPPED，
// 由上下文持有，完成回调中释放。
//
// 编译：
//   cl /EHsc /utf-8 /Feiocp_accept_overlap_fixed.exe iocp_accept_overlap_fixed.cpp ws2_32.lib

#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <memory>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

struct accept_op {
    OVERLAPPED ol{};
    int        id{0};
    SOCKET     accept_socket{INVALID_SOCKET};
};

struct accept_context {
    HANDLE listen_handle;
    HANDLE iocp;
    LPFN_ACCEPTEX accept_ex;

    std::vector<std::unique_ptr<accept_op>> kept;

    auto release(accept_op* p) -> void {
        auto it = std::find_if(kept.begin(), kept.end(),
                               [p](const auto& up) { return up.get() == p; });
        if (it != kept.end()) {
            *it = std::move(kept.back());
            kept.pop_back();
        }
    }

    auto issue(int id) -> bool {
        auto op = std::make_unique<accept_op>();
        op->id = id;
        op->accept_socket = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                         nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (op->accept_socket == INVALID_SOCKET) {
            std::cerr << "WSASocket failed\n";
            return false;
        }

        ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(op->accept_socket), iocp, 0, 0);

        constexpr DWORD addr_len = sizeof(sockaddr_storage) + 16;
        static char buf[2 * (sizeof(sockaddr_storage) + 16)];
        DWORD transferred = 0;

        auto* raw = op.get();
        BOOL ok = accept_ex(reinterpret_cast<SOCKET>(listen_handle), op->accept_socket, buf, 0,
                            addr_len, addr_len, &transferred, &op->ol);
        if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
            std::cerr << "AcceptEx #" << id << " failed: " << WSAGetLastError() << "\n";
            return false;
        }

        std::cout << "AcceptEx #" << id << " issued (pending or sync)\n";
        kept.push_back(std::move(op));
        return true;
    }
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

    accept_context ctx{reinterpret_cast<HANDLE>(listen_sock), iocp, accept_ex};

    // 连续发起两次 accept，各自拥有独立的 accept_op
    if (!ctx.issue(1) || !ctx.issue(2)) {
        return 1;
    }

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
            ctx.release(op);
        }
    }

    std::sort(completed_ids.begin(), completed_ids.end());
    std::cout << "completed ids: ";
    for (int id : completed_ids) std::cout << id << " ";
    std::cout << "(expected: 1 2, order may vary)\n";

    bool ok = (completed_ids.size() == 2 && completed_ids[0] == 1 && completed_ids[1] == 2);

    ::closesocket(listen_sock);
    ::CloseHandle(iocp);
    WSACleanup();

    return ok ? 0 : 1;
}
