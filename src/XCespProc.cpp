/**
 * @file    XCespProc.cpp
 * @brief   XCespProc — XCESP processing application implementation
 * @project XCESP
 * @date    2026-02-19
 */

#include "XCespProc.h"

#include "ConsoleWriter.h"
#include "FileWriter.h"
#include "SyslogWriter.h"
#include "UdpTesterPObj.h"

#include <algorithm>
#include <iostream>

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
    argConfig.addOption('s', "signal-interval", "Seconds between SIGUSR1 heartbeats to parent (default 20)");
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

    // 6. SIGUSR1 heartbeat to parent process
    int signalIntervalSec = 20;
    auto sArg = argConfig.getValue('s');
    if (sArg.has_value()) {
        try { signalIntervalSec = std::stoi(sArg.value()); } catch (...) {}
    }
    if (signalIntervalSec > 0) {
        setParentHeartbeat(signalIntervalSec * 1000);
        logManager.log(LOG_DEBUG, logTag,
                       "SIGUSR1 heartbeat to parent every " +
                       std::to_string(signalIntervalSec) + " s");
    }

    // 7. Main thread 10 s heartbeat timer
    addTimer(10000, &XCespProc::mainTimerCallback, this, true);

    // 8. Load processing objects from [object.N] sections
    loadObjects();

    // 9-11. Create processing thread, register timer, start thread
    procThread = addThread();
    procThread->addTimer(100, &XCespProc::processingTimerCallback, this, true);
    procThread->start();

    logManager.log(LOG_INFO, logTag,
                   "Processing thread started (" +
                   std::to_string(procObjects.size()) +
                   " object(s), 100 ms tick)");

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
// loadObjects
// ---------------------------------------------------------------------------

void XCespProc::loadObjects()
{
    for (int i = 1; ; ++i) {
        std::string section = "object." + std::to_string(i);
        auto typeVal = iniConfig->getValue(section, "TYPE");
        if (!typeVal.has_value() || typeVal->empty()) break;

        const std::string& type = typeVal.value();

        std::unique_ptr<ProcObject> obj;

        if (type == "UdpTester") {
            obj = std::make_unique<UdpTesterPObj>();
        } else {
            logManager.log(LOG_WARNING, logTag,
                           section + ": unknown TYPE \"" + type + "\" — skipping");
            continue;
        }

        if (!obj->loadConfig(*iniConfig, section)) {
            logManager.log(LOG_WARNING, logTag,
                           section + ": loadConfig() failed — skipping");
            continue;
        }

        logManager.log(LOG_DEBUG, logTag,
                       "Loaded object: " + obj->getName() + " (type=" + type + ")");
        procObjects.push_back(std::move(obj));
    }

    logManager.log(LOG_DEBUG, logTag,
                   "loadObjects: " + std::to_string(procObjects.size()) + " object(s) loaded");
}

// ---------------------------------------------------------------------------
// mainTick / processingTick
// ---------------------------------------------------------------------------

void XCespProc::mainTick()
{
    logManager.log(LOG_DEBUG, logTag, "heartbeat");
}

void XCespProc::processingTick()
{
    for (auto& obj : procObjects) {
        obj->process();
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
