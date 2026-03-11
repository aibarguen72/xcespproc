/**
 * @file    UdpTesterPObj.h
 * @brief   UdpTesterPObj — UDP packet sender/receiver processing object
 * @project XCESP
 * @date    2026-02-19
 */

#ifndef UDPTESTERPOBJ_H
#define UDPTESTERPOBJ_H

#include "ProcObject.h"
#include "PduLinkObject.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <netinet/in.h>

/**
 * @brief  Configuration parameters for UdpTesterPObj, loaded from an INI section
 */
struct UdpTesterConfig {
    int         intervalMs  = 1000;        ///< INTERVAL_MS: ms between self-generated packets; 0 = disabled
    int         packetSize  = 64;          ///< PACKET_SIZE: payload size in bytes (self-send mode only)
    std::string srcIp       = "127.0.0.1"; ///< SRC_IP: local bind address
    int         srcPort     = 0;           ///< SRC_PORT: local bind port (0 = any)
    std::string dstIp       = "127.0.0.1"; ///< DST_IP: destination address
    int         dstPort     = 9999;        ///< DST_PORT: destination port
    std::string linkName;                  ///< LINK: named link to register as ROLE_MASTER (empty = no link)
    bool        shutdown    = false;       ///< SHUTDOWN: node defined but not instantiated as a real object
};

/**
 * @brief  Public runtime status for UdpTesterPObj
 */
struct UdpTesterStatus {
    ProcObject::ObjStatus objStatus = ProcObject::ObjStatus::IDLE;
};

/**
 * @brief  Accumulated counters for UdpTesterPObj
 */
struct UdpTesterStats {
    uint64_t packetsSent     = 0;
    uint64_t packetsReceived = 0;
};

/**
 * @brief  Internal processing state — not exposed via getStatus().
 *
 * Holds data needed during processing that is not relevant to external observers.
 */
struct UdpTesterLocalStatus {
    struct sockaddr_in    dst{};         ///< destination address, pre-computed when socket opens
    int                   socketFd = -1; ///< bound UDP socket fd; -1 when closed
    std::vector<uint8_t>  payload;       ///< send buffer, sized once in loadConfig()
};

/**
 * @brief  Processing object that sends and receives UDP packets.
 *
 * Operates in two modes depending on configuration:
 *
 * Self-send mode (INTERVAL_MS > 0, no LINK):
 *   IDLE   -> openSocket() → ACTIVE: register send timer + recv callback
 *   ACTIVE -> send timer fires → onSendTimer() sends self-generated payload to dstIp:dstPort
 *             socket readable  → onRecv() counts arriving packets
 *
 * Link mode (LINK = <name>, optionally INTERVAL_MS = 0):
 *   IDLE   -> openSocket() → register as ROLE_MASTER of named PduLink → ACTIVE
 *             If INTERVAL_MS > 0: also register self-send timer
 *   ACTIVE -> peer calls sendPDU() → onSendPDU() sends the PDU payload as UDP
 *             socket readable  → onRecv() forwards received UDP data to peer via sendPDU()
 *
 * ERROR: process() is a no-op.
 */
class UdpTesterPObj : public ProcObject, public IPduReceiver {
public:
    ~UdpTesterPObj() override;

    // --- ProcObject interface ---

    void init(EvApplication& loop, LogManager& mgr,
              const std::string& appTag) override;
    bool loadConfig(IniConfig& ini, const std::string& section) override;
    void process() override;

    // --- IPduReceiver interface ---

    /**
     * @brief  Send the given PDU as a UDP packet to the configured destination.
     *
     * Called by the link peer (e.g. PktBertPObj) to inject a packet into the
     * UDP path.  Runs on the processing thread — no locking needed.
     */
    void onSendPDU(const uint8_t* data, size_t len) override;

    /**
     * @brief  Send the given PDU as a UDP packet to an explicit destination.
     *
     * Called by PduLinkObject::sendPDUTo() when the peer supplies a one-shot
     * address override.  Uses dstIp:dstPort for this datagram instead of the
     * object's configured DST_IP:DST_PORT.
     *
     * Runs on the processing thread — no locking needed.
     */
    void onSendPDUTo(const uint8_t* data, size_t len,
                     const std::string& dstIp, uint16_t dstPort) override;

    // --- Virtual accessors (const void* per base contract) ---

    const void* getConfig() const override { return &config_; }

    /**
     * Returns the most recently published snapshot of UdpTesterStatus.
     * Safe to call from any thread — no lock required.
     */
    const void* getStatus() const override {
        return &statusSnap_[snapIdx_.load(std::memory_order_acquire)];
    }

    /**
     * Returns the most recently published snapshot of UdpTesterStats.
     * Safe to call from any thread — no lock required.
     */
    const void* getStats() const override {
        return &statsSnap_[snapIdx_.load(std::memory_order_acquire)];
    }

    void syncSnapshot() override;
    void clearStats()   override;
    std::string buildStatusJson() const override;

private:
    // --- Primary data (written exclusively by the processing thread) ---
    UdpTesterConfig      config_;
    UdpTesterStatus      status_;           ///< current status  — process thread only
    UdpTesterStats       stats_;            ///< current counters — process thread only
    UdpTesterLocalStatus local_;            ///< internal socket state — process thread only

    // --- Triple-buffer snapshot (lock-free, published via syncSnapshot()) ---
    // Two read buffers; snapIdx_ is the index of the latest valid one.
    // Process thread always writes to (snapIdx_^1), then flips snapIdx_ with release.
    // Main thread reads from snapIdx_ with acquire — no mutex needed.
    UdpTesterStatus      statusSnap_[2]{};  ///< read buffers for main-thread consumers
    UdpTesterStats       statsSnap_[2]{};   ///< read buffers for main-thread consumers
    std::atomic<int>     snapIdx_{0};       ///< index of the valid (latest) snapshot

    int            sendTimerId_ = -1;      ///< timer ID from loop_->addTimer(); -1 = inactive
    PduLinkObject* pduLink_     = nullptr; ///< link obtained after registration; nullptr = no link

    bool openSocket();
    void closeSocket();
    void onSendTimer();
    void onRecv();

    static void sendTimerCallback(int id, void* userData);
    static void recvCallback(int fd, void* userData);
};

#endif // UDPTESTERPOBJ_H
