#include "runtime_ready.h"

#include "log.h"

#include "modloader/hook.h"

typedef bool (__cdecl *steam_api_init_t)(void);

static steam_api_init_t old_steam_api_init;
static HMODULE steam_api_module;
static void *steam_api_init_hook_target;
/* 0 = pending, 1 = running, 2 = done. Not an INIT_ONCE: an external DLL loaded by
 * the callback may call SteamAPI_Init itself, and INIT_ONCE would then wait for
 * its own pending initialization forever. Nested calls simply pass through. */
static volatile LONG after_runtime_state;
static ml_runtime_ready_callback_t after_runtime_callback;
static const wchar_t *hook_log_scope;

static bool __cdecl steam_api_init_hooked(void) {
    bool result = old_steam_api_init();
    if (result && InterlockedCompareExchange(&after_runtime_state, 1, 0) == 0) {
        if (after_runtime_callback()) {
            InterlockedExchange(&after_runtime_state, 2);
        } else {
            InterlockedExchange(&after_runtime_state, 0);
            ML_LOG_WARN(hook_log_scope, L"AFTER_RUNTIME_INIT setup failed; deferred initialization will retry");
        }
    }
    return result;
}

bool ml_runtime_ready_hook_install(const wchar_t *log_scope,
                                   ml_runtime_ready_callback_t callback) {
    void *steam_api_init;
    ml_hook_result_t result;

    if (log_scope == NULL || callback == NULL || steam_api_init_hook_target != NULL) return false;
    steam_api_module = LoadLibraryW(L"steam_api64.dll");
    steam_api_init = steam_api_module == NULL ? NULL :
        (void *)GetProcAddress(steam_api_module, "SteamAPI_Init");
    after_runtime_callback = callback;
    hook_log_scope = log_scope;
    result = ml_hook_install(steam_api_init, steam_api_init_hooked, (void **)&old_steam_api_init);
    if (result != ML_HOOK_APPLIED) {
        if (steam_api_module != NULL) FreeLibrary(steam_api_module);
        steam_api_module = NULL;
        after_runtime_callback = NULL;
        hook_log_scope = NULL;
        ml_log_write(ML_LOG_LEVEL_WARN, log_scope,
                     L"SteamAPI_Init runtime-ready hook %hs", ml_hook_result_name(result));
        return false;
    }
    steam_api_init_hook_target = steam_api_init;
    ML_LOG_INFO(log_scope, L"SteamAPI_Init runtime-ready hook APPLIED");
    return true;
}

bool ml_runtime_ready_hook_uninstall(const wchar_t *log_scope) {
    if (!ml_hook_remove(steam_api_init_hook_target, log_scope)) return false;
    steam_api_init_hook_target = NULL;
    old_steam_api_init = NULL;
    after_runtime_callback = NULL;
    hook_log_scope = NULL;
    InterlockedExchange(&after_runtime_state, 0);
    if (steam_api_module != NULL) FreeLibrary(steam_api_module);
    steam_api_module = NULL;
    return true;
}
