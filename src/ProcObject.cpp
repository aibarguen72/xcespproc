/**
 * @file    ProcObject.cpp
 * @brief   ProcObject — abstract base class implementation
 * @project XCESP
 * @date    2026-02-19
 */

#include "ProcObject.h"

bool ProcObject::loadConfig(IniConfig& ini, const std::string& section)
{
    (void)ini;
    name_ = section;
    return true;
}

void ProcObject::init(EvApplication& loop, LogManager& mgr, const std::string& appTag)
{
    loop_   = &loop;
    log_    = &mgr;
    logTag_ = appTag + ":" + name_;
}
