#include "net/server.h"

#include <ws2tcpip.h>

#include <cstring>

#include "protocol.h"
#include "util/log.h"

namespace twin {

namespace {

bool recv_exact(SOCKET s, void* buf, int len) {
    char* p = static_cast<char*>(buf);
    int got = 0;
    while (got < len) {
        int r = ::recv(s, p + got, len - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

bool send_all(SOCKET s, const void* buf, int len) {
    const char* p = static_cast<const char*>(buf);
    int sent = 0;
    while (sent < len) {
        int r = ::send(s, p + sent, len - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

void set_nodelay(SOCKET s) {
    int no = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no),
                 sizeof(no));
}

}  // namespace

bool TcpServer::Start(uint16_t port) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log("WSAStartup failed");
        return false;
    }
    listen_sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) return false;
    set_nodelay(listen_sock_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        return false;
    if (::listen(listen_sock_, 2) != 0) return false;
    return true;
}

void TcpServer::Shutdown() {
    Disconnect();
    if (listen_sock_ != INVALID_SOCKET) {
        ::closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }
    WSACleanup();
}

bool TcpServer::WaitForClient(int timeout_ms) {
    fd_set f;
    FD_ZERO(&f);
    FD_SET(listen_sock_, &f);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int r = ::select(0, &f, nullptr, nullptr, &tv);
    if (r <= 0) return false;
    client_sock_ = ::accept(listen_sock_, nullptr, nullptr);
    if (client_sock_ == INVALID_SOCKET) return false;
    set_nodelay(client_sock_);
    Log("client connected");
    return true;
}

void TcpServer::Disconnect() {
    if (client_sock_ != INVALID_SOCKET) {
        ::closesocket(client_sock_);
        client_sock_ = INVALID_SOCKET;
    }
}

bool TcpServer::SendFrame(uint16_t type, const void* payload, uint32_t len) {
    if (client_sock_ == INVALID_SOCKET) return false;
    sp_header h{};
    h.magic = SP_MAGIC;
    h.version = SP_VERSION;
    h.type = type;
    h.seq = ++seq_;
    h.payload_len = len;
    if (!send_all(client_sock_, &h, sizeof(h))) {
        Disconnect();
        return false;
    }
    if (len && !send_all(client_sock_, payload, static_cast<int>(len))) {
        Disconnect();
        return false;
    }
    return true;
}

bool TcpServer::ReadFrame(uint16_t& type, std::vector<uint8_t>& payload, int timeout_ms) {
    if (client_sock_ == INVALID_SOCKET) return false;
    fd_set f;
    FD_ZERO(&f);
    FD_SET(client_sock_, &f);
    timeval tv{timeout_ms > 0 ? timeout_ms / 1000 : 0,
               timeout_ms > 0 ? (timeout_ms % 1000) * 1000 : 0};
    if (::select(0, &f, nullptr, nullptr, &tv) <= 0) return false;

    sp_header h;
    if (!recv_exact(client_sock_, &h, sizeof(h))) {
        Disconnect();
        return false;
    }
    if (h.magic != SP_MAGIC || h.version != SP_VERSION) {
        Log("bad frame magic/version");
        Disconnect();
        return false;
    }
    type = h.type;
    payload.resize(h.payload_len);
    if (h.payload_len && !recv_exact(client_sock_, payload.data(),
                                     static_cast<int>(h.payload_len))) {
        Disconnect();
        return false;
    }
    return true;
}

}  // namespace twin
