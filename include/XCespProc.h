/**
 * @file    XCespProc.h
 * @brief   XCespProc — XCESP processing application
 * @project XCESP
 * @date    2026-02-19
 */

#ifndef XCESPPROC_H
#define XCESPPROC_H

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "evapplication.h"
#include "LogManager.h"
#include "IniConfig.h"
#include "ArgConfig.h"

#include "ProcObject.h"
#include "LinkRegistry.h"
#include "links/LinkObject.h"

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
class XCespProc : public EvApplication, public LinkRegistry {
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

    /**
     * @brief  Schedule a ProcObject for removal by name.
     *
     * Thread-safe: can be called from any thread. The actual destruction is
     * deferred to the next processing tick so no raw pointer held by the
     * processing thread is ever invalidated.
     *
     * @param  name  The name returned by ProcObject::getName()
     * @return true if an object with that name was found (and queued),
     *         false if no match exists
     */
    bool removeProcObject(const std::string& name);

    // --- LinkRegistry interface ---

    /**
     * @brief  Register a ProcObject into a named link with the given role.
     *
     * Creates the LinkObject if it does not exist (using linkClass to select
     * the concrete type).  Returns false if the link already exists with a
     * different class, if the role is already occupied, or if linkClass is
     * unknown.
     *
     * Thread-safe: protected by linksMutex_.
     */
    bool registerLink(const std::string& name,
                      ProcObject*         obj,
                      LinkRole            role,
                      const std::string&  linkClass) override;

    /**
     * @brief  Remove the given object from the named link.
     *
     * The LinkObject itself is retained in the registry (allows re-registration
     * without recreating it).
     *
     * Thread-safe: protected by linksMutex_.
     */
    void unregisterLink(const std::string& name, ProcObject* obj) override;

    /**
     * @brief  Look up a link by name.
     *
     * @return Pointer to the LinkObject, or nullptr if not found.
     *         The pointer is stable for the lifetime of this XCespProc instance.
     */
    LinkObject* getLink(const std::string& name) override;

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

    // --- Scheduler config (read from [PROC] in INI) ---
    int hbIntervalMs_   = 100;   ///< PROC_HB_INTERVAL: timer interval in ms
    int maxObjsPerHb_   = 100;   ///< PROC_MAX_OBJECTS_PER_HB: objects handled per tick
    int hbIntervalMult_ = 100;   ///< PROC_HB_INTERVAL_MULT: ticks per full round

    // --- Processing round state (processing thread only, no lock needed) ---
    uint32_t currentIteration_ = 1;  ///< iteration token; increments each completed round
    int      processCounter_   = 0;  ///< ticks elapsed in the current round [0, hbIntervalMult_)

    // --- Deferred loading state (main thread only, no lock needed) ---
    int nextSectionIdx_ = 1;   ///< next INI [object.N] section index to probe
    int loadTimerId_    = -1;  ///< main-thread load timer ID; -1 when not active

    // --- Thread safety for procObjects (written by main, read by proc thread) ---
    std::mutex               procMutex_;
    std::vector<std::string> pendingRemovals_;  ///< names queued for removal; protected by procMutex_

    // --- Link registry (LinkRegistry interface implementation) ---
    std::map<std::string, std::unique_ptr<LinkObject>> links_;
    std::mutex                                          linksMutex_;  ///< protects links_ map structure

    /**
     * @brief  Set up log writers from [PROC] config:
     *         console, optional file, optional local syslog forwarding
     */
    void setupLogging();

    /**
     * @brief  Load up to maxObjsPerHb_ objects from [object.N] INI sections,
     *         starting from nextSectionIdx_. Cancels the load timer when the end
     *         of configured sections is reached. Called from loadBatchTimerCallback.
     */
    void loadObjectsBatch();

    /**
     * @brief  Print startup banner (program name and version) directly to stdout
     */
    void printBanner() const;

    /**
     * @brief  Called from the main thread 10 s timer — heartbeat log + parent-loss check
     */
    void mainTick();

    /**
     * @brief  Called from the processing thread timer every PROC_HB_INTERVAL ms.
     *         Processes up to maxObjsPerHb_ pending objects per tick.
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

    /**
     * @brief  Main-thread timer callback — dispatches to loadObjectsBatch()
     */
    static void loadBatchTimerCallback(int id, void* userData);
};

#endif // XCESPPROC_H
