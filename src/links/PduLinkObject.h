/**
 * @file    PduLinkObject.h
 * @brief   PduLinkObject — PDU-passing link; IPduReceiver companion interface
 * @project XCESP
 * @date    2026-02-26
 */

#ifndef PDULINKOBJECT_H
#define PDULINKOBJECT_H

#include "LinkObject.h"

#include <cstddef>
#include <cstdint>

/**
 * @brief  Interface that a ProcObject must implement to receive PDUs via a PduLinkObject.
 *
 * Any ProcObject that registers with a PduLinkObject should also inherit from
 * IPduReceiver and implement onSendPDU().  PduLinkObject::sendPDU() checks for
 * this interface via dynamic_cast at send time — if the peer does not implement
 * it, sendPDU() returns false.
 *
 * onSendPDU() is always invoked from the processing thread (the same thread that
 * calls sendPDU), so no additional locking is required inside the implementation.
 *
 * The data pointer is caller-owned and valid only for the duration of the call.
 */
class IPduReceiver {
public:
    virtual ~IPduReceiver() = default;

    /**
     * @brief  Called by PduLinkObject::sendPDU() on the receiving side.
     *
     * @param  data  Pointer to the PDU payload (caller-owned; valid for this call only).
     * @param  len   Payload length in bytes.
     */
    virtual void onSendPDU(const uint8_t* data, size_t len) = 0;
};

/**
 * @brief  Link type for raw PDU exchange between two ProcObjects.
 *
 * Either registered party calls sendPDU() to deliver a byte buffer to the
 * other party's onSendPDU() handler.  The call is synchronous and in-thread:
 * the sender calls sendPDU(), which directly calls the peer's onSendPDU()
 * before returning.  Both sender and receiver therefore execute on the
 * processing thread with no extra locking.
 *
 * sendPDU() is safe against a concurrent unregisterLink from the main thread
 * because it snapshots both role pointers with acquire loads at the start of
 * the call and operates only on those local copies.
 */
class PduLinkObject : public LinkObject {
public:
    explicit PduLinkObject(const std::string& name);

    /**
     * @brief  Returns "PDU" — the factory key used to create this link class.
     */
    const std::string& getLinkClass() const override;

    /**
     * @brief  Deliver a PDU from sender to its peer in the other role.
     *
     * @param  sender  Must be the object registered as ROLE_MASTER or ROLE_SLAVE.
     * @param  data    PDU payload (not retained after this call returns).
     * @param  len     Payload length in bytes.
     *
     * @return true on successful delivery.
     *         false if:
     *           - the link is not UP (either role is empty),
     *           - sender is not a registered party of this link,
     *           - the peer does not implement IPduReceiver.
     */
    bool sendPDU(ProcObject* sender, const uint8_t* data, size_t len);

private:
    static const std::string CLASS_NAME;   ///< "PDU"
};

#endif // PDULINKOBJECT_H
