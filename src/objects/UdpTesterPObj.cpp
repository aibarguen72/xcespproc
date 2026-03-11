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
    config_.linkName   = ini.getValue(section, "LINK", std::string(""));
    config_.shutdown   = ini.getValueBoolean(section, "SHUTDOWN", false);

    local_.payload.assign(static_cast<size_t>(config_.packetSize), 0);

    return true;
}

void UdpTesterPObj::process()
{
    // Shutdown mode: node is declared but not instantiated as a real object
    if (config_.shutdown) return;

    // IDLE: attempt to open socket and transition to ACTIVE
    if (status_.objStatus == ObjStatus::IDLE) {
        if (log_ != nullptr)
            log_->log(LOG_INFO, logTag_, "Object IDLE. Trying to open socket");
        if (openSocket()) {
            status_.objStatus = ObjStatus::ACTIVE;
            status            = ObjStatus::ACTIVE;

            if (loop_ != nullptr) {
                // Self-send timer: only when INTERVAL_MS > 0
                if (config_.intervalMs > 0) {
                    sendTimerId_ = loop_->addTimer(
                        config_.intervalMs,
                        &UdpTesterPObj::sendTimerCallback,
                        this,
                        true);
                }
                // Always register recv callback (receives both self-send and link-routed packets)
                loop_->addSocket(
                    local_.socketFd,
                    &UdpTesterPObj::recvCallback,
                    this);
            }

            // Register into the named link as ROLE_MASTER if configured
            if (!config_.linkName.empty() && linkRegistry_ != nullptr) {
                if (linkRegistry_->registerLink(config_.linkName, this,
                                                LinkRole::ROLE_MASTER, "PDU")) {
                    pduLink_ = static_cast<PduLinkObject*>(
                                   linkRegistry_->getLink(config_.linkName));
                    if (log_ != nullptr)
                        log_->log(LOG_INFO, logTag_,
                                  "registered as MASTER in link \"" + config_.linkName + "\"");
                } else {
                    if (log_ != nullptr)
                        log_->log(LOG_WARNING, logTag_,
                                  "registerLink failed for \"" + config_.linkName + "\"");
                }
            }

            if (log_ != nullptr) {
                std::string msg = "socket opened";
                if (config_.intervalMs > 0) msg += ", send timer active";
                if (pduLink_ != nullptr)     msg += ", link registered";
                log_->log(LOG_INFO, logTag_, msg);
            }
        } else {
            status_.objStatus = ObjStatus::ERROR;
            status            = ObjStatus::ERROR;

            if (log_ != nullptr)
                log_->log(LOG_ERROR, logTag_, "openSocket() failed");
        }
    }
    // ACTIVE / ERROR: no-op — event callbacks drive sending and receiving

    // Publish a fresh snapshot so the main thread sees up-to-date data without locking
    syncSnapshot();
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

    local_.socketFd = fd;

    // Pre-compute destination address into local_ (used by every send, avoids repetition)
    local_.dst.sin_family = AF_INET;
    local_.dst.sin_port   = htons(static_cast<uint16_t>(config_.dstPort));
    inet_pton(AF_INET, config_.dstIp.c_str(), &local_.dst.sin_addr);

    return true;
}

void UdpTesterPObj::closeSocket()
{
    // Unregister from link before releasing the socket
    if (pduLink_ != nullptr && linkRegistry_ != nullptr) {
        linkRegistry_->unregisterLink(config_.linkName, this);
        pduLink_ = nullptr;
    }
    if (sendTimerId_ >= 0 && loop_ != nullptr) {
        loop_->removeTimer(sendTimerId_);
        sendTimerId_ = -1;
    }
    if (local_.socketFd >= 0) {
        if (loop_ != nullptr)
            loop_->removeSocket(local_.socketFd);
        ::close(local_.socketFd);
        local_.socketFd = -1;
    }
}

