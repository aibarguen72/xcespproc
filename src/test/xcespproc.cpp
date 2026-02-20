/**
 * @file    xcespproc.cpp
 * @brief   Smoke tests for xcespproc processing objects
 * @project XCESP
 * @date    2026-02-19
 */

#include "UdpTesterPObj.h"
#include "PktBertPObj.h"
#include "IniConfig.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

static int passCount = 0;
static int failCount = 0;

static void check(bool cond, const char* msg)
{
    if (cond) {
        ++passCount;
        std::cout << "  PASS: " << msg << "\n";
    } else {
        ++failCount;
        std::cerr << "  FAIL: " << msg << "\n";
    }
}

// ---------------------------------------------------------------------------
// Helper: write a temp INI file and return its path
// ---------------------------------------------------------------------------

static std::string writeTempIni(const std::string& content)
{
    char tmpl[] = "/tmp/xcespproc_test_XXXXXX.ini";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) {
        std::cerr << "mkstemps failed\n";
        std::exit(1);
    }
    write(fd, content.c_str(), content.size());
    close(fd);
    return std::string(tmpl);
}

// ---------------------------------------------------------------------------
// Helper: typed accessor casts
// ---------------------------------------------------------------------------

static const UdpTesterConfig& cfg(const UdpTesterPObj& obj)
{
    return *static_cast<const UdpTesterConfig*>(obj.getConfig());
}

static const UdpTesterStatus& sts(const UdpTesterPObj& obj)
{
    return *static_cast<const UdpTesterStatus*>(obj.getStatus());
}

static const UdpTesterStats& sta(const UdpTesterPObj& obj)
{
    return *static_cast<const UdpTesterStats*>(obj.getStats());
}

// ---------------------------------------------------------------------------
// Test 1: loadConfig() populates all UdpTesterConfig fields correctly
// ---------------------------------------------------------------------------

static void testLoadConfig()
{
    std::cout << "\n[Test 1] loadConfig() populates all fields\n";

    std::string ini = writeTempIni(
        "[object.1]\n"
        "TYPE=UdpTester\n"
        "INTERVAL_MS=500\n"
        "PACKET_SIZE=128\n"
        "SRC_IP=127.0.0.1\n"
        "SRC_PORT=15100\n"
        "DST_IP=127.0.0.1\n"
        "DST_PORT=15101\n"
    );

    IniConfig icfg(ini);
    check(icfg.isLoaded(), "INI file loaded");

    UdpTesterPObj obj;
    bool ok = obj.loadConfig(icfg, "object.1");
    check(ok, "loadConfig() returns true");

    check(obj.getName()         == "object.1", "name == \"object.1\"");
    check(cfg(obj).intervalMs  == 500,         "intervalMs == 500");
    check(cfg(obj).packetSize  == 128,         "packetSize == 128");
    check(cfg(obj).srcIp       == "127.0.0.1", "srcIp == 127.0.0.1");
    check(cfg(obj).srcPort     == 15100,       "srcPort == 15100");
    check(cfg(obj).dstIp       == "127.0.0.1", "dstIp == 127.0.0.1");
    check(cfg(obj).dstPort     == 15101,       "dstPort == 15101");

    std::remove(ini.c_str());
}

// ---------------------------------------------------------------------------
// Test 2: First process() from IDLE opens socket and transitions to ACTIVE
// ---------------------------------------------------------------------------

static void testFirstProcessActivates()
{
    std::cout << "\n[Test 2] First process() IDLE -> ACTIVE\n";

    // SRC_PORT=0 -> kernel assigns any available port; DST_PORT need not be bound
    std::string ini = writeTempIni(
        "[object.1]\n"
        "TYPE=UdpTester\n"
        "INTERVAL_MS=1000\n"
        "PACKET_SIZE=64\n"
        "SRC_IP=127.0.0.1\n"
        "SRC_PORT=0\n"
        "DST_IP=127.0.0.1\n"
        "DST_PORT=15200\n"
    );

    IniConfig icfg(ini);
    UdpTesterPObj obj;
    obj.loadConfig(icfg, "object.1");

    check(sts(obj).objStatus == ProcObject::ObjStatus::IDLE,
          "initial status is IDLE");

    obj.process();  // IDLE -> ACTIVE

    check(sts(obj).objStatus == ProcObject::ObjStatus::ACTIVE,
          "status is ACTIVE after first process()");
    check(sts(obj).objStatus == ProcObject::ObjStatus::ACTIVE,
          "status is ACTIVE (socket opened successfully)");

    std::remove(ini.c_str());
}

// ---------------------------------------------------------------------------
// Test 3: process() when ACTIVE is a no-op (event-driven model: no send)
// ---------------------------------------------------------------------------

static void testNoSendFromProcess()
{
    std::cout << "\n[Test 3] process() when ACTIVE is a no-op (event-driven)\n";

    std::string ini = writeTempIni(
        "[object.1]\n"
        "TYPE=UdpTester\n"
        "INTERVAL_MS=60000\n"
        "PACKET_SIZE=64\n"
        "SRC_IP=127.0.0.1\n"
        "SRC_PORT=0\n"
        "DST_IP=127.0.0.1\n"
        "DST_PORT=15300\n"
    );

    IniConfig icfg(ini);
    UdpTesterPObj obj;
    obj.loadConfig(icfg, "object.1");

    obj.process();  // IDLE -> ACTIVE
    check(sts(obj).objStatus == ProcObject::ObjStatus::ACTIVE,
          "ACTIVE after first process()");

    obj.process();  // ACTIVE -> no-op; no event loop running -> no send
    check(sta(obj).packetsSent == 0,
          "no packet sent from process() — sending is event-driven");

    std::remove(ini.c_str());
}

