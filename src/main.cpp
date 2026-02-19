/**
 * @file    main.cpp
 * @brief   xcespproc entry point
 * @project XCESP
 * @date    2026-02-19
 */

#include "XCespProc.h"

int main(int argc, char* argv[])
{
    XCespProc app(argc, argv);

    if (!app.init()) {
        return 1;
    }

    app.run();
    return 0;
}
