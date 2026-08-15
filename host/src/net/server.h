#pragma once

#include <cstdint>
#include <vector>

#include <winsock2.h>

namespace twin {

/*
 * Minimal blocking TCP server that speaks the twin-screen framing.
 * One client at a time. Winsock is initialized in Start() and cleaned in
 * Shutdown().
 */
class TcpServer {
public:
    TcpServer() = default;
    ~TcpServer() { Shutdown(); }

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool Start(uint16_t port);
    void Shutdown();

    bool WaitForClient(int timeout_ms);
    void Disconnect();

    bool SendFrame(uint16_t type, const void* payload, uint32_t len);
    /* Reads one message. timeout_ms <= 0 polls without blocking. */
    bool ReadFrame(uint16_t& type, std::vector<uint8_t>& payload, int timeout_ms);

    bool HasClient() const { return client_sock_ != INVALID_SOCKET; }

private:
    SOCKET listen_sock_ = INVALID_SOCKET;
    SOCKET client_sock_ = INVALID_SOCKET;
    uint32_t seq_ = 0;
};

}  // namespace twin
