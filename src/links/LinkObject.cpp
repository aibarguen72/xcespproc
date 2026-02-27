/**
 * @file    LinkObject.cpp
 * @brief   LinkObject — base class implementation
 * @project XCESP
 * @date    2026-02-26
 */

#include "LinkObject.h"

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

LinkObject::LinkObject(const std::string& name)
    : name_(name)
{}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

bool LinkObject::registerObject(ProcObject* obj, LinkRole role)
{
    switch (role) {
        case LinkRole::ROLE_MASTER: {
            ProcObject* expected = nullptr;
            return master_.compare_exchange_strong(expected, obj,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire);
        }
        case LinkRole::ROLE_SLAVE: {
            ProcObject* expected = nullptr;
            return slave_.compare_exchange_strong(expected, obj,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire);
        }
    }
    return false;   // unknown role — guard for future extensibility
}

void LinkObject::unregisterObject(ProcObject* obj)
{
    // Clear whichever slot holds this object.
    // Load-then-store is not atomic as a unit, but this is called under linksMutex_
    // so no other thread modifies the slot concurrently.
    ProcObject* m = master_.load(std::memory_order_acquire);
    if (m == obj)
        master_.store(nullptr, std::memory_order_release);

    ProcObject* s = slave_.load(std::memory_order_acquire);
    if (s == obj)
        slave_.store(nullptr, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

LinkObject::LinkState LinkObject::getState() const
{
    return (master_.load(std::memory_order_acquire) != nullptr &&
            slave_.load(std::memory_order_acquire)  != nullptr)
           ? LinkState::UP
           : LinkState::INCOMPLETE;
}
