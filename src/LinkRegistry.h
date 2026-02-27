/**
 * @file    LinkRegistry.h
 * @brief   LinkRegistry — abstract interface for object-to-object link management
 * @project XCESP
 * @date    2026-02-26
 */

#ifndef LINKREGISTRY_H
#define LINKREGISTRY_H

#include <string>
#include "links/LinkRole.h"

class ProcObject;   // forward — avoids circular include
class LinkObject;   // forward

/**
 * @brief  Abstract interface for the link registry.
 *
 * ProcObject subclasses receive a pointer to this interface via
 * setLinkRegistry() (injected by XCespProc after init()).  They use it to
 * register themselves into named links without depending on the concrete
 * XCespProc class.
 *
 * Typical usage from a ProcObject::process() first tick:
 * @code
 *   if (linkRegistry_)
 *       linkRegistry_->registerLink("my-link", this, LinkRole::ROLE_MASTER, "PDU");
 * @endcode
 *
 * And before the object is removed:
 * @code
 *   if (linkRegistry_)
 *       linkRegistry_->unregisterLink("my-link", this);
 * @endcode
 */
class LinkRegistry {
public:
    virtual ~LinkRegistry() = default;

    /**
     * @brief  Register an object into a named link with the given role.
     *
     * If the named link does not yet exist it is created using linkClass
     * (e.g. "PDU" creates a PduLinkObject).  If the link exists but was
     * created with a different linkClass, the call returns false.  If the
     * requested role is already occupied, the call returns false.
     *
     * Thread-safe: may be called from the processing thread or the main thread.
     *
     * @param  name       Human-readable link name (e.g. "bert-link-1")
     * @param  obj        The ProcObject registering itself (non-null)
     * @param  role       ROLE_MASTER or ROLE_SLAVE
     * @param  linkClass  Factory key that selects the derived LinkObject type
     * @return true on success, false on any error
     */
    virtual bool registerLink(const std::string& name,
                              ProcObject*         obj,
                              LinkRole            role,
                              const std::string&  linkClass) = 0;

    /**
     * @brief  Remove the given object from the named link.
     *
     * Clears whichever role slot holds obj.  The LinkObject itself is retained
     * in the registry so that re-registration is possible without recreating it.
     *
     * Thread-safe: may be called from the processing thread or the main thread.
     *
     * @param  name  The link name
     * @param  obj   The object to unregister
     */
    virtual void unregisterLink(const std::string& name, ProcObject* obj) = 0;

    /**
     * @brief  Look up a link by name.
     *
     * @return Pointer to the LinkObject, or nullptr if not found.
     *         The pointer is stable for the lifetime of the XCespProc instance
     *         (link entries are never removed from the registry).
     */
    virtual LinkObject* getLink(const std::string& name) = 0;
};

#endif // LINKREGISTRY_H
