/**
 * @file    PktBertPObj.h
 * @brief   PktBertPObj — PRBS packet BERT processing object
 * @project XCESP
 * @date    2026-02-20
 */

#ifndef PKTBERTPOBJ_H
#define PKTBERTPOBJ_H

#include "ProcObject.h"
#include "PduLinkObject.h"

#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

/**
 * @brief  Configuration parameters for PktBertPObj, loaded from an INI section
 */
struct PktBertConfig {
    int         intervalMs    = 1000; ///< INTERVAL_MS: ms between BERT packets (default 1000)
    int         packetSize    = 64;   ///< PACKET_SIZE: bytes per test packet
    int         prbsType      = 7;    ///< PRBS_TYPE: 7, 11, or 15 (LFSR degree)
    uint32_t    packetLossPPM = 0;    ///< PACKET_LOSS_PPM: simulated drop rate in standalone mode (0–1 000 000)
    std::string linkName;             ///< LINK: named link to register as ROLE_SLAVE (empty = standalone loopback)
};

/**
 * @brief  Public runtime status for PktBertPObj
 */
struct PktBertStatus {
    ProcObject::ObjStatus objStatus = ProcObject::ObjStatus::IDLE;
    bool                  syncOk    = false; ///< true when the last received packet matched the PRBS sequence
};

/**
 * @brief  Accumulated BERT counters for PktBertPObj
 */
struct PktBertStats {
    uint64_t goodPackets = 0; ///< packets received matching the expected PRBS sequence
    uint64_t badPackets  = 0; ///< packets received NOT matching the expected sequence
};

/**
 * @brief  Internal processing state — not exposed via getStatus().
 *
 * Holds the TX and RX LFSR states, the pre-allocated TX packet buffer,
 * and the RNG used for drop decisions.
 */
struct PktBertLocalStatus {
    uint32_t             txLfsrState = 1;  ///< TX LFSR running state; initialised to 1
    uint32_t             rxLfsrState = 1;  ///< RX LFSR running state; starts in sync with TX
    std::vector<uint8_t> txPacket;         ///< pre-allocated TX packet buffer (sized in loadConfig)
    std::mt19937         rng;              ///< RNG for drop-decision (seeded in loadConfig)
};

/**
 * @brief  Processing object that runs a PRBS packet BERT.
 *
 * Operates in two modes depending on configuration:
 *
 * Standalone loopback mode (no LINK):
 *   IDLE   -> process() registers a repeating timer → ACTIVE
 *   ACTIVE -> timer fires: generate PRBS packet, optionally simulate drop, verify locally
 *
 * Link mode (LINK = <name>):
 *   IDLE   -> register as ROLE_SLAVE of named PduLink → register timer → ACTIVE
 *   ACTIVE -> timer fires: generate PRBS packet → sendPDU() to ROLE_MASTER (UdpTesterPObj)
 *             onSendPDU() called when UDP response arrives: verify received bytes against PRBS
 *
 * In both modes the TX Fibonacci LFSR fills packets with ITU-T PRBS-7/11/15 bytes.
 * After a mismatch the RX LFSR is re-synced to the TX position.
 *
 * ERROR: process() is a no-op (timer registration failed).
 */
class PktBertPObj : public ProcObject, public IPduReceiver {
public:
    ~PktBertPObj() override;

    // --- ProcObject interface ---

    void init(EvApplication& loop, LogManager& mgr,
              const std::string& appTag) override;
    bool loadConfig(IniConfig& ini, const std::string& section) override;
    void process() override;

    // --- IPduReceiver interface ---

    /**
     * @brief  Receive a PDU from the link peer (UdpTesterPObj) and verify it as BERT data.
     *
     * Called when UdpTesterPObj receives a UDP packet and forwards it back via sendPDU().
     * Runs on the processing thread — no locking needed.
     */
    void onSendPDU(const uint8_t* data, size_t len) override;

    // --- Virtual accessors (const void* per base contract) ---

    const void* getConfig() const override { return &config_; }

    /**
     * Returns the most recently published snapshot of PktBertStatus.
     * Safe to call from any thread — no lock required.
     */
    const void* getStatus() const override {
        return &statusSnap_[snapIdx_.load(std::memory_order_acquire)];
    }

    /**
     * Returns the most recently published snapshot of PktBertStats.
     * Safe to call from any thread — no lock required.
     */
    const void* getStats() const override {
        return &statsSnap_[snapIdx_.load(std::memory_order_acquire)];
    }

    void syncSnapshot() override;
    void clearStats()   override;

private:
    // --- Primary data (written exclusively by the processing thread) ---
    PktBertConfig      config_;
    PktBertStatus      status_;           ///< current status  — process thread only
    PktBertStats       stats_;            ///< current counters — process thread only
    PktBertLocalStatus local_;            ///< internal LFSR/RNG state — process thread only

    // --- Triple-buffer snapshot (lock-free, published via syncSnapshot()) ---
    PktBertStatus      statusSnap_[2]{};  ///< read buffers for main-thread consumers
    PktBertStats       statsSnap_[2]{};   ///< read buffers for main-thread consumers
    std::atomic<int>   snapIdx_{0};       ///< index of the valid (latest) snapshot

    int            bertTimerId_ = -1;      ///< BERT repeating timer; -1 = inactive
    PduLinkObject* pduLink_     = nullptr; ///< link obtained after registration; nullptr = standalone mode

    void onTimer();
    void receivePacket(const uint8_t* buf, int size);

    static void bertTimerCallback(int id, void* userData);
};

#endif // PKTBERTPOBJ_H
