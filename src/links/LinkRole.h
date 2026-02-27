/**
 * @file    LinkRole.h
 * @brief   LinkRole enum — roles an object can fill in a Link
 * @project XCESP
 * @date    2026-02-26
 */

#ifndef LINKROLE_H
#define LINKROLE_H

/**
 * @brief  Role an object occupies in a LinkObject.
 *
 * ROLE_MASTER and ROLE_SLAVE label the two participant slots.
 * Extensible: additional roles (e.g. ROLE_MONITOR) can be added later;
 * LinkObject::registerObject() will need a corresponding switch extension.
 */
enum class LinkRole {
    ROLE_MASTER,
    ROLE_SLAVE
};

#endif // LINKROLE_H
