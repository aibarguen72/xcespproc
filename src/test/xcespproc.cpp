/**
 * @file    xcespproc.cpp
 * @brief   Smoke tests for xcespproc processing objects
 * @project XCESP
 * @date    2026-02-19
 */

#include "UdpTesterPObj.h"
#include "IniConfig.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
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

    IniConfig cfg(ini);
    check(cfg.isLoaded(), "INI file loaded");

    UdpTesterPObj obj;
    bool ok = obj.loadConfig(cfg, "object.1");
    check(ok, "loadConfig() returns true");

    check(obj.getName()          == "object.1", "name == \"object.1\"");
    check(obj.getConfig().intervalMs  == 500,   "intervalMs == 500");
    check(obj.getConfig().packetSize  == 128,   "packetSize == 128");
    check(obj.getConfig().srcIp       == "127.0.0.1", "srcIp == 127.0.0.1");
    check(obj.getConfig().srcPort     == 15100, "srcPort == 15100");
    check(obj.getConfig().dstIp       == "127.0.0.1", "dstIp == 127.0.0.1");
    check(obj.getConfig().dstPort     == 15101, "dstPort == 15101");

    std::remove(ini.c_str());
}

// ---------------------------------------------------------------------------
// Test 2: First process() from IDLE opens socket and transitions to ACTIVE
// ---------------------------------------------------------------------------

static void testFirstProcessActivates()
{
    std::cout << "\n[Test 2] First process() IDLE -> ACTIVE\n";

    // SRC_PORT=0 → kernel assigns any available port; DST_PORT need not be bound
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

    IniConfig cfg(ini);
    UdpTesterPObj obj;
    obj.loadConfig(cfg, "object.1");

    check(obj.getStatus().objStatus == ProcObject::ObjStatus::IDLE,
          "initial status is IDLE");

    obj.process();  // IDLE → ACTIVE

    check(obj.getStatus().objStatus == ProcObject::ObjStatus::ACTIVE,
          "status is ACTIVE after first process()");
    check(obj.getStatus().socketFd >= 0,
          "socket is open (fd >= 0)");

    std::remove(ini.c_str());
}

// ---------------------------------------------------------------------------
// Test 3: Second process() within a long interval → no packet sent
// ---------------------------------------------------------------------------

static void testNoSendWithinInterval()
{
    std::cout << "\n[Test 3] Second process() within interval -> no packet sent\n";

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

    IniConfig cfg(ini);
    UdpTesterPObj obj;
    obj.loadConfig(cfg, "object.1");

    obj.process();  // IDLE → ACTIVE, records lastSendTime_
    check(obj.getStatus().objStatus == ProcObject::ObjStatus::ACTIVE,
          "ACTIVE after first process()");

    obj.process();  // elapsed << 60000 ms → no send
    check(obj.getStats().packetsSent == 0,
          "no packet sent within interval (packetsSent == 0)");

    std::remove(ini.c_str());
}

// ---------------------------------------------------------------------------
// Test 4: intervalMs=0 → immediate send on second process() call
// ---------------------------------------------------------------------------

static void testImmediateSendZeroInterval()
{
    std::cout << "\n[Test 4] intervalMs=0 -> immediate send on second process()\n";

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

    IniConfig cfg(ini);
    UdpTesterPObj obj;
    obj.loadConfig(cfg, "object.1");

    obj.process();  // IDLE → ACTIVE
    check(obj.getStatus().objStatus == ProcObject::ObjStatus::ACTIVE,
          "ACTIVE after first process()");
    check(obj.getStats().packetsSent == 0,
          "no packet sent on IDLE->ACTIVE transition");

    obj.process();  // elapsed (0 us) >= 0 ms → send immediately
    check(obj.getStats().packetsSent == 1,
          "one packet sent on second process() with intervalMs=0");

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
    testNoSendWithinInterval();
    testImmediateSendZeroInterval();

    std::cout << "\n=== Results: " << passCount << " passed, "
              << failCount << " failed ===\n";

    return failCount == 0 ? 0 : 1;
}
