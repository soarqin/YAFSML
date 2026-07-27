#include <stdbool.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static volatile LONG init_count;
static volatile LONG connector_count;

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
