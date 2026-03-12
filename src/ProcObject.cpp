/**
 * @file    ProcObject.cpp
 * @brief   ProcObject — abstract base class implementation
 * @project XCESP
 * @date    2026-02-19
 */

#include "ProcObject.h"

bool ProcObject::loadConfig(IniConfig& ini, const std::string& section)
{
    auto nameVal = ini.getValue(section, "NAME");
    name_ = (nameVal.has_value() && !nameVal->empty()) ? nameVal.value() : section;
    nodeType_     = ini.getValue(section, "NODE_TYPE",     std::string(""));
    nodeInstance_ = ini.getValue(section, "NODE_INSTANCE", std::string(""));
    nodePath_     = ini.getValue(section, "NODE_PATH",     std::string(""));
    return true;
}

void ProcObject::init(EvApplication& loop, LogManager& mgr, const std::string& appTag)
{
    loop_   = &loop;
    log_    = &mgr;
    logTag_ = appTag + ":" + name_;
}
