#include <stdio.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "dearxan/analysis.h"
#include "dearxan/image.h"
#include "dearxan/patch.h"

int wmain(int argc, wchar_t **argv) {
    HMODULE module;
    dearxan_image_t image;
    dearxan_stub_list_t stubs;
    const char *error_message = NULL;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER analyzed;
    LARGE_INTEGER end;
    FILETIME creation_time;
    FILETIME exit_time;
    FILETIME kernel_start;
    FILETIME user_start;
    FILETIME kernel_end;
    FILETIME user_end;
    ULARGE_INTEGER kernel_start_value;
    ULARGE_INTEGER user_start_value;
    ULARGE_INTEGER kernel_end_value;
    ULARGE_INTEGER user_end_value;

    if (argc != 2) {
        fwprintf(stderr, L"usage: benchmark_dearxan_analysis <game.exe>\n");
        return 2;
    }
    module = LoadLibraryExW(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (module == NULL) {
        fwprintf(stderr, L"failed to map %ls: %lu\n", argv[1], GetLastError());
        return 1;
    }
    if (!dearxan_image_from_module(module, &image) ||
        !dearxan_image_set_preferred_base_from_file(&image, argv[1])) {
        fwprintf(stderr, L"failed to initialize mapped image\n");
        FreeLibrary(module);
        return 1;
    }
    QueryPerformanceFrequency(&frequency);
    GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time,
                    &kernel_start, &user_start);
    QueryPerformanceCounter(&start);
    if (!dearxan_analyze_all_stubs(&image, &stubs, &error_message)) {
        fprintf(stderr, "analysis failed: %s\n",
                error_message != NULL ? error_message : "unknown error");
        FreeLibrary(module);
        return 1;
    }
    QueryPerformanceCounter(&analyzed);
    if (!dearxan_image_make_sections_rwe(&image) ||
        !dearxan_apply_stub_patches(&image, &stubs, &error_message)) {
        fprintf(stderr, "patch failed: %s\n",
                error_message != NULL ? error_message : "unknown error");
        dearxan_free_stub_list(&stubs);
        FreeLibrary(module);
        return 1;
    }
    QueryPerformanceCounter(&end);
    GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time,
                    &kernel_end, &user_end);
    kernel_start_value.LowPart = kernel_start.dwLowDateTime;
    kernel_start_value.HighPart = kernel_start.dwHighDateTime;
    user_start_value.LowPart = user_start.dwLowDateTime;
    user_start_value.HighPart = user_start.dwHighDateTime;
    kernel_end_value.LowPart = kernel_end.dwLowDateTime;
    kernel_end_value.HighPart = kernel_end.dwHighDateTime;
    user_end_value.LowPart = user_end.dwLowDateTime;
    user_end_value.HighPart = user_end.dwHighDateTime;
    printf("stubs=%zu analysis_ms=%.3f patch_ms=%.3f total_ms=%.3f cpu_ms=%.3f\n",
           stubs.count,
           1000.0 * (double)(analyzed.QuadPart - start.QuadPart) /
               (double)frequency.QuadPart,
           1000.0 * (double)(end.QuadPart - analyzed.QuadPart) /
               (double)frequency.QuadPart,
           1000.0 * (double)(end.QuadPart - start.QuadPart) /
               (double)frequency.QuadPart,
           (double)((kernel_end_value.QuadPart - kernel_start_value.QuadPart) +
                    (user_end_value.QuadPart - user_start_value.QuadPart)) /
               10000.0);
    dearxan_free_stub_list(&stubs);
    FreeLibrary(module);
    return 0;
}
