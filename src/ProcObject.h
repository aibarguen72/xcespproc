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

/**
 * @brief  Abstract base for all processing objects managed by the processing thread.
 *
 * Each concrete subclass implements loadConfig() to read its parameters from an
 * INI section and process() to perform periodic work (called every 100 ms tick).
 */
class ProcObject {
public:
    virtual ~ProcObject() = default;

    enum class ObjStatus { IDLE, ACTIVE, ERROR };

    // --- Identity ---

    /**
     * @brief  Return the object's human-readable name (typically its INI section)
     */
    virtual const std::string& getName() const = 0;

    // --- Lifecycle ---

    /**
     * @brief  Read configuration from the given INI section
     * @param  ini      Loaded IniConfig instance
     * @param  section  INI section name (e.g. "object.1")
     * @return true on success, false if mandatory keys are missing or invalid
     */
    virtual bool loadConfig(IniConfig& ini, const std::string& section) = 0;

    /**
     * @brief  Perform one processing tick (called from processing thread every 100 ms)
     */
    virtual void process() = 0;

    // --- Status access ---

    /**
     * @brief  Return the current object status
     */
    ObjStatus getObjStatus() const { return status; }

protected:
    ObjStatus status = ObjStatus::IDLE;
};

#endif // PROCOBJECT_H
