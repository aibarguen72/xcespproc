/**
 * @file    CtrlLink.cpp
 * @brief   CtrlLink — event-driven TCP client to xcespserver control port
 * @project XCESP
 * @date    2026-03-10
 */

#include "CtrlLink.h"
#include "XCespProc.h"
#include "IniConfig.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

CtrlLink::CtrlLink(const std::string& host, int port, int procId,
                   int heartbeatMs, int retryMs,
                   XCespProc& app, LogManager& log, const std::string& logTag)
    : host_(host), port_(port), procId_(procId),
      heartbeatMs_(heartbeatMs), retryMs_(retryMs),
      app_(app), log_(log), logTag_(logTag + ":CtrlLink")
{}

CtrlLink::~CtrlLink()
{
    // Remove retry timer before calling disconnect so it is not rescheduled
    if (retryTimerId_ >= 0) {
        app_.removeTimer(retryTimerId_);
        retryTimerId_ = -1;
    }
    disconnect();
}

// ---------------------------------------------------------------------------
// connect  (public entry point — called once from XCespProc::init)
// ---------------------------------------------------------------------------

bool CtrlLink::connect()
{
    // Start repeating retry timer so reconnects happen automatically
    if (retryMs_ > 0)
        retryTimerId_ = app_.addTimer(retryMs_, &CtrlLink::retryTimerCallback, this, true);

    return doConnect();
}

// ---------------------------------------------------------------------------
// doConnect  (internal — called on first attempt and on every retry tick)
// ---------------------------------------------------------------------------

bool CtrlLink::doConnect()
{
    if (fd_ >= 0) return false;  // already connected

    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0) {
        log_.log(LOG_WARNING, logTag_,
                 "getaddrinfo failed for " + host_ + ":" + std::to_string(port_));
        return false;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        log_.log(LOG_WARNING, logTag_, "socket() failed");
        freeaddrinfo(res);
        return false;
    }

    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        log_.log(LOG_WARNING, logTag_,
                 "connect() failed: " + std::string(strerror(errno)));
        freeaddrinfo(res);
        ::close(fd);
        return false;
    }
    freeaddrinfo(res);

    fd_ = fd;
    recvBuf_.clear();

    log_.log(LOG_INFO, logTag_,
             "Connected to " + host_ + ":" + std::to_string(port_));

    // Register socket with event loop and start heartbeat timer
    app_.addSocket(fd_, &CtrlLink::socketCallback, this);

    if (heartbeatMs_ > 0)
        hbTimerId_ = app_.addTimer(heartbeatMs_, &CtrlLink::heartbeatCallback, this, true);

    // Send HELLO
    sendFrame("HELLO", "PROC_ID=" + std::to_string(procId_) + "\n");
    log_.log(LOG_DEBUG, logTag_, "Sent HELLO PROC_ID=" + std::to_string(procId_));

    return true;
}

// ---------------------------------------------------------------------------
// Frame codec
// ---------------------------------------------------------------------------

void CtrlLink::sendFrame(const std::string& verb, const std::string& body)
{
    if (fd_ < 0) return;
    std::string frame = verb + " " + std::to_string(body.size()) + "\n" + body + "\n";
    const char* ptr = frame.data();
    size_t rem = frame.size();
    while (rem > 0) {
        ssize_t n = ::send(fd_, ptr, rem, MSG_NOSIGNAL);
        if (n <= 0) { disconnect(); return; }
        ptr += static_cast<size_t>(n);
        rem -= static_cast<size_t>(n);
    }
}

bool CtrlLink::tryExtractFrame(Frame& out)
{
    size_t nl = recvBuf_.find('\n');
    if (nl == std::string::npos) return false;

    std::string header = recvBuf_.substr(0, nl);
    size_t sp = header.find(' ');
    if (sp == std::string::npos) {
        recvBuf_.erase(0, nl + 1);  // malformed header — discard
        return false;
    }

    std::string verb = header.substr(0, sp);
    size_t bodyLen;
    try { bodyLen = static_cast<size_t>(std::stoul(header.substr(sp + 1))); }
    catch (...) { recvBuf_.erase(0, nl + 1); return false; }

    size_t needed = nl + 1 + bodyLen + 1;
    if (recvBuf_.size() < needed) return false;

    out.verb = std::move(verb);
    out.body = recvBuf_.substr(nl + 1, bodyLen);
    recvBuf_.erase(0, needed);
    return true;
}

// ---------------------------------------------------------------------------
// Readable callback
// ---------------------------------------------------------------------------

