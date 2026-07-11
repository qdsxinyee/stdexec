#include <winsock2.h>
#include <windows.h>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

#define PORT 9000
#define BUFFER_SIZE 1024

struct PerIoData {
    OVERLAPPED overlapped;
    WSABUF buffer;
    char data[BUFFER_SIZE];
};

DWORD WINAPI WorkerThread(LPVOID param) {
    HANDLE iocp = (HANDLE)param;

    while (true) {
        DWORD bytesTransferred;
        ULONG_PTR completionKey;
        LPOVERLAPPED overlapped;

        BOOL result = GetQueuedCompletionStatus(
            iocp,
            &bytesTransferred,
            &completionKey,
            &overlapped,
            INFINITE
        );

        SOCKET client = (SOCKET)completionKey;
        PerIoData* ioData = (PerIoData*)overlapped;

        if (bytesTransferred == 0) {
            std::cout << "Client disconnected\n";
            closesocket(client);
            delete ioData;
            continue;
        }

        // Echo back
        DWORD sent;
        WSASend(client, &ioData->buffer, 1, &sent, 0, &ioData->overlapped, NULL);

        // 再次投递接收
        ZeroMemory(&ioData->overlapped, sizeof(OVERLAPPED));
        DWORD flags = 0;
        WSARecv(client, &ioData->buffer, 1, NULL, &flags, &ioData->overlapped, NULL);
    }

    return 0;
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(listenSock, (sockaddr*)&addr, sizeof(addr));
    listen(listenSock, SOMAXCONN);

    // 创建 IOCP
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

    // 创建 worker 线程
    for (int i = 0; i < 4; ++i) {
        CreateThread(NULL, 0, WorkerThread, iocp, 0, NULL);
    }

    std::cout << "Server started on port " << PORT << "\n";

    while (true) {
        SOCKET client = accept(listenSock, NULL, NULL);

        std::cout << "Client connected\n";

        // 绑定 socket 到 IOCP
        CreateIoCompletionPort((HANDLE)client, iocp, (ULONG_PTR)client, 0);

        // 分配 IO 数据
        PerIoData* ioData = new PerIoData{};
        ioData->buffer.buf = ioData->data;
        ioData->buffer.len = BUFFER_SIZE;

        DWORD flags = 0;

        // 投递第一次异步接收
        WSARecv(client, &ioData->buffer, 1, NULL, &flags, &ioData->overlapped, NULL);
    }

    WSACleanup();
    return 0;
}