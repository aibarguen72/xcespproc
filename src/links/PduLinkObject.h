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
#include <string>

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

    /**
     * @brief  Called by PduLinkObject::sendPDUTo() — same as onSendPDU but carries a
     *         one-shot destination address hint.  The receiver may use dstIp/dstPort to
     *         direct this specific payload to an address other than its configured default.
     *
     * Default implementation ignores the address hint and delegates to onSendPDU().
     *
     * @param  data     PDU payload (caller-owned; valid for this call only).
     * @param  len      Payload length in bytes.
     * @param  dstIp    Destination IPv4 address string (e.g. "10.0.0.1").
     * @param  dstPort  Destination UDP port.
     */
    virtual void onSendPDUTo(const uint8_t* data, size_t len,
                              const std::string& dstIp, uint16_t dstPort) {
        (void)dstIp; (void)dstPort;
        onSendPDU(data, len);
    }

    /**
     * @brief  Called by PduLinkObject::getPDU() — a pull request from the peer.
     *
     * The implementation should generate a PDU and push it back to the requesting
     * peer by calling pduLink_->sendPDU() (or sendPDUTo()).  Default is a no-op.
     */
    virtual void onGetPDU() {}
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

    /**
     * @brief  Deliver a PDU with a one-shot destination address hint.
     *
     * Like sendPDU() but calls the peer's onSendPDUTo(data, len, dstIp, dstPort)
     * instead of onSendPDU(), allowing the carrier object to redirect this specific
     * packet to a different network address without changing its persistent config.
     *
     * @param  sender   Must be the object registered as ROLE_MASTER or ROLE_SLAVE.
     * @param  data     PDU payload (not retained after this call returns).
     * @param  len      Payload length in bytes.
     * @param  dstIp    Destination IPv4 address hint for this datagram.
     * @param  dstPort  Destination UDP port hint for this datagram.
     *
     * @return true on successful delivery; false on same conditions as sendPDU().
     */
    bool sendPDUTo(ProcObject* sender, const uint8_t* data, size_t len,
                   const std::string& dstIp, uint16_t dstPort);

    /**
     * @brief  Request a PDU from the peer (pull mechanism).
     *
     * Calls the peer's onGetPDU().  The peer is expected to generate a packet
     * and push it back via sendPDU() / sendPDUTo() synchronously.
     *
     * @param  sender  Must be the object registered as ROLE_MASTER or ROLE_SLAVE.
     *
     * @return true if the request was delivered; false on same conditions as sendPDU().
     */
    bool getPDU(ProcObject* sender);

private:
    static const std::string CLASS_NAME;   ///< "PDU"
};

#endif // PDULINKOBJECT_H