void UdpTesterPObj::onSendTimer()
{
    // Link mode: request a packet from the peer (pull mechanism) instead of self-sending
    if (pduLink_ != nullptr) {
        if (!pduLink_->getPDU(this)) {
            if (log_ != nullptr)
                log_->log(LOG_DEBUG, logTag_, "getPDU() returned false (link not UP yet?)");
        }
        return;
    }

    // Standalone: send own pre-allocated payload to the configured destination
    if (local_.socketFd < 0) return;

    ssize_t sent = sendto(local_.socketFd,
                          local_.payload.data(), local_.payload.size(),
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

void UdpTesterPObj::onSendPDU(const uint8_t* data, size_t len)
{
    if (local_.socketFd < 0) return;

    ssize_t sent = sendto(local_.socketFd,
                          data, len,
                          0,
                          reinterpret_cast<const struct sockaddr*>(&local_.dst),
                          sizeof(local_.dst));
    if (sent > 0) {
        ++stats_.packetsSent;
        if (log_ != nullptr)
            log_->vlog(LOG_DEBUG, logTag_, "link→UDP: sent %d bytes", static_cast<int>(sent));
    } else {
        if (log_ != nullptr)
            log_->log(LOG_WARNING, logTag_, "link→UDP: sendto() failed");
    }
}

void UdpTesterPObj::onSendPDUTo(const uint8_t* data, size_t len,
                                 const std::string& dstIp, uint16_t dstPort)
{
    if (local_.socketFd < 0) return;

    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(dstPort);
    inet_pton(AF_INET, dstIp.c_str(), &dst.sin_addr);

    ssize_t sent = sendto(local_.socketFd,
                          data, len,
                          0,
                          reinterpret_cast<const struct sockaddr*>(&dst),
                          sizeof(dst));
    if (sent > 0) {
        ++stats_.packetsSent;
        if (log_ != nullptr)
            log_->vlog(LOG_DEBUG, logTag_, "link→UDP(to %s:%u): sent %d bytes",
                       dstIp.c_str(), static_cast<unsigned>(dstPort),
                       static_cast<int>(sent));
    } else {
        if (log_ != nullptr)
            log_->log(LOG_WARNING, logTag_, "link→UDP(to): sendto() failed");
    }
}

void UdpTesterPObj::onRecv()
{
    uint8_t buf[65536];
    ssize_t n = recvfrom(local_.socketFd,
                         buf, sizeof(buf),
                         MSG_DONTWAIT,
                         nullptr, nullptr);
    if (n > 0) {
        ++stats_.packetsReceived;
        if (log_ != nullptr)
            log_->vlog(LOG_DEBUG, logTag_, "received %zd bytes", n);

        // In link mode: forward received UDP payload to the peer via sendPDU
        if (pduLink_ != nullptr) {
            if (!pduLink_->sendPDU(this, buf, static_cast<size_t>(n))) {
                if (log_ != nullptr)
                    log_->log(LOG_DEBUG, logTag_, "UDP→link: sendPDU() returned false (link not UP?)");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Triple-buffer snapshot
// ---------------------------------------------------------------------------

void UdpTesterPObj::syncSnapshot()
{
    // Write to the buffer that the main thread is NOT currently reading,
    // then atomically make it the active one (release → acquire pairing in readers).
    int next = snapIdx_.load(std::memory_order_relaxed) ^ 1;
    statusSnap_[next] = status_;
    statsSnap_[next]  = stats_;
    snapIdx_.store(next, std::memory_order_release);
}

void UdpTesterPObj::clearStats()
{
    // Called from the processing thread only — no locking needed.
    // The next syncSnapshot() will publish the zeroed counters.
    stats_ = UdpTesterStats{};
}

std::string UdpTesterPObj::buildStatusJson() const
{
    if (config_.shutdown) return "";

    int idx = snapIdx_.load(std::memory_order_acquire);
    const UdpTesterStatus& st = statusSnap_[idx];
    const UdpTesterStats&  ss = statsSnap_[idx];

    const char* statusStr = "IDLE";
    if (st.objStatus == ObjStatus::ACTIVE) statusStr = "ACTIVE";
    else if (st.objStatus == ObjStatus::ERROR) statusStr = "ERROR";

    return std::string("{\"type\":\"UdpTester\",\"name\":\"") + name_ +
           "\",\"node_type\":\"" + nodeType_ +
           "\",\"node_instance\":\"" + nodeInstance_ +
           "\",\"status\":\"" + statusStr +
           "\",\"stats\":{\"packetsSent\":" + std::to_string(ss.packetsSent) +
           ",\"packetsReceived\":" + std::to_string(ss.packetsReceived) + "}}";
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