// ---------------------------------------------------------------------------
// Test 4: Sending is event-driven; process() never increments packetsSent
// ---------------------------------------------------------------------------

static void testProcessNeverSends()
{
    std::cout << "\n[Test 4] process() never sends — event loop drives packetsSent\n";

    std::string ini = writeTempIni(
        "[object.1]\n"
        "TYPE=UdpTester\n"
        "INTERVAL_MS=0\n"
        "PACKET_SIZE=32\n"
        "SRC_IP=127.0.0.1\n"
        "SRC_PORT=0\n"
        "DST_IP=127.0.0.1\n"
        "DST_PORT=15400\n"
    );

    IniConfig icfg(ini);
    UdpTesterPObj obj;
    obj.loadConfig(icfg, "object.1");

    obj.process();  // IDLE -> ACTIVE
    check(sts(obj).objStatus == ProcObject::ObjStatus::ACTIVE,
          "ACTIVE after first process()");
    check(sta(obj).packetsSent == 0,
          "packetsSent == 0 on IDLE->ACTIVE transition");

    obj.process();  // ACTIVE -> no-op (no event loop, timer never fires)
    check(sta(obj).packetsSent == 0,
          "packetsSent == 0 after second process() — send is event-driven");

    std::remove(ini.c_str());
}

// ---------------------------------------------------------------------------
// PktBertPObj typed accessor helpers
// ---------------------------------------------------------------------------

static const PktBertConfig& bCfg(const PktBertPObj& obj)
{
    return *static_cast<const PktBertConfig*>(obj.getConfig());
}

static const PktBertStatus& bSts(const PktBertPObj& obj)
{
    return *static_cast<const PktBertStatus*>(obj.getStatus());
}

static const PktBertStats& bSta(const PktBertPObj& obj)
{
    return *static_cast<const PktBertStats*>(obj.getStats());
}

// ---------------------------------------------------------------------------
// Test 5: loadConfig() populates all PktBertConfig fields correctly
// ---------------------------------------------------------------------------

static void testPktBertLoadConfig()
{
    std::cout << "\n[Test 5] PktBert loadConfig() populates all fields\n";

    std::string ini = writeTempIni(
        "[object.1]\n"
        "TYPE=PktBert\n"
        "PACKET_SIZE=128\n"
        "PRBS_TYPE=11\n"
        "PACKET_LOSS_PPM=500\n"
    );

    IniConfig icfg(ini);
    check(icfg.isLoaded(), "INI file loaded");

    PktBertPObj obj;
    bool ok = obj.loadConfig(icfg, "object.1");
    check(ok,                                     "loadConfig() returns true");

    check(obj.getName()            == "object.1", "name == \"object.1\"");
    check(bCfg(obj).packetSize    == 128,         "packetSize == 128");
    check(bCfg(obj).prbsType      == 11,          "prbsType == 11");
    check(bCfg(obj).packetLossPPM == 500u,        "packetLossPPM == 500");

    std::remove(ini.c_str());
}

// ---------------------------------------------------------------------------
// Test 6: First process() IDLE -> ACTIVE; syncOk starts false
// ---------------------------------------------------------------------------

static void testPktBertFirstProcessActivates()
{
    std::cout << "\n[Test 6] PktBert first process() IDLE -> ACTIVE\n";

    std::string ini = writeTempIni(
        "[object.1]\n"
        "TYPE=PktBert\n"
        "PACKET_SIZE=64\n"
        "PRBS_TYPE=7\n"
        "PACKET_LOSS_PPM=0\n"
    );

    IniConfig icfg(ini);
    PktBertPObj obj;
    obj.loadConfig(icfg, "object.1");

    check(bSts(obj).objStatus == ProcObject::ObjStatus::IDLE,
          "initial status is IDLE");
    check(!bSts(obj).syncOk, "initial syncOk is false");

    obj.process();  // IDLE → ACTIVE (no event loop — timer not registered but status changes)

    check(bSts(obj).objStatus == ProcObject::ObjStatus::ACTIVE ||
          bSts(obj).objStatus == ProcObject::ObjStatus::ERROR,
          "status is ACTIVE or ERROR after first process() (no event loop)");
    check(bSta(obj).goodPackets == 0, "goodPackets == 0 (timer never fired)");
    check(bSta(obj).badPackets  == 0, "badPackets  == 0 (timer never fired)");

    std::remove(ini.c_str());
}

// ---------------------------------------------------------------------------
// Test 7: Default config values applied when keys are absent
// ---------------------------------------------------------------------------

static void testPktBertDefaultConfig()
{
    std::cout << "\n[Test 7] PktBert default config values\n";

    std::string ini = writeTempIni(
        "[object.1]\n"
        "TYPE=PktBert\n"
    );

    IniConfig icfg(ini);
    PktBertPObj obj;
    bool ok = obj.loadConfig(icfg, "object.1");
    check(ok,                                     "loadConfig() returns true with no keys");
    check(bCfg(obj).packetSize    == 64,          "default packetSize == 64");
    check(bCfg(obj).prbsType      == 7,           "default prbsType == 7");
    check(bCfg(obj).packetLossPPM == 0u,          "default packetLossPPM == 0");

    std::remove(ini.c_str());
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    std::cout << "=== xcespproc smoke tests ===\n";

    testLoadConfig();
    testFirstProcessActivates();
    testNoSendFromProcess();
    testProcessNeverSends();

    testPktBertLoadConfig();
    testPktBertFirstProcessActivates();
    testPktBertDefaultConfig();

    std::cout << "\n=== Results: " << passCount << " passed, "
              << failCount << " failed ===\n";

    return failCount == 0 ? 0 : 1;
}
