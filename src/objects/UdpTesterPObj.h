/**
 * @file    UdpTesterPObj.h
 * @brief   UdpTesterPObj — UDP packet sender/receiver processing object
 * @project XCESP
 * @date    2026-02-19
 */

#ifndef UDPTESTERPOBJ_H
#define UDPTESTERPOBJ_H

#include "ProcObject.h"

#include <chrono>
#include <cstdint>
#include <string>

/**
 * @brief  Configuration parameters for UdpTesterPObj, loaded from an INI section
 */
struct UdpTesterConfig {
    int         intervalMs  = 1000;       ///< INTERVAL_MS: ms between transmitted packets
    int         packetSize  = 64;         ///< PACKET_SIZE: payload size in bytes
    std::string srcIp       = "127.0.0.1"; ///< SRC_IP: local bind address
    int         srcPort     = 0;          ///< SRC_PORT: local bind port (0 = any)
    std::string dstIp       = "127.0.0.1"; ///< DST_IP: destination address
    int         dstPort     = 9999;       ///< DST_PORT: destination port
};

/**
 * @brief  Runtime status for UdpTesterPObj
 */
struct UdpTesterStatus {
    ProcObject::ObjStatus objStatus = ProcObject::ObjStatus::IDLE;
    int                   socketFd  = -1;
};

/**
 * @brief  Accumulated counters for UdpTesterPObj
 */
struct UdpTesterStats {
    uint64_t packetsSent     = 0;
    uint64_t packetsReceived = 0;
};

/**
 * @brief  Processing object that sends UDP packets at a configurable interval.
 *
 * State machine:
 *   IDLE   → openSocket() [SO_REUSEADDR + bind srcIp:srcPort]
 *              success → ACTIVE, record lastSendTime_
 *              failure → ERROR
 *   ACTIVE → if elapsed >= intervalMs: sendto(dstIp:dstPort), recvfrom(MSG_DONTWAIT)
 *   ERROR  → no-op
 */
class UdpTesterPObj : public ProcObject {
public:
    ~UdpTesterPObj() override;

    // --- ProcObject interface ---

    const std::string& getName()   const override;
    bool loadConfig(IniConfig& ini, const std::string& section) override;
    void process() override;

    // --- Accessors for inspection / test ---

    const UdpTesterConfig& getConfig() const;
    const UdpTesterStatus& getStatus() const;
    const UdpTesterStats&  getStats()  const;

private:
    std::string     name_;
    UdpTesterConfig config_;
    UdpTesterStatus status_;
    UdpTesterStats  stats_;
    std::chrono::steady_clock::time_point lastSendTime_{};

    bool openSocket();
    void closeSocket();
};

#endif // UDPTESTERPOBJ_H
