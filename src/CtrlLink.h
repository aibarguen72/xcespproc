/**
 * @file    CtrlLink.h
 * @brief   CtrlLink — event-driven TCP client to xcespserver control port
 * @project XCESP
 * @date    2026-03-10
 */

#ifndef CTRLLINK_H
#define CTRLLINK_H

#include <string>
#include <unordered_map>
#include "LogManager.h"

// Forward declarations — full headers included in CtrlLink.cpp
class XCespProc;

/**
 * @brief  TCP client that implements the xcespctrl protocol toward xcespserver.
 *
 * Protocol flow (xcespproc side):
 *   connect → send HELLO → wait for RESET + DEPLOY frames → send ACK
 *   receive STATUS_POLL → send STATUS_REPORT
 *   send periodic HEARTBEAT
 *   receive REMOVE → remove object by name
 *
 * Wire format (same as ProcClient on the server):
 *   "VERB BODYLEN\n<body>\n"
 *
 * CtrlLink runs entirely on the main thread event loop (addSocket / addTimer).
 * Object creation and removal is delegated to XCespProc.
 *
 * Reconnect behaviour:
 *   If retryMs > 0, a repeating retry timer is started immediately.  It fires
 *   every retryMs milliseconds and calls doConnect() only when not currently
 *   connected (fd_ < 0).  On disconnect() the socket/heartbeat timer are
 *   removed but the retry timer keeps running, so reconnect is automatic.
 */
class CtrlLink {
public:
    /**
     * @param  host         xcespserver hostname or IP
     * @param  port         xcespserver control port
     * @param  procId       This process instance ID (sent in HELLO / HEARTBEAT)
     * @param  heartbeatMs  Interval between HEARTBEAT frames in ms
     * @param  retryMs      Reconnect retry interval in ms (0 = no retry)
     * @param  app          XCespProc that owns this CtrlLink
     * @param  log          Shared log manager
     * @param  logTag       Log source tag (e.g. "xcespproc-1")
     */
    CtrlLink(const std::string& host, int port, int procId,
             int heartbeatMs, int retryMs,
             XCespProc& app, LogManager& log, const std::string& logTag);

    ~CtrlLink();

    /**
     * @brief  Start the retry timer and attempt the first connection immediately.
     *
     * If the initial attempt fails and retryMs > 0, the retry timer will
     * reattempt at the configured interval.  Returns true if the first attempt
     * succeeded.
     */
    bool connect();

private:
    std::string host_;
    int         port_;
    int         procId_;
    int         heartbeatMs_;
    int         retryMs_;
    XCespProc&  app_;
    LogManager& log_;
    std::string logTag_;

    int         fd_            = -1;
    std::string recvBuf_;
    int         hbTimerId_    = -1;
    int         retryTimerId_ = -1;

    struct Frame { std::string verb, body; };

    // Attempt one TCP connect; returns true on success.
    // No-op (returns false) when already connected (fd_ >= 0).
    bool doConnect();

    bool tryExtractFrame(Frame& out);
    void sendFrame(const std::string& verb, const std::string& body);

    void handleFrame(const Frame& f);
    void handleReset(const std::string& body);
    void handleDeploy(const std::string& body);
    void handleRemove(const std::string& body);
    void handleStatusPoll(const std::string& body);

    void onReadable();
    void onHeartbeat();
    void onRetryTimer();

    // Tear down socket and heartbeat timer; retry timer is left running.
    void disconnect();

    static std::unordered_map<std::string,std::string> parseKvBody(const std::string& body);

    static void socketCallback(int fd, void* userData);
    static void heartbeatCallback(int id, void* userData);
    static void retryTimerCallback(int id, void* userData);
};

#endif // CTRLLINK_H
