/**
 * @file    UdpTesterPObj.h
 * @brief   UdpTesterPObj — UDP packet sender/receiver processing object
 * @project XCESP
 * @date    2026-02-19
 */

#ifndef UDPTESTERPOBJ_H
#define UDPTESTERPOBJ_H

#include "ProcObject.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <netinet/in.h>

/**
 * @brief  Configuration parameters for UdpTesterPObj, loaded from an INI section
 */
struct UdpTesterConfig {
    int         intervalMs  = 1000;        ///< INTERVAL_MS: ms between transmitted packets
    int         packetSize  = 64;          ///< PACKET_SIZE: payload size in bytes
    std::string srcIp       = "127.0.0.1"; ///< SRC_IP: local bind address
    int         srcPort     = 0;           ///< SRC_PORT: local bind port (0 = any)
    std::string dstIp       = "127.0.0.1"; ///< DST_IP: destination address
    int         dstPort     = 9999;        ///< DST_PORT: destination port
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
 * @brief  Processing object that sends UDP packets at a configurable interval.
 *
 * State machine:
 *   IDLE   -> process() calls openSocket() [SO_REUSEADDR + bind srcIp:srcPort]
 *              success -> ACTIVE: registers send timer and recv socket callback
 *              failure -> ERROR
 *   ACTIVE -> send timer fires -> onSendTimer() sends to dstIp:dstPort
 *             socket readable  -> onRecv() reads arriving packets
 *   ERROR  -> process() is a no-op
 */
class UdpTesterPObj : public ProcObject {
public:
    ~UdpTesterPObj() override;

    // --- ProcObject interface ---

    void init(EvApplication& loop, LogManager& mgr,
              const std::string& appTag) override;
    bool loadConfig(IniConfig& ini, const std::string& section) override;
    void process() override;

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

    int sendTimerId_ = -1;             ///< timer ID from loop_->addTimer(); -1 = inactive

    bool openSocket();
    void closeSocket();
    void onSendTimer();
    void onRecv();

    static void sendTimerCallback(int id, void* userData);
    static void recvCallback(int fd, void* userData);
};

#endif // UDPTESTERPOBJ_H
