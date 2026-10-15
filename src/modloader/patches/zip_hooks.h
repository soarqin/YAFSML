#pragma once

#include "modloader/hook_batch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ML_ZIP_HOOK_COUNT 23

void ml_zip_file_hook_specs(ml_hook_spec_t *specs, bool resolve_targets);
bool ml_zip_file_hooks_prepare(void);
HANDLE ml_zip_file_open(const wchar_t *path, DWORD access, DWORD share,
                        LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags);
void ml_zip_file_close_all(void);

#ifdef ML_WIN32_HOOKS_TEST
void ml_zip_file_hooks_test_init(void);
void ml_zip_file_hooks_test_read_gate(HANDLE entered, HANDLE resume, HANDLE shutdown);
#endif

#ifdef __cplusplus
}
#endif
