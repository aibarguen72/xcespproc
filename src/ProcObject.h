/**
 * @file    ProcObject.h
 * @brief   ProcObject — abstract base class for processing objects
 * @project XCESP
 * @date    2026-02-19
 */

#ifndef PROCOBJECT_H
#define PROCOBJECT_H

#include <string>
#include "IniConfig.h"
#include "LogManager.h"
#include "evapplication.h"
#include "LinkRegistry.h"

/**
 * @brief  Abstract base for all processing objects managed by the processing thread.
 *
 * Each concrete subclass implements loadConfig() to read its parameters from an
 * INI section and process() to perform periodic work (called every 100 ms tick).
 *
 * The base class owns the object name, event-loop reference, log manager reference,
 * and composite log tag ("appTag:name"). Derived classes call ProcObject::loadConfig()
 * first to set the name, and ProcObject::init() first to set the shared resources.
 */
class ProcObject {
public:
    virtual ~ProcObject() = default;

    enum class ObjStatus { IDLE, ACTIVE, ERROR };

    // --- Identity (concrete; derived classes do not override) ---

    /**
     * @brief  Return the object's human-readable name (set from the INI section name)
     */
    const std::string& getName() const { return name_; }

    // --- Lifecycle ---

    /**
     * @brief  Supply the thread event loop, log manager, and application-level tag.
     *
     * Called by XCespProc after loadConfig() and after addThread(), but before
     * procThread->start().  Base implementation stores loop_, log_, and builds
     * logTag_ = appTag + ":" + name_.
     *
     * Derived classes MUST call ProcObject::init() first if they override.
     */
    virtual void init(EvApplication& loop, LogManager& mgr, const std::string& appTag);

    /**
     * @brief  Read configuration from the given INI section.
     *
     * Base implementation sets name_ = section.
     * Derived classes MUST call ProcObject::loadConfig() first if they override.
     *
     * @param  ini      Loaded IniConfig instance
     * @param  section  INI section name (e.g. "object.1")
     * @return true on success, false if mandatory keys are missing or invalid
     */
    virtual bool loadConfig(IniConfig& ini, const std::string& section);

    /**
     * @brief  Perform one processing tick (called from processing thread every 100 ms).
     *
     * In the event-driven model this is used only to open the socket (IDLE->ACTIVE)
     * and retry on error. Send and receive are handled by event callbacks.
     */
    virtual void process() = 0;

    // --- Optional typed accessors (virtual, default nullptr) ---

    /**
     * @brief  Return a pointer to the object's configuration struct, or nullptr.
     *         Callers that need the typed struct must static_cast.
     */
    virtual const void* getConfig() const { return nullptr; }

    /**
     * @brief  Return a pointer to the object's public status struct, or nullptr.
     */
    virtual const void* getStatus() const { return nullptr; }

    /**
     * @brief  Return a pointer to the object's statistics struct, or nullptr.
     */
    virtual const void* getStats()  const { return nullptr; }

    /**
     * @brief  Reset all accumulated statistics counters to zero.
     *
     * Must be called from the processing thread only — the primary stats struct
     * is process-thread-owned so no locking is needed.  The next syncSnapshot()
     * call will publish the zeroed counters to the main-thread snapshot.
     */
    virtual void clearStats() {}

    /**
     * @brief  Copy primary status/stats to the next snapshot buffer and atomically
     *         publish it for the main thread.
     *
     * Derived classes override this to implement lock-free triple-buffer
     * synchronisation: one primary (process-thread-owned) + two read buffers,
     * indexed by an atomic int that is flipped after each copy.
     *
     * Called at the end of process() so the main thread always sees a freshly
     * updated, self-consistent snapshot without any mutex.
     */
    virtual void syncSnapshot() {}

    // --- Base status accessor ---

    /**
     * @brief  Return the current object status
     */
    ObjStatus getObjStatus() const { return status; }

    // --- Scheduler iteration tracking ---

    /**
     * @brief  Return the scheduling iteration in which this object was last process()ed.
     *         0 = never processed. Set by XCespProc::processingTick().
     */
    uint32_t getProcessIteration() const        { return processIteration_; }
    void     setProcessIteration(uint32_t iter) { processIteration_ = iter; }

    // --- Link registry injection ---

    /**
     * @brief  Inject the link registry.
     *
     * Called by XCespProc after init().  Derived classes use linkRegistry_ to
     * register themselves into named links during their first process() tick
     * (IDLE state), and to unregister before removal.
     *
     * Guard all usage: linkRegistry_ is nullptr until injection.
     */
    void setLinkRegistry(LinkRegistry* reg) { linkRegistry_ = reg; }

protected:
    std::string    name_;
    ObjStatus      status           = ObjStatus::IDLE;
    EvApplication* loop_            = nullptr;    ///< thread event loop (set by init())
    LogManager*    log_             = nullptr;    ///< shared log manager (set by init())
    std::string    logTag_;                       ///< e.g. "xcespproc-1:object.1" (set by init())
    uint32_t       processIteration_ = 0;         ///< last scheduling iteration; 0 = unprocessed
    LinkRegistry*  linkRegistry_    = nullptr;    ///< link registry (injected after init(); nullptr until then)
};

#endif // PROCOBJECT_H
