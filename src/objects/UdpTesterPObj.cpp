/**
 * @file    UdpTesterPObj.cpp
 * @brief   UdpTesterPObj — UDP packet sender/receiver processing object implementation
 * @project XCESP
 * @date    2026-02-19
 */

#include "UdpTesterPObj.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <vector>

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

UdpTesterPObj::~UdpTesterPObj()
{
    closeSocket();
}

// ---------------------------------------------------------------------------
// ProcObject interface
// ---------------------------------------------------------------------------

const std::string& UdpTesterPObj::getName() const
{
    return name_;
}

bool UdpTesterPObj::loadConfig(IniConfig& ini, const std::string& section)
{
    name_ = section;

    config_.intervalMs = static_cast<int>(ini.getValueInteger(section, "INTERVAL_MS",  1000));
    config_.packetSize = static_cast<int>(ini.getValueInteger(section, "PACKET_SIZE",    64));
    config_.srcIp      = ini.getValue(section, "SRC_IP", std::string("127.0.0.1"));
    config_.srcPort    = static_cast<int>(ini.getValueInteger(section, "SRC_PORT",         0));
    config_.dstIp      = ini.getValue(section, "DST_IP", std::string("127.0.0.1"));
    config_.dstPort    = static_cast<int>(ini.getValueInteger(section, "DST_PORT",      9999));

    return true;
}

void UdpTesterPObj::process()
{
    using namespace std::chrono;

    // --- IDLE: attempt to open socket and transition to ACTIVE ---
    if (status_.objStatus == ObjStatus::IDLE) {
        if (openSocket()) {
            status_.objStatus = ObjStatus::ACTIVE;
            status            = ObjStatus::ACTIVE;
            lastSendTime_     = steady_clock::now();
        } else {
            status_.objStatus = ObjStatus::ERROR;
            status            = ObjStatus::ERROR;
        }
        return;
    }

    // --- ERROR: no-op ---
    if (status_.objStatus == ObjStatus::ERROR) {
        return;
    }

    // --- ACTIVE: send packet when interval has elapsed ---
    auto now     = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(now - lastSendTime_).count();

    if (elapsed >= static_cast<long long>(config_.intervalMs)) {
        // Build destination address
        struct sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(static_cast<uint16_t>(config_.dstPort));
        inet_pton(AF_INET, config_.dstIp.c_str(), &dst.sin_addr);

        // Send zero-filled payload
        std::vector<uint8_t> payload(static_cast<size_t>(config_.packetSize), 0);
        sendto(status_.socketFd,
               payload.data(), payload.size(),
               0,
               reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst));
        ++stats_.packetsSent;

        // Non-blocking receive attempt
        uint8_t buf[65536];
        ssize_t n = recvfrom(status_.socketFd,
                             buf, sizeof(buf),
                             MSG_DONTWAIT,
                             nullptr, nullptr);
        if (n > 0) {
            ++stats_.packetsReceived;
        }

        lastSendTime_ = now;
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const UdpTesterConfig& UdpTesterPObj::getConfig() const { return config_; }
const UdpTesterStatus& UdpTesterPObj::getStatus() const { return status_; }
const UdpTesterStats&  UdpTesterPObj::getStats()  const { return stats_;  }

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool UdpTesterPObj::openSocket()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(config_.srcPort));
    inet_pton(AF_INET, config_.srcIp.c_str(), &addr.sin_addr);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    status_.socketFd = fd;
    return true;
}

void UdpTesterPObj::closeSocket()
{
    if (status_.socketFd >= 0) {
        ::close(status_.socketFd);
        status_.socketFd = -1;
    }
}
