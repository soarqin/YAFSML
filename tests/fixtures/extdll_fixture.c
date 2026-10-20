#include <stdbool.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static volatile LONG init_count;
static volatile LONG connector_count;
static volatile LONG custom_init_count;
static volatile LONG failed_init_count;

/* Also serves as a loader fixture for the launcher's long-path injection test. */
__declspec(dllexport) DWORD WINAPI YAFSMLInit(LPVOID parameter) {
    (void)parameter;
    return 1;
}

__declspec(dllexport) bool modengine_ext_init(const void *connector, void **extension) {
    InterlockedIncrement(&init_count);
    if (connector != NULL) InterlockedIncrement(&connector_count);
    if (extension != NULL) *extension = (void *)1;
    return true;
}

__declspec(dllexport) LONG extdll_fixture_init_count(void) {
    return InterlockedCompareExchange(&init_count, 0, 0);
}

__declspec(dllexport) LONG extdll_fixture_connector_count(void) {
    return InterlockedCompareExchange(&connector_count, 0, 0);
}

__declspec(dllexport) bool __cdecl extdll_fixture_init(void) {
    InterlockedIncrement(&custom_init_count);
    return true;
}

__declspec(dllexport) bool __cdecl extdll_fixture_fail_init(void) {
    InterlockedIncrement(&failed_init_count);
    return false;
}

__declspec(dllexport) LONG extdll_fixture_custom_init_count(void) {
    return InterlockedCompareExchange(&custom_init_count, 0, 0);
}

__declspec(dllexport) LONG extdll_fixture_failed_init_count(void) {
    return InterlockedCompareExchange(&failed_init_count, 0, 0);
}
