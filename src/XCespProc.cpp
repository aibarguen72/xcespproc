/**
 * @file    XCespProc.cpp
 * @brief   XCespProc — XCESP processing application implementation
 * @project XCESP
 * @date    2026-02-19
 */

#include "XCespProc.h"
#include "CtrlLink.h"

#include "ConsoleWriter.h"
#include "FileWriter.h"
#include "SyslogWriter.h"
#include "UdpTesterPObj.h"
#include "PktBertPObj.h"
#include "LinkObject.h"
#include "PduLinkObject.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <unistd.h>
#include <vector>

#ifndef PRJNAME
#define PRJNAME "xcespproc"
#endif
#ifndef PRJVERSION
#define PRJVERSION "0.0.0"
#endif

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

XCespProc::XCespProc(int argc, char* argv[])
    : argConfig(argc, argv)
{
    argConfig.addOption('c', "config",          "Path to INI configuration file");
    argConfig.addOption('i', "id",              "Process instance ID shown in banner and log tag (default 1)");
    argConfig.addOption('p', "local-port",      "Local syslog forwarding port (overrides LOG_LOCAL_PORT)");
    argConfig.addOption('s', "signal-interval", "Seconds between SIGUSR1 heartbeats to parent (default 0-disabled)");
    argConfig.addFlag  ('v', "verbose",         "Enable verbose (DEBUG) output on all writers");
    argConfig.addFlag  ('h', "help",            "Show this help message and exit");
}

