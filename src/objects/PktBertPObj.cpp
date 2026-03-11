/**
 * @file    PktBertPObj.cpp
 * @brief   PktBertPObj — PRBS packet BERT processing object implementation
 * @project XCESP
 * @date    2026-02-20
 */

#include "PktBertPObj.h"

#include <cinttypes>

// ---------------------------------------------------------------------------
// PRBS helper
// ---------------------------------------------------------------------------

/**
 * @brief  Advance a Fibonacci LFSR by 8 bits and return the output byte.
 *
 * ITU-T standard polynomials:
 *   PRBS-7:  x^7 + x^6 + 1   — feedback = bit[6] XOR bit[5]
 *   PRBS-11: x^11 + x^9 + 1  — feedback = bit[10] XOR bit[8]
 *   PRBS-15: x^15 + x^14 + 1 — feedback = bit[14] XOR bit[13]
 *
 * The all-zeros state is a lock-up condition; it is forced to 1 if reached.
 *
 * @param  state  LFSR state (modified in place)
 * @param  type   PRBS degree (7, 11, or 15); unknown values fall back to 7
 * @return next output byte (MSB first)
 */
static uint8_t prbsNextByte(uint32_t& state, int type)
{
    uint8_t out = 0;
    for (int b = 7; b >= 0; --b) {
        uint32_t fb;
        switch (type) {
            case 7:  fb = ((state >> 6) ^ (state >> 5)) & 1; break;
            case 11: fb = ((state >> 10) ^ (state >> 8)) & 1; break;
            case 15: fb = ((state >> 14) ^ (state >> 13)) & 1; break;
            default: fb = ((state >> 6) ^ (state >> 5)) & 1; break;
        }
        state = ((state << 1) | fb) & ((1u << type) - 1);
        if (state == 0) state = 1;          // prevent all-zeros lock-up
        out |= static_cast<uint8_t>(fb << b);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

PktBertPObj::~PktBertPObj()
{
    // Unregister from link before the timer is removed
    if (pduLink_ != nullptr && linkRegistry_ != nullptr) {
        linkRegistry_->unregisterLink(config_.linkName, this);
        pduLink_ = nullptr;
    }
    if (bertTimerId_ >= 0 && loop_ != nullptr) {
        loop_->removeTimer(bertTimerId_);
        bertTimerId_ = -1;
    }
}

// ---------------------------------------------------------------------------
// ProcObject interface
// ---------------------------------------------------------------------------

void PktBertPObj::init(EvApplication& loop, LogManager& mgr,
                       const std::string& appTag)
{
    ProcObject::init(loop, mgr, appTag);
    log_->log(LOG_DEBUG, logTag_, "PktBert initialized");
}

bool PktBertPObj::loadConfig(IniConfig& ini, const std::string& section)
{
    ProcObject::loadConfig(ini, section);

    config_.intervalMs    = static_cast<int>(
                                ini.getValueInteger(section, "INTERVAL_MS",    1000));
    config_.packetSize    = static_cast<int>(
                                ini.getValueInteger(section, "PACKET_SIZE",       64));
    config_.prbsType      = static_cast<int>(
                                ini.getValueInteger(section, "PRBS_TYPE",          7));
    config_.packetLossPPM = static_cast<uint32_t>(
                                ini.getValueInteger(section, "PACKET_LOSS_PPM",    0));
    config_.linkName      = ini.getValue(section, "LINK", std::string(""));
    config_.dstIp         = ini.getValue(section, "DST_IP",  std::string(""));
    config_.dstPort       = static_cast<uint16_t>(
                                ini.getValueInteger(section, "DST_PORT", 0));

    // intervalMs=0 is valid (pull-only mode: no self-timer, responds to onGetPDU)
    if (config_.intervalMs < 0) config_.intervalMs = 0;

    // Clamp prbsType to supported values; default to 7
    if (config_.prbsType != 7 && config_.prbsType != 11 && config_.prbsType != 15)
        config_.prbsType = 7;

    // Pre-allocate TX packet buffer
    local_.txPacket.resize(static_cast<size_t>(config_.packetSize));

    // Seed RNG deterministically from the initial LFSR state
    local_.rng.seed(local_.txLfsrState);

    return true;
}

void PktBertPObj::process()
{
    // IDLE: register link (if configured) and start the BERT timer → ACTIVE
    if (status_.objStatus == ObjStatus::IDLE) {
        if (log_ != nullptr)
            log_->log(LOG_INFO, logTag_, "Object IDLE. Starting BERT");

        // Register into named link as ROLE_SLAVE if configured
        if (!config_.linkName.empty() && linkRegistry_ != nullptr) {
            if (linkRegistry_->registerLink(config_.linkName, this,
                                            LinkRole::ROLE_SLAVE, "PDU")) {
                pduLink_ = static_cast<PduLinkObject*>(
                               linkRegistry_->getLink(config_.linkName));
                if (log_ != nullptr)
                    log_->log(LOG_INFO, logTag_,
                              "registered as SLAVE in link \"" + config_.linkName + "\"");
            } else {
                if (log_ != nullptr)
                    log_->log(LOG_WARNING, logTag_,
                              "registerLink failed for \"" + config_.linkName + "\"");
            }
        }

        if (config_.intervalMs > 0 && loop_ != nullptr) {
            bertTimerId_ = loop_->addTimer(
                config_.intervalMs,
                &PktBertPObj::bertTimerCallback,
                this,
                true);
        }

        // Pull-only mode (intervalMs=0) is valid: go ACTIVE without a self-timer.
        // Push mode requires a successfully registered timer.
        bool timerOk = (config_.intervalMs == 0) || (bertTimerId_ >= 0);
        if (timerOk) {
            status_.objStatus = ObjStatus::ACTIVE;
            status            = ObjStatus::ACTIVE;
            if (log_ != nullptr) {
                std::string msg;
                if (config_.intervalMs == 0) {
                    msg = "BERT active (pull-only, INTERVAL_MS=0)";
                } else {
                    msg = "BERT timer registered (" +
                          std::to_string(config_.intervalMs) + " ms)";
                }
                if (pduLink_ != nullptr) msg += ", link mode";
                else                      msg += ", standalone loopback";
                log_->log(LOG_INFO, logTag_, msg);
            }
        } else {
            status_.objStatus = ObjStatus::ERROR;
            status            = ObjStatus::ERROR;
            if (log_ != nullptr)
                log_->log(LOG_ERROR, logTag_, "addTimer() failed");
        }
    }
    // ACTIVE / ERROR: no-op — timer callback drives BERT logic

    // Publish fresh snapshot for main-thread consumers
    syncSnapshot();
}

// ---------------------------------------------------------------------------
// BERT logic
// ---------------------------------------------------------------------------

void PktBertPObj::onTimer()
{
    if (pduLink_ != nullptr) {
        // Link mode: generate and push packet via the link.
        generateAndSendPacket();
    } else {
        // Standalone loopback mode: generate, simulate optional drop, verify locally.
        for (int i = 0; i < config_.packetSize; ++i)
            local_.txPacket[static_cast<size_t>(i)] =
                prbsNextByte(local_.txLfsrState, config_.prbsType);

        bool drop = (config_.packetLossPPM > 0) &&
                    (local_.rng() % 1000000u < config_.packetLossPPM);
        if (drop) {
            if (log_ != nullptr)
                log_->log(LOG_DEBUG, logTag_, "packet dropped (loss simulation)");
            return;
        }
        receivePacket(local_.txPacket.data(), config_.packetSize);
    }
}

void PktBertPObj::generateAndSendPacket()
{
    // Fill TX packet with the next PRBS bytes
    for (int i = 0; i < config_.packetSize; ++i)
        local_.txPacket[static_cast<size_t>(i)] =
            prbsNextByte(local_.txLfsrState, config_.prbsType);

    // Silently skip if the link is not yet UP (master not registered yet)
    if (pduLink_ == nullptr || pduLink_->getState() != LinkObject::LinkState::UP)
        return;

    bool ok;
    if (!config_.dstIp.empty() && config_.dstPort != 0) {
        ok = pduLink_->sendPDUTo(this, local_.txPacket.data(),
                                  static_cast<size_t>(config_.packetSize),
                                  config_.dstIp, config_.dstPort);
    } else {
        ok = pduLink_->sendPDU(this, local_.txPacket.data(),
                                static_cast<size_t>(config_.packetSize));
    }
    if (!ok && log_ != nullptr)
        log_->log(LOG_WARNING, logTag_, "sendPDU/sendPDUTo failed");
}

void PktBertPObj::onSendPDU(const uint8_t* data, size_t len)
{
    // Called by UdpTesterPObj::onRecv() when a UDP packet arrives back.
    // Verify the received payload against the expected PRBS sequence.
    receivePacket(data, static_cast<int>(len));
}

void PktBertPObj::onGetPDU()
{
    // Pull request from the ROLE_MASTER (UdpTesterPObj timer).
    // Generate and push the next PRBS packet back via the link.
    if (status_.objStatus != ObjStatus::ACTIVE) return;
    generateAndSendPacket();
}

void PktBertPObj::receivePacket(const uint8_t* buf, int size)
{
    bool match = true;
    for (int i = 0; i < size; ++i) {
        uint8_t expected = prbsNextByte(local_.rxLfsrState, config_.prbsType);
        if (buf[i] != expected) {
            match = false;
            // Consume remaining expected bytes so rxLfsrState reaches end-of-packet
            for (int j = i + 1; j < size; ++j)
                prbsNextByte(local_.rxLfsrState, config_.prbsType);
            break;
        }
    }

    if (match) {
        ++stats_.goodPackets;
        status_.syncOk = true;
        if (log_ != nullptr)
            log_->vlog(LOG_DEBUG, logTag_, "good packet (total good=%" PRIu64 ")",
                       stats_.goodPackets);
    } else {
        ++stats_.badPackets;
        status_.syncOk = false;
        // Re-sync: advance RX LFSR to match TX position for next packet
        local_.rxLfsrState = local_.txLfsrState;
        if (log_ != nullptr)
            log_->vlog(LOG_WARNING, logTag_, "PRBS mismatch — re-synced (bad=%" PRIu64 ")",
                       stats_.badPackets);
    }
}

// ---------------------------------------------------------------------------
// Triple-buffer snapshot
// ---------------------------------------------------------------------------

void PktBertPObj::syncSnapshot()
{
    int next = snapIdx_.load(std::memory_order_relaxed) ^ 1;
    statusSnap_[next] = status_;
    statsSnap_[next]  = stats_;
    snapIdx_.store(next, std::memory_order_release);
}

void PktBertPObj::clearStats()
{
    // Called from the processing thread only — no locking needed.
    // The next syncSnapshot() will publish the zeroed counters.
    stats_ = PktBertStats{};
}

std::string PktBertPObj::buildStatusJson() const
{
    int idx = snapIdx_.load(std::memory_order_acquire);
    const PktBertStatus& st = statusSnap_[idx];
    const PktBertStats&  ss = statsSnap_[idx];

    const char* statusStr = "IDLE";
    if (st.objStatus == ObjStatus::ACTIVE) statusStr = "ACTIVE";
    else if (st.objStatus == ObjStatus::ERROR) statusStr = "ERROR";

    return std::string("{\"type\":\"PktBert\",\"name\":\"") + name_ +
           "\",\"node_type\":\"" + nodeType_ +
           "\",\"node_instance\":\"" + nodeInstance_ +
           "\",\"status\":\"" + statusStr +
           "\",\"stats\":{\"goodPackets\":" + std::to_string(ss.goodPackets) +
           ",\"badPackets\":" + std::to_string(ss.badPackets) +
           ",\"syncOk\":" + (st.syncOk ? "true" : "false") + "}}";
}

// ---------------------------------------------------------------------------
// Static callback forwarder
// ---------------------------------------------------------------------------

void PktBertPObj::bertTimerCallback(int /*id*/, void* userData)
{
    static_cast<PktBertPObj*>(userData)->onTimer();
}
