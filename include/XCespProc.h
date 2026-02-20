/**
 * @file    XCespProc.h
 * @brief   XCespProc — XCESP processing application
 * @project XCESP
 * @date    2026-02-19
 */

#ifndef XCESPPROC_H
#define XCESPPROC_H

#include <memory>
#include <string>
#include <vector>

#include "evapplication.h"
#include "LogManager.h"
#include "IniConfig.h"
#include "ArgConfig.h"

#include "ProcObject.h"

// Forward declaration — full definition in evthread.h (included via evapplication.h)
class EvThread;

/**
 * @brief  Main processing application class
 *
 * Inherits the EvApplication event loop and adds:
 *  - Command-line argument parsing (ArgConfig)
 *  - INI-based configuration loading (IniConfig)
 *  - Centralised logging (LogManager): console, file, and optional local
 *    syslog forwarding to 127.0.0.1:LOG_LOCAL_PORT (default: xcespwdog on 1514)
 *  - A dedicated processing thread (EvThread) with a 100 ms repeating timer
 *    that calls process() on every registered ProcObject
 */
class XCespProc : public EvApplication {
public:
    /**
     * @brief  Construct the application, wrapping argc/argv for later parsing
     */
    XCespProc(int argc, char* argv[]);

    ~XCespProc() override;

    /**
     * @brief  Initialise the application
     *
     * Parses arguments, loads the INI configuration file, sets up log writers,
     * opens the local syslog listener, loads processing objects, creates the
     * processing thread, and calls EvApplication::initApplication().
     *
     * @return true on success, false if a fatal error prevents startup
     */
    bool init();

    /**
     * @brief  Enter the event loop (blocks until stopLoop() is called)
     */
    void run();

private:
    ArgConfig     argConfig;
    IniConfig*    iniConfig    = nullptr;
    LogManager    logManager;
    EvThread*     procThread   = nullptr;

    std::vector<std::unique_ptr<ProcObject>> procObjects;

    std::string configFile;
    bool        verbose   = false;
    int         procId    = 1;
    std::string logTag;           ///< e.g. "xcespproc-1" — used as the log source tag
    int         localPort = 1514;
    pid_t       originalPpid_ = 0; ///< PPID recorded at startup; 0 = no parent-loss check

    /**
     * @brief  Set up log writers from [PROC] config:
     *         console, optional file, optional local syslog forwarding
     */
    void setupLogging();

    /**
     * @brief  Load processing objects from [object.N] INI sections
     *
     * Iterates until no TYPE key is found. Supported types: "UdpTester".
     */
    void loadObjects();

    /**
     * @brief  Print startup banner (program name and version) directly to stdout
     */
    void printBanner() const;

    /**
     * @brief  Called from the main thread timer every 10 s — heartbeat log
     */
    void mainTick();

    /**
     * @brief  Called from the processing thread timer every 100 ms
     */
    void processingTick();

    /**
     * @brief  Parse a log level string from the INI file (e.g. "Info" → LOG_INFO)
     */
    static LogLevel parseLogLevel(const std::string& s);

    /**
     * @brief  Main thread 10 s timer callback — dispatches to mainTick()
     */
    static void mainTimerCallback(int id, void* userData);

    /**
     * @brief  EvThread timer callback — dispatches to processingTick()
     */
    static void processingTimerCallback(int id, void* userData);
};

#endif // XCESPPROC_H
