/**
 * @file    PduLinkObject.cpp
 * @brief   PduLinkObject — PDU-passing link implementation
 * @project XCESP
 * @date    2026-02-26
 */

#include "PduLinkObject.h"
#include "ProcObject.h"

// ---------------------------------------------------------------------------
// Class name constant
// ---------------------------------------------------------------------------

const std::string PduLinkObject::CLASS_NAME = "PDU";

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

PduLinkObject::PduLinkObject(const std::string& name)
    : LinkObject(name)
{}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

const std::string& PduLinkObject::getLinkClass() const
{
    return CLASS_NAME;
}

// ---------------------------------------------------------------------------
// sendPDU
// ---------------------------------------------------------------------------

bool PduLinkObject::sendPDU(ProcObject* sender, const uint8_t* data, size_t len)
{
    // Snapshot both role pointers once with acquire ordering.
    // This guards against a concurrent unregisterLink from the main thread
    // nulling master_/slave_ while we are mid-call.
    ProcObject* m = master_.load(std::memory_order_acquire);
    ProcObject* s = slave_.load(std::memory_order_acquire);

    // Link must be UP — both roles must be filled
    if (m == nullptr || s == nullptr)
        return false;

    // Determine the peer: the party that is NOT the sender
    ProcObject* peer = nullptr;
    if      (sender == m) peer = s;
    else if (sender == s) peer = m;
    else return false;   // sender is not a registered party of this link

    // Peer must implement IPduReceiver
    auto* receiver = dynamic_cast<IPduReceiver*>(peer);
    if (receiver == nullptr)
        return false;

    receiver->onSendPDU(data, len);
    return true;
}

// ---------------------------------------------------------------------------
// sendPDUTo
// ---------------------------------------------------------------------------

bool PduLinkObject::sendPDUTo(ProcObject* sender, const uint8_t* data, size_t len,
                               const std::string& dstIp, uint16_t dstPort)
{
    ProcObject* m = master_.load(std::memory_order_acquire);
    ProcObject* s = slave_.load(std::memory_order_acquire);

    if (m == nullptr || s == nullptr)
        return false;

    ProcObject* peer = nullptr;
    if      (sender == m) peer = s;
    else if (sender == s) peer = m;
    else return false;

    auto* receiver = dynamic_cast<IPduReceiver*>(peer);
    if (receiver == nullptr)
        return false;

    receiver->onSendPDUTo(data, len, dstIp, dstPort);
    return true;
}

// ---------------------------------------------------------------------------
// getPDU
// ---------------------------------------------------------------------------

bool PduLinkObject::getPDU(ProcObject* sender)
{
    ProcObject* m = master_.load(std::memory_order_acquire);
    ProcObject* s = slave_.load(std::memory_order_acquire);

    if (m == nullptr || s == nullptr)
        return false;

    ProcObject* peer = nullptr;
    if      (sender == m) peer = s;
    else if (sender == s) peer = m;
    else return false;

    auto* receiver = dynamic_cast<IPduReceiver*>(peer);
    if (receiver == nullptr)
        return false;

    receiver->onGetPDU();
    return true;
}