void CtrlLink::onReadable()
{
    char buf[4096];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n <= 0) {
        log_.log(LOG_WARNING, logTag_,
                 "Server disconnected" +
                 std::string(retryMs_ > 0
                     ? " — retrying in " + std::to_string(retryMs_ / 1000) + " s"
                     : ""));
        disconnect();
        return;
    }
    recvBuf_.append(buf, static_cast<size_t>(n));

    Frame frame;
    while (tryExtractFrame(frame)) {
        handleFrame(frame);
        if (fd_ < 0) return;  // disconnect() was called inside handleFrame
    }
}

// ---------------------------------------------------------------------------
// Frame dispatch
// ---------------------------------------------------------------------------

void CtrlLink::handleFrame(const Frame& f)
{
    if      (f.verb == "RESET")       handleReset(f.body);
    else if (f.verb == "DEPLOY")      handleDeploy(f.body);
    else if (f.verb == "REMOVE")      handleRemove(f.body);
    else if (f.verb == "STATUS_POLL") handleStatusPoll(f.body);
    else if (f.verb == "GOODBYE")     disconnect();
    else {
        log_.log(LOG_DEBUG, logTag_, "Ignoring verb: " + f.verb);
    }
}

void CtrlLink::handleReset(const std::string& /*body*/)
{
    log_.log(LOG_INFO, logTag_, "RESET received — clearing all objects");
    app_.clearAllObjects();
    sendFrame("ACK", "CMD=RESET\n");
}

void CtrlLink::handleDeploy(const std::string& body)
{
    IniConfig ini;
    ini.loadFromString(body);
    int count = 0;
    for (const auto& section : ini.getSections()) {
        if (app_.deployObjectFromIni(ini, section))
            ++count;
    }
    log_.log(LOG_INFO, logTag_,
             "DEPLOY received: " + std::to_string(count) + " object(s) deployed");
    sendFrame("ACK", "CMD=DEPLOY\n");
}

void CtrlLink::handleRemove(const std::string& body)
{
    auto kv = parseKvBody(body);
    const std::string& name = kv["NAME"];
    if (name.empty()) {
        log_.log(LOG_WARNING, logTag_, "REMOVE: missing NAME");
        return;
    }
    bool removed = app_.removeProcObject(name);
    log_.log(LOG_INFO, logTag_,
             "REMOVE NAME=" + name + (removed ? " — queued" : " — not found"));
}

void CtrlLink::handleStatusPoll(const std::string& body)
{
    auto kv = parseKvBody(body);
    const std::string& reqId = kv["REQ_ID"];
    log_.log(LOG_DEBUG, logTag_, "STATUS_POLL REQ_ID=" + reqId);
    std::string report = app_.buildStatusReportJson();
    sendFrame("STATUS_REPORT", report);
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

void CtrlLink::onHeartbeat()
{
    sendFrame("HEARTBEAT", "PROC_ID=" + std::to_string(procId_) + "\n");
    log_.log(LOG_DEBUG, logTag_, "Sent HEARTBEAT");
}

// ---------------------------------------------------------------------------
// Retry timer
// ---------------------------------------------------------------------------

void CtrlLink::onRetryTimer()
{
    if (fd_ >= 0) return;  // already connected — nothing to do
    log_.log(LOG_DEBUG, logTag_,
             "Retrying connection to " + host_ + ":" + std::to_string(port_));
    doConnect();
}

// ---------------------------------------------------------------------------
// disconnect  (leaves retry timer running for automatic reconnect)
// ---------------------------------------------------------------------------

void CtrlLink::disconnect()
{
    if (hbTimerId_ >= 0) {
        app_.removeTimer(hbTimerId_);
        hbTimerId_ = -1;
    }
    if (fd_ >= 0) {
        app_.removeSocket(fd_);
        ::close(fd_);
        fd_ = -1;
        log_.log(LOG_INFO, logTag_, "Disconnected");
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::unordered_map<std::string,std::string>
CtrlLink::parseKvBody(const std::string& body)
{
    std::unordered_map<std::string,std::string> result;
    std::istringstream ss(body);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        result[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Static callbacks
// ---------------------------------------------------------------------------

void CtrlLink::socketCallback(int /*fd*/, void* userData)
{
    static_cast<CtrlLink*>(userData)->onReadable();
}

void CtrlLink::heartbeatCallback(int /*id*/, void* userData)
{
    static_cast<CtrlLink*>(userData)->onHeartbeat();
}

void CtrlLink::retryTimerCallback(int /*id*/, void* userData)
{
    static_cast<CtrlLink*>(userData)->onRetryTimer();
}
