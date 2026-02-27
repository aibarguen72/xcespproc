/**
 * @file    LinkObject.h
 * @brief   LinkObject — base class for all link infrastructure objects
 * @project XCESP
 * @date    2026-02-26
 */

#ifndef LINKOBJECT_H
#define LINKOBJECT_H

#include <atomic>
#include <string>
#include "LinkRole.h"

class ProcObject;   // forward declaration — LinkObject stores raw pointers but does not own them

/**
 * @brief  Base class for all link types.
 *
 * A LinkObject holds exactly one ProcObject* per role (master and slave).
 * It is NOT a ProcObject — it is pure infrastructure that does not participate
 * in the processing tick.  Derived classes (e.g. PduLinkObject) add the
 * actual communication methods (e.g. sendPDU).
 *
 * Lifecycle:
 *   - Created by XCespProc::registerLink() when the first object registers into a
 *     named link.  The concrete type is determined by the linkClass argument.
 *   - Persists until XCespProc is destroyed (entries are never erased from the
 *     links_ map), so raw pointers returned by getLink() remain stable.
 *   - Objects call unregisterLink() (via linkRegistry_) before they are removed.
 *
 * State:
 *   - INCOMPLETE until both ROLE_MASTER and ROLE_SLAVE slots are filled.
 *   - UP once both slots are filled; reverts to INCOMPLETE on any unregistration.
 *
 * Thread safety:
 *   - registerObject() / unregisterObject() are called from XCespProc under
 *     linksMutex_.  The underlying atomics make individual pointer stores and
 *     loads safe independently of that mutex.
 *   - getMaster() / getSlave() / getState() can be called without linksMutex_
 *     from any thread; they use acquire loads on the atomics.
 *   - Derived-class communication methods (e.g. sendPDU) snapshot both pointers
 *     once with acquire and operate on the local copies — safe against a
 *     concurrent unregisterObject from another thread.
 */
class LinkObject {
public:
    /**
     * @brief  Link state — UP only when both roles are filled.
     */
    enum class LinkState { UP, INCOMPLETE };

    explicit LinkObject(const std::string& name);
    virtual ~LinkObject() = default;

    // --- Identity ---

    const std::string& getName() const { return name_; }

    /**
     * @brief  The factory key used to create this link (e.g. "PDU").
     *
     * Set at construction by derived classes; checked by XCespProc::registerLink()
     * to detect linkClass mismatches on subsequent registrations into an existing link.
     */
    virtual const std::string& getLinkClass() const = 0;

    // --- Registration ---

    /**
     * @brief  Register an object in the given role.
     *
     * Uses compare_exchange_strong so only one caller wins a given slot.
     *
     * @return false if the role slot is already occupied (caller should log error).
     */
    bool registerObject(ProcObject* obj, LinkRole role);

    /**
     * @brief  Remove the given object from whichever role it occupies.
     *
     * No-op if the object is not currently registered in this link.
     */
    void unregisterObject(ProcObject* obj);

    // --- State query ---

    /**
     * @brief  Returns UP if both master and slave slots are non-null.
     */
    LinkState getState() const;

    /**
     * @brief  Return the master-role object, or nullptr if not registered.
     *         Uses acquire ordering — safe to call from any thread.
     */
    ProcObject* getMaster() const { return master_.load(std::memory_order_acquire); }

    /**
     * @brief  Return the slave-role object, or nullptr if not registered.
     *         Uses acquire ordering — safe to call from any thread.
     */
    ProcObject* getSlave()  const { return slave_.load(std::memory_order_acquire); }

protected:
    std::string              name_;
    std::atomic<ProcObject*> master_{nullptr};  ///< master slot; compare_exchange for claim
    std::atomic<ProcObject*> slave_ {nullptr};  ///< slave slot;  compare_exchange for claim
};

#endif // LINKOBJECT_H
