/**
 * @file    PktBertPObj.h
 * @brief   PktBertPObj — PRBS packet BERT processing object
 * @project XCESP
 * @date    2026-02-20
 */

#ifndef PKTBERTPOBJ_H
#define PKTBERTPOBJ_H

#include "ProcObject.h"

#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

/**
 * @brief  Configuration parameters for PktBertPObj, loaded from an INI section
 */
struct PktBertConfig {
    int      packetSize    = 64;   ///< PACKET_SIZE: bytes per test packet
    int      prbsType      = 7;    ///< PRBS_TYPE: 7, 11, or 15 (LFSR degree)
    uint32_t packetLossPPM = 0;    ///< PACKET_LOSS_PPM: simulated drop rate (0–1 000 000)
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
 * @brief  Processing object that runs a loopback PRBS packet BERT.
 *
 * Every second the TX side fills a packet with PRBS bytes (Fibonacci LFSR,
 * ITU-T polynomials for degree 7, 11, or 15).  A configurable fraction of
 * packets is "dropped" (receiver not called) to simulate packet loss.
 * The RX side verifies received bytes against its own independently-running
 * copy of the same LFSR.  After a mismatch the RX LFSR is re-synced to the
 * TX LFSR so that normal operation resumes on the next non-dropped packet.
 *
 * State machine:
 *   IDLE   -> process() registers a 1-second repeating timer → ACTIVE
 *   ACTIVE -> timer fires each second: generate packet, maybe drop, then receive
 *   ERROR  -> process() is a no-op (timer registration failed)
 */
class PktBertPObj : public ProcObject {
public:
    ~PktBertPObj() override;

    // --- ProcObject interface ---

    void init(EvApplication& loop, LogManager& mgr,
              const std::string& appTag) override;
    bool loadConfig(IniConfig& ini, const std::string& section) override;
    void process() override;

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

    int bertTimerId_ = -1;               ///< 1-second BERT timer; -1 = inactive

    void onTimer();
    void receivePacket(const uint8_t* buf, int size);

    static void bertTimerCallback(int id, void* userData);
};

#endif // PKTBERTPOBJ_H
