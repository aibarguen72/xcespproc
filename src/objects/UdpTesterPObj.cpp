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

void UdpTesterPObj::init(EvApplication& loop, LogManager& mgr,
                         const std::string& appTag)
{
    ProcObject::init(loop, mgr, appTag);   // sets loop_, log_, logTag_
    log_->log(LOG_DEBUG, logTag_, "UdpTester initialized");
}

bool UdpTesterPObj::loadConfig(IniConfig& ini, const std::string& section)
{
    ProcObject::loadConfig(ini, section);  // sets name_

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
    // IDLE: attempt to open socket and transition to ACTIVE
    if (status_.objStatus == ObjStatus::IDLE) {
        if (log_ != nullptr)
            log_->log(LOG_INFO, logTag_, "Object IDLE. Trying to open socket");
        if (openSocket()) {
            status_.objStatus = ObjStatus::ACTIVE;
            status            = ObjStatus::ACTIVE;

            // Register event-driven send timer and recv socket callback
            if (loop_ != nullptr) {
                /*sendTimerId_ = loop_->addTimer(
                    config_.intervalMs,
                    &UdpTesterPObj::sendTimerCallback,
                    this,
                    true);*/
                loop_->addSocket(
                    status_.socketFd,
                    &UdpTesterPObj::recvCallback,
                    this);
            }

            if (log_ != nullptr)
                log_->log(LOG_INFO, logTag_, "socket opened, send timer registered");
        } else {
            status_.objStatus = ObjStatus::ERROR;
            status            = ObjStatus::ERROR;

            if (log_ != nullptr)
                log_->log(LOG_ERROR, logTag_, "openSocket() failed");
        }
    }
    // ACTIVE / ERROR: no-op — event callbacks drive sending and receiving
}

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

    // Pre-compute destination address into local_ (used by every send, avoids repetition)
    local_.dst.sin_family = AF_INET;
    local_.dst.sin_port   = htons(static_cast<uint16_t>(config_.dstPort));
    inet_pton(AF_INET, config_.dstIp.c_str(), &local_.dst.sin_addr);

    return true;
}

void UdpTesterPObj::closeSocket()
{
    if (sendTimerId_ >= 0 && loop_ != nullptr) {
        loop_->removeTimer(sendTimerId_);
        sendTimerId_ = -1;
    }
    if (status_.socketFd >= 0) {
        if (loop_ != nullptr)
            loop_->removeSocket(status_.socketFd);
        ::close(status_.socketFd);
        status_.socketFd = -1;
    }
}

void UdpTesterPObj::onSendTimer()
{
    if (status_.socketFd < 0) return;

    std::vector<uint8_t> payload(static_cast<size_t>(config_.packetSize), 0);
    ssize_t sent = sendto(status_.socketFd,
                          payload.data(), payload.size(),
                          0,
                          reinterpret_cast<const struct sockaddr*>(&local_.dst),
                          sizeof(local_.dst));
    if (sent > 0) {
        ++stats_.packetsSent;
        if (log_ != nullptr)
            log_->vlog(LOG_DEBUG, logTag_, "sent %d bytes", static_cast<int>(sent));
    } else {
        if (log_ != nullptr)
            log_->log(LOG_WARNING, logTag_, "sendto() failed");
    }
}

void UdpTesterPObj::onRecv()
{
    uint8_t buf[65536];
    ssize_t n = recvfrom(status_.socketFd,
                         buf, sizeof(buf),
                         MSG_DONTWAIT,
                         nullptr, nullptr);
    if (n > 0) {
        ++stats_.packetsReceived;
        if (log_ != nullptr)
            log_->vlog(LOG_DEBUG, logTag_, "received %zd bytes", n);
    }
}

// ---------------------------------------------------------------------------
// Static callback forwarders
// ---------------------------------------------------------------------------

void UdpTesterPObj::sendTimerCallback(int /*id*/, void* userData)
{
    static_cast<UdpTesterPObj*>(userData)->onSendTimer();
}

void UdpTesterPObj::recvCallback(int /*fd*/, void* userData)
{
    static_cast<UdpTesterPObj*>(userData)->onRecv();
}
