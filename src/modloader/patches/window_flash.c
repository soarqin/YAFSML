/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "window_flash.h"
#include "log.h"

typedef ATOM (WINAPI *register_class_ex_w_t)(const WNDCLASSEXW *window_class);

static register_class_ex_w_t old_register_class_ex_w;

void ml_window_class_blacken(const WNDCLASSEXW *source, HBRUSH background, WNDCLASSEXW *result) {
    *result = *source;
    result->hbrBackground = background;
}

static ATOM WINAPI register_class_ex_w_hooked(const WNDCLASSEXW *window_class) {
    WNDCLASSEXW black_class;
    static HBRUSH black_background;

    if (window_class == NULL) return old_register_class_ex_w(window_class);

    if (black_background == NULL) {
        /* RegisterClassExW may be called from multiple threads; publish the
         * brush atomically so concurrent callers do not leak a GDI handle. */
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
        if (brush == NULL) return old_register_class_ex_w(window_class);
        if (InterlockedCompareExchangePointer((PVOID volatile *)&black_background, brush, NULL) != NULL) {
            DeleteObject(brush);
        }
    }

    ml_window_class_blacken(window_class, black_background, &black_class);
    return old_register_class_ex_w(&black_class);
}

ml_hook_result_t ml_window_flash_install(void) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void *register_class_ex_w = user32 == NULL ? NULL : (void *)GetProcAddress(user32, "RegisterClassExW");
    ml_hook_result_t result = ml_hook_install(register_class_ex_w, register_class_ex_w_hooked,
                                              (void **)&old_register_class_ex_w);
    ml_log_write(result == ML_HOOK_APPLIED ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 L"window-flash", L"RegisterClassExW hook %hs", ml_hook_result_name(result));
    return result;
}
