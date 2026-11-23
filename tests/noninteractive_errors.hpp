// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifdef __cplusplus
#include <cstdlib>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
// CI/mock hosts must fail instead of opening a modal CRT assertion window on
// the user's desktop. Checks are NOT disabled: diagnostics go to stderr and
// the process retains its failing exit status. This header is tests-only.
inline const bool kNonInteractiveCrtReports = [] {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    return true;
}();
#endif
