/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void extdlls_add(const char *name, const wchar_t *path);
extern void extdlls_add_spec(const char *name, const char *value);
extern int extdlls_count();
extern void extdlls_prepare();
extern bool extdlls_load_early();
extern void extdlls_load_all();
extern void extdlls_load_data_ready();
extern void extdlls_unload_all();

#ifdef ML_EXTDLL_TEST
extern const char *extdlls_test_name_at(int index);
extern int extdlls_test_stage_at(int index);
extern bool extdlls_test_is_early_at(int index);
extern bool extdlls_test_is_effective_early_at(int index);
extern bool extdlls_test_is_data_ready_at(int index);
extern bool extdlls_test_is_deferred_at(int index);
extern bool extdlls_test_has_delayed_at(bool early);
extern uint32_t extdlls_test_delay_at(int index);
extern int extdlls_test_after_count(int index);
extern const char *extdlls_test_after_at(int index, int dependency);
extern void extdlls_test_load_at(int index);
#endif

#ifdef __cplusplus
}
#endif