XCespProc::~XCespProc()
{
    if (procThread) {
        procThread->signal();
    }
    endApplication();
    delete iniConfig;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool XCespProc::init()
{
    // 1. Bootstrap console writer; parse args first so the ID is known for the banner
    ConsoleWriter bootstrap(true, true);
    bootstrap.setMinLevel(LOG_INFO);
    logManager.addWriter(&bootstrap);

    if (!argConfig.parse()) {
        logManager.log(LOG_FATAL, PRJNAME, "Argument parsing failed");
        std::cout << std::string("Arguments:\n");
        std::cout << argConfig.getHelp();
        logManager.removeWriter(&bootstrap);
        return false;
    }

    if (argConfig.hasFlag('h')) {
        std::cout << std::string("Arguments:\n");
        std::cout << argConfig.getHelp();
        logManager.removeWriter(&bootstrap);
        return false;
    }

    verbose    = argConfig.hasFlag('v');
    configFile = argConfig.getValue('c', std::string("xcespproc.ini"));

    try { procId = std::stoi(argConfig.getValue('i', std::string("1"))); } catch (...) {}
    if (procId < 1) procId = 1;
    logTag = std::string(PRJNAME) + "-" + std::to_string(procId);

    // 2. Banner — shows logTag and version
    printBanner();

    // 3. Load INI
    iniConfig = new IniConfig(configFile);
    if (!iniConfig->isLoaded()) {
        logManager.log(LOG_FATAL, logTag,
                       "Cannot load configuration file: " + configFile);
        logManager.removeWriter(&bootstrap);
        return false;
    }

    // 4. Switch to configured writers
    setupLogging();
    logManager.removeWriter(&bootstrap);

    logManager.log(LOG_INFO, logTag, logTag + " v" + PRJVERSION + " starting");

    // 5. Initialise EvApplication
    if (!initApplication()) {
        logManager.log(LOG_FATAL, logTag, "initApplication() failed");
        return false;
    }

    // 6. SIGUSR1 heartbeat to parent process. Disabled by default
    int signalIntervalSec = 0;
    auto sArg = argConfig.getValue('s');
    if (sArg.has_value()) {
        try { signalIntervalSec = std::stoi(sArg.value()); } catch (...) {}
    }
    if (signalIntervalSec > 0) {
        originalPpid_ = getppid();
        setParentHeartbeat(signalIntervalSec * 1000);
        logManager.log(LOG_DEBUG, logTag,
                       "SIGUSR1 heartbeat to parent every " +
                       std::to_string(signalIntervalSec) + " s"
                       + " (parent PID=" + std::to_string(originalPpid_) + ")");
    }

    // 7. Read scheduler config from [PROC]
    hbIntervalMs_   = static_cast<int>(iniConfig->getValueInteger("PROC", "PROC_HB_INTERVAL",        100));
    maxObjsPerHb_   = static_cast<int>(iniConfig->getValueInteger("PROC", "PROC_MAX_OBJECTS_PER_HB",  100));
    hbIntervalMult_ = static_cast<int>(iniConfig->getValueInteger("PROC", "PROC_HB_INTERVAL_MULT",    100));
    if (hbIntervalMs_   < 1) hbIntervalMs_   = 1;
    if (maxObjsPerHb_   < 1) maxObjsPerHb_   = 1;
    if (hbIntervalMult_ < 1) hbIntervalMult_ = 1;
    logManager.log(LOG_DEBUG, logTag,
                   "Scheduler: HB=" + std::to_string(hbIntervalMs_) + " ms, " +
                   "max=" + std::to_string(maxObjsPerHb_) + "/tick, " +
                   "mult=" + std::to_string(hbIntervalMult_));

    // 8. Main thread 10 s heartbeat timer
    addTimer(10000, &XCespProc::mainTimerCallback, this, true);

    // 9. Create processing thread
    procThread = addThread();

    // 10. Register processing tick timer (PROC_HB_INTERVAL) and start thread
    procThread->addTimer(hbIntervalMs_, &XCespProc::processingTimerCallback, this, true);
    procThread->setBackend(EvBackend::Epoll);
    procThread->start();

    logManager.log(LOG_INFO, logTag,
                   "Processing thread started (HB=" + std::to_string(hbIntervalMs_) +
                   " ms, mult=" + std::to_string(hbIntervalMult_) + ")");

    // 11. CtrlLink or INI-based object loading
    std::string ctrlHost    = iniConfig->getValue("PROC", "CTRL_HOST", "");
    int ctrlPort            = static_cast<int>(iniConfig->getValueInteger("PROC", "CTRL_PORT",           9900));
    int ctrlRetryMs         = static_cast<int>(iniConfig->getValueInteger("PROC", "CTRL_RETRY_INTERVAL", 30000));
    if (ctrlRetryMs < 0) ctrlRetryMs = 0;

    if (!ctrlHost.empty()) {
        int hbMs = hbIntervalMs_ * hbIntervalMult_;
        ctrlLink_ = std::make_unique<CtrlLink>(ctrlHost, ctrlPort, procId, hbMs, ctrlRetryMs,
                                               *this, logManager, logTag);
        if (!ctrlLink_->connect()) {
            std::string retryMsg = ctrlRetryMs > 0
                ? " — retrying every " + std::to_string(ctrlRetryMs / 1000) + " s"
                : " — no retry configured";
            logManager.log(LOG_WARNING, logTag,
                           "CtrlLink: could not connect to " + ctrlHost +
                           ":" + std::to_string(ctrlPort) + retryMsg);
        }
    } else {
        // Fallback: load objects from [object.N] INI sections
        loadTimerId_ = addTimer(hbIntervalMs_, &XCespProc::loadBatchTimerCallback, this, true);
    }

    return true;
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

void XCespProc::run()
{
    logManager.log(LOG_INFO, logTag, "Entering event loop");
    runLoop();
    logManager.log(LOG_INFO, logTag, "Event loop exited");
}

// ---------------------------------------------------------------------------
// setupLogging
// ---------------------------------------------------------------------------

void XCespProc::setupLogging()
{
    LogLevel consoleLevel = parseLogLevel(iniConfig->getValue("PROC", "LOG_CONSOLE_LEVEL", "Info"));
    LogLevel fileLevel    = parseLogLevel(iniConfig->getValue("PROC", "LOG_FILE_LEVEL",    "Info"));

    // -v flag forces DEBUG on all writers
    if (verbose) {
        consoleLevel = LOG_DEBUG;
        fileLevel    = LOG_DEBUG;
    }

    // --- Console writer ---
    if (iniConfig->getValueBoolean("PROC", "LOG_CONSOLE_ENABLED", true)) {
        bool useStderr = iniConfig->getValueBoolean("PROC", "LOG_CONSOLE_STDERR", false);
        auto* console  = new ConsoleWriter(true, useStderr);
        console->setMinLevel(consoleLevel);
        logManager.addWriter(console);
    }

    // --- File writer ---
    if (iniConfig->getValueBoolean("PROC", "LOG_FILE_ENABLED", false)) {
        std::string logFile = iniConfig->getValue("PROC", "LOG_FILE", "/var/log/xcespproc.log");
        bool cleanStart     = iniConfig->getValueBoolean("PROC", "LOG_FILE_CLEANSTART", false);
        auto* fileWriter    = new FileWriter(logFile, cleanStart);
        fileWriter->setMinLevel(fileLevel);
        logManager.addWriter(fileWriter);
    }

    // --- Local syslog forwarding to 127.0.0.1:LOG_LOCAL_PORT (xcespwdog listener) ---
    // Priority: -p argument > LOG_LOCAL_PORT ini value > built-in default (1514)
    if (iniConfig->getValueBoolean("PROC", "LOG_LOCAL_ENABLE", true)) {
        localPort = static_cast<int>(iniConfig->getValueInteger("PROC", "LOG_LOCAL_PORT", 1514));
        auto portArg = argConfig.getValue('p');
        if (portArg.has_value()) {
            try { localPort = std::stoi(portArg.value()); } catch (...) {}
        }

        LogLevel localLevel = parseLogLevel(iniConfig->getValue("PROC", "LOG_LOCAL_LEVEL", "Info"));
        if (verbose) localLevel = LOG_DEBUG;

        auto* localSyslog = new SyslogWriter("127.0.0.1", localPort);
        localSyslog->setMinLevel(localLevel);
        logManager.addWriter(localSyslog);
        logManager.log(LOG_DEBUG, logTag,
                       "Forwarding logs to 127.0.0.1:" + std::to_string(localPort));
    }
}

// ---------------------------------------------------------------------------
// loadObjectsBatch  (main thread only — called from loadBatchTimerCallback)
// ---------------------------------------------------------------------------

void XCespProc::loadObjectsBatch()
{
    int loaded = 0;

    while (loaded < maxObjsPerHb_) {
        std::string section = "object." + std::to_string(nextSectionIdx_);
        auto typeVal = iniConfig->getValue(section, "TYPE");

        if (!typeVal.has_value() || typeVal->empty()) {
            // Reached end of configured sections — cancel load timer
            if (loadTimerId_ >= 0) {
                removeTimer(loadTimerId_);
                loadTimerId_ = -1;
            }
            logManager.log(LOG_INFO, logTag,
                           "Object loading complete: " +
                           std::to_string(procObjects.size()) + " object(s)");
            return;
        }

        const std::string& type = typeVal.value();
        std::unique_ptr<ProcObject> obj;

        if (type == "UdpTester") {
            obj = std::make_unique<UdpTesterPObj>();
        } else if (type == "PktBert") {
            obj = std::make_unique<PktBertPObj>();
        } else {
            logManager.log(LOG_WARNING, logTag,
                           section + ": unknown TYPE \"" + type + "\" — skipping");
            ++nextSectionIdx_;
            continue;
        }

        if (!obj->loadConfig(*iniConfig, section)) {
            logManager.log(LOG_WARNING, logTag,
                           section + ": loadConfig() failed — skipping");
            ++nextSectionIdx_;
            continue;
        }

        obj->init(procThread->getLoop(), logManager, logTag);
        obj->setLinkRegistry(this);

        logManager.log(LOG_DEBUG, logTag,
                       "Loaded object: " + obj->getName() + " (type=" + type + ")");

        {
            std::lock_guard<std::mutex> lock(procMutex_);
            procObjects.push_back(std::move(obj));
        }

        ++nextSectionIdx_;
        ++loaded;
    }
}

// ---------------------------------------------------------------------------
// deployObjectFromIni  (main thread — called from CtrlLink on DEPLOY)
// ---------------------------------------------------------------------------

bool XCespProc::deployObjectFromIni(IniConfig& ini, const std::string& section)
{
    auto typeVal = ini.getValue(section, "TYPE");
    if (!typeVal.has_value() || typeVal->empty()) {
        logManager.log(LOG_WARNING, logTag,
                       "deployObjectFromIni: no TYPE in section \"" + section + "\"");
        return false;
    }

    const std::string& type = typeVal.value();
    std::unique_ptr<ProcObject> obj;

    if (type == "UdpTester") {
        obj = std::make_unique<UdpTesterPObj>();
    } else if (type == "PktBert") {
        obj = std::make_unique<PktBertPObj>();
    } else {
        logManager.log(LOG_WARNING, logTag,
                       "deployObjectFromIni: unknown TYPE \"" + type + "\" in \"" + section + "\"");
        return false;
    }

    if (!obj->loadConfig(ini, section)) {
        logManager.log(LOG_WARNING, logTag,
                       "deployObjectFromIni: loadConfig() failed for \"" + section + "\"");
        return false;
    }

    obj->init(procThread->getLoop(), logManager, logTag);
    obj->setLinkRegistry(this);

    logManager.log(LOG_DEBUG, logTag,
                   "Deployed object: " + obj->getName() + " (type=" + type + ")");

    std::lock_guard<std::mutex> lock(procMutex_);
    procObjects.push_back(std::move(obj));
    return true;
}

// ---------------------------------------------------------------------------
// clearAllObjects  (main thread — called from CtrlLink on RESET)
// ---------------------------------------------------------------------------

void XCespProc::clearAllObjects()
{
    std::lock_guard<std::mutex> lock(procMutex_);
    logManager.log(LOG_INFO, logTag,
                   "Clearing all " + std::to_string(procObjects.size()) + " objects");
    for (const auto& obj : procObjects)
        pendingRemovals_.push_back(obj->getName());
}

// ---------------------------------------------------------------------------
// buildStatusReportJson  (main thread — called from CtrlLink on STATUS_POLL)
// ---------------------------------------------------------------------------

std::string XCespProc::buildStatusReportJson()
{
    std::lock_guard<std::mutex> lock(procMutex_);
    std::string json = "{\"objects\":[";
    bool first = true;
    for (const auto& obj : procObjects) {
        std::string s = obj->buildStatusJson();
        if (s.empty()) continue;
        if (!first) json += ',';
        json += s;
        first = false;
    }
    json += "]}";
    return json;
}

// ---------------------------------------------------------------------------
// removeProcObject  (thread-safe; called from any thread)
// ---------------------------------------------------------------------------

bool XCespProc::removeProcObject(const std::string& name)
{
    std::lock_guard<std::mutex> lock(procMutex_);
    auto it = std::find_if(procObjects.begin(), procObjects.end(),
                           [&name](const std::unique_ptr<ProcObject>& p) {
                               return p->getName() == name;
                           });
    if (it == procObjects.end())
        return false;

    pendingRemovals_.push_back(name);
    logManager.log(LOG_DEBUG, logTag, "Queued removal of object: " + name);
    return true;
}

// ---------------------------------------------------------------------------
// LinkRegistry interface implementation
// ---------------------------------------------------------------------------

bool XCespProc::registerLink(const std::string& name,
                             ProcObject*         obj,
                             LinkRole            role,
                             const std::string&  linkClass)
{
    std::lock_guard<std::mutex> lock(linksMutex_);

    auto it = links_.find(name);
    if (it == links_.end()) {
        // Link does not exist — create using factory
        std::unique_ptr<LinkObject> newLink;

        if (linkClass == "PDU") {
            newLink = std::make_unique<PduLinkObject>(name);
        } else {
            logManager.log(LOG_ERROR, logTag,
                           "registerLink: unknown link class \"" + linkClass +
                           "\" for link \"" + name + "\"");
            return false;
        }

        auto ins = links_.emplace(name, std::move(newLink));
        it = ins.first;
    } else {
        // Link already exists — verify the class matches
        if (it->second->getLinkClass() != linkClass) {
            logManager.log(LOG_ERROR, logTag,
                           "registerLink: link \"" + name + "\" is class \"" +
                           it->second->getLinkClass() + "\", caller requested \"" +
                           linkClass + "\"");
            return false;
        }
    }

    bool ok = it->second->registerObject(obj, role);
    if (!ok) {
        logManager.log(LOG_WARNING, logTag,
                       "registerLink: role already taken in link \"" + name + "\"");
    } else {
        const std::string roleStr = (role == LinkRole::ROLE_MASTER) ? "MASTER" : "SLAVE";
        logManager.log(LOG_DEBUG, logTag,
                       "registerLink: " + obj->getName() +
                       " registered as " + roleStr +
                       " in link \"" + name + "\" (class=" + linkClass + ")" +
                       " — state=" +
                       (it->second->getState() == LinkObject::LinkState::UP ? "UP" : "INCOMPLETE"));
    }
    return ok;
}

void XCespProc::unregisterLink(const std::string& name, ProcObject* obj)
{
    std::lock_guard<std::mutex> lock(linksMutex_);

    auto it = links_.find(name);
    if (it == links_.end()) {
        logManager.log(LOG_WARNING, logTag,
                       "unregisterLink: link \"" + name + "\" not found");
        return;
    }

    it->second->unregisterObject(obj);

    logManager.log(LOG_DEBUG, logTag,
                   "unregisterLink: removed object from link \"" + name + "\"" +
                   " — state=" +
                   (it->second->getState() == LinkObject::LinkState::UP ? "UP" : "INCOMPLETE"));
    // Note: the LinkObject is retained in links_ to allow re-registration.
}

LinkObject* XCespProc::getLink(const std::string& name)
{
    std::lock_guard<std::mutex> lock(linksMutex_);

    auto it = links_.find(name);
    if (it == links_.end())
        return nullptr;
    return it->second.get();
}

// ---------------------------------------------------------------------------
// mainTick / processingTick
// ---------------------------------------------------------------------------

void XCespProc::mainTick()
{
    logManager.log(LOG_DEBUG, logTag, "heartbeat");

    // If parent heartbeat is enabled, check that the original parent is still alive.
    // getppid() returns 1 (init/systemd) when the original parent has died.
    if (originalPpid_ != 0 && getppid() != originalPpid_) {
        logManager.log(LOG_FATAL, logTag,
                       "Parent process changed (was PID=" + std::to_string(originalPpid_) +
                       ", now PPID=" + std::to_string(getppid()) + ") — exiting");
        stopLoop();
    }
}

void XCespProc::processingTick()
{
    // Collect up to maxObjsPerHb_ objects not yet processed in the current iteration.
    // Lock briefly to snapshot pointers; process() is called outside the lock.
    std::vector<ProcObject*> batch;
    batch.reserve(static_cast<size_t>(maxObjsPerHb_));

    {
        std::lock_guard<std::mutex> lock(procMutex_);

        // Drain deferred removals before building the batch so no raw pointer
        // in the batch can refer to a destroyed object.
        for (const auto& name : pendingRemovals_) {
            auto it = std::find_if(procObjects.begin(), procObjects.end(),
                                   [&name](const std::unique_ptr<ProcObject>& p) {
                                       return p->getName() == name;
                                   });
            if (it != procObjects.end()) {
                logManager.log(LOG_INFO, logTag, "Removed object: " + name);
                procObjects.erase(it);
            }
        }
        pendingRemovals_.clear();

        for (auto& obj : procObjects) {
            if (static_cast<int>(batch.size()) >= maxObjsPerHb_) break;
            if (obj->getProcessIteration() != currentIteration_) {
                obj->setProcessIteration(currentIteration_);
                batch.push_back(obj.get());
            }
        }
    }

    for (ProcObject* obj : batch) {
        logManager.log(LOG_DEBUG, logTag,"Process call for object "+obj->getName()+
            " Iter:"+std::to_string(obj->getProcessIteration()));
        obj->process();
    }

    // Advance round counter; start a new iteration when the round is complete
    ++processCounter_;
    if (processCounter_ >= hbIntervalMult_) {
        bool allCovered = true;
        {
            std::lock_guard<std::mutex> lock(procMutex_);
            for (auto& obj : procObjects) {
                if (obj->getProcessIteration() != currentIteration_) {
                    allCovered = false;
                    break;
                }
            }
        }
        if (allCovered) {
            ++currentIteration_;
            if (currentIteration_ == 0)
                currentIteration_ = 1;   // 0 is reserved as "never processed"
        }
        processCounter_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void XCespProc::printBanner() const
{
    const std::string name    = logTag + " v" + PRJVERSION;
    const std::string purpose = "XCESP Processing Application";
    const int width = static_cast<int>(std::max(name.size(), purpose.size())) + 4;
    const std::string bar(width, '-');

    std::cout << "+" << bar << "+\n"
              << "|  " << name    << std::string(width - 2 - name.size(),    ' ') << "|\n"
              << "|  " << purpose << std::string(width - 2 - purpose.size(), ' ') << "|\n"
              << "+" << bar << "+\n" << std::flush;
}

LogLevel XCespProc::parseLogLevel(const std::string& s)
{
    if (s == "Debug"   || s == "debug"   || s == "DEBUG")   return LOG_DEBUG;
    if (s == "Info"    || s == "info"    || s == "INFO")     return LOG_INFO;
    if (s == "Warning" || s == "warning" || s == "WARNING")  return LOG_WARNING;
    if (s == "Error"   || s == "error"   || s == "ERROR")    return LOG_ERROR;
    if (s == "Fatal"   || s == "fatal"   || s == "FATAL")    return LOG_FATAL;
    return LOG_INFO;
}

void XCespProc::mainTimerCallback(int id, void* userData)
{
    (void)id;
    static_cast<XCespProc*>(userData)->mainTick();
}

void XCespProc::processingTimerCallback(int id, void* userData)
{
    (void)id;
    static_cast<XCespProc*>(userData)->processingTick();
}

void XCespProc::loadBatchTimerCallback(int id, void* userData)
{
    (void)id;
    static_cast<XCespProc*>(userData)->loadObjectsBatch();
}
