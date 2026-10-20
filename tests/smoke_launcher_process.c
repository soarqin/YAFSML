#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "launcher/launch_process.h"
#include "test_common.h"

static bool test_wine;
static bool test_getter_available;
static bool test_setter_available;
static bool test_locale_present;
static bool test_expect_locale_cleared;
static bool test_spawn_arguments_valid;
static BOOL test_spawn_result;
static LONG test_read_status;
static LONG test_clear_status;
static LONG test_restore_status;
static unsigned test_reads;
static unsigned test_writes;
static unsigned test_spawns;
static char test_locale[8192];

static LONG __cdecl test_unix_getenv(const char *name, char *buffer, size_t capacity) {
    test_reads++;
    if (strcmp(name, "LC_ALL") != 0) return (LONG)0xc000000d;
    if (test_read_status != 0) return test_read_status;
    if (!test_locale_present) return (LONG)0xc0000100;
    size_t size = strlen(test_locale) + 1;
    if (capacity < size) return (LONG)0xc0000023;
    memcpy(buffer, test_locale, size);
    return 0;
}

static LONG __cdecl test_unix_setenv(const char *name, const char *value) {
    test_writes++;
    /* Restoring the locale must not overwrite CreateProcessW's error. */
    SetLastError(ERROR_INVALID_FUNCTION);
    if (strcmp(name, "LC_ALL") != 0) return (LONG)0xc000000d;
    LONG status = test_writes == 1 ? test_clear_status : test_restore_status;
    if (status < 0) return status;
    test_locale_present = value != NULL;
    if (value != NULL) strcpy(test_locale, value);
    return 0;
}

static const char *__cdecl test_wine_version(void) {
    return "test";
}

/* Only ntdll is faked; injection and proxy detection use the real modules. */
static HMODULE WINAPI test_get_module_handle(LPCWSTR name) {
    return wcscmp(name, L"ntdll.dll") == 0 ? (HMODULE)(uintptr_t)1 : GetModuleHandleW(name);
}

static FARPROC WINAPI test_get_proc_address(HMODULE module, LPCSTR name) {
    if (module != (HMODULE)(uintptr_t)1) return GetProcAddress(module, name);
    if (test_wine && strcmp(name, "wine_get_version") == 0) return (FARPROC)test_wine_version;
    if (test_getter_available && strcmp(name, "__wine_get_unix_env") == 0) return (FARPROC)test_unix_getenv;
    if (test_setter_available && strcmp(name, "__wine_set_unix_env") == 0) return (FARPROC)test_unix_setenv;
    return NULL;
}

static BOOL WINAPI test_create_process(LPCWSTR executable, LPWSTR command_line,
                                        LPSECURITY_ATTRIBUTES process_attributes,
                                        LPSECURITY_ATTRIBUTES thread_attributes,
                                        BOOL inherit_handles, DWORD creation_flags,
                                        LPVOID environment, LPCWSTR working_directory,
                                        LPSTARTUPINFOW startup,
                                        LPPROCESS_INFORMATION process) {
    test_spawns++;
    test_spawn_arguments_valid = wcscmp(executable, L"C:\\模组\\game.exe") == 0 &&
        command_line != NULL && wcscmp(command_line, L"\"C:\\模组\\game.exe\"") == 0 &&
        process_attributes == NULL && thread_attributes == NULL &&
        !inherit_handles && creation_flags == CREATE_SUSPENDED && environment == NULL &&
        wcscmp(working_directory, L"C:\\模组") == 0 && startup->cb == sizeof(*startup) &&
        process != NULL && (!test_expect_locale_cleared || !test_locale_present);
    SetLastError(test_spawn_result ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
    return test_spawn_result;
}

/* Start with one slot so every module lookup exercises the buffer growth. */
#define ML_LAUNCHER_INITIAL_MODULES 1
#define GetModuleHandleW test_get_module_handle
#define GetProcAddress test_get_proc_address
#define CreateProcessW test_create_process
#include "../src/launcher/launch_process.c"
#undef CreateProcessW
#undef GetProcAddress
#undef GetModuleHandleW

static void reset_wine_test(const char *locale) {
    test_wine = true;
    test_getter_available = true;
    test_setter_available = true;
    test_locale_present = locale != NULL;
    if (locale != NULL) strcpy(test_locale, locale);
    test_expect_locale_cleared = true;
    test_spawn_arguments_valid = false;
    test_spawn_result = TRUE;
    test_read_status = test_clear_status = test_restore_status = 0;
    test_reads = test_writes = test_spawns = 0;
}

static int spawn_game(const wchar_t *executable, const wchar_t *working_directory) {
    STARTUPINFOW startup = { 0 };
    PROCESS_INFORMATION process = { 0 };
    startup.cb = sizeof(startup);
    EXPECT_EQ(ml_launcher_create_game_process(executable, working_directory,
                                               &startup, &process), test_spawn_result);
    EXPECT_EQ(GetLastError(), test_spawn_result ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
    EXPECT_TRUE(test_spawn_arguments_valid);
    EXPECT_EQ(test_spawns, 1);
    return 0;
}

static int spawn_with_test_locale(void) {
    return spawn_game(L"C:\\模组\\game.exe", L"C:\\模组");
}

/* Extended spellings that fit reach the game as DOS paths: with an extended
 * current directory, the game could not resolve relative paths with "..". */
static int test_plain_spawn_paths(void) {
    reset_wine_test(NULL);
    test_wine = false;
    test_expect_locale_cleared = false;
    EXPECT_EQ(spawn_game(L"\\\\?\\C:\\模组\\game.exe", L"\\\\?\\C:\\模组"), 0);
    return 0;
}

static int test_wine_locale(void) {
    reset_wine_test("C");
    test_wine = false;
    test_expect_locale_cleared = false;
    EXPECT_EQ(spawn_with_test_locale(), 0);
    EXPECT_EQ(test_reads, 0);
    EXPECT_EQ(test_writes, 0);
    EXPECT_STREQ_A(test_locale, "C");

    reset_wine_test("C");
    EXPECT_EQ(spawn_with_test_locale(), 0);
    EXPECT_EQ(test_writes, 2);
    EXPECT_TRUE(test_locale_present);
    EXPECT_STREQ_A(test_locale, "C");

    reset_wine_test(NULL);
    EXPECT_EQ(spawn_with_test_locale(), 0);
    EXPECT_EQ(test_writes, 2);
    EXPECT_TRUE(!test_locale_present);

    char long_locale[6001];
    memset(long_locale, 'x', sizeof(long_locale) - 1);
    long_locale[sizeof(long_locale) - 1] = '\0';
    reset_wine_test(long_locale);
    EXPECT_EQ(spawn_with_test_locale(), 0);
    EXPECT_TRUE(test_reads > 1);
    EXPECT_STREQ_A(test_locale, long_locale);

    reset_wine_test("C");
    test_spawn_result = FALSE;
    EXPECT_EQ(spawn_with_test_locale(), 0);
    EXPECT_TRUE(test_locale_present);
    EXPECT_STREQ_A(test_locale, "C");

    for (unsigned missing = 0; missing < 2; missing++) {
        reset_wine_test("C");
        test_getter_available = missing != 0;
        test_setter_available = missing == 0;
        test_expect_locale_cleared = false;
        EXPECT_EQ(spawn_with_test_locale(), 0);
        EXPECT_EQ(test_reads, 0);
        EXPECT_EQ(test_writes, 0);
        EXPECT_STREQ_A(test_locale, "C");
    }
    reset_wine_test("C");
    test_read_status = (LONG)0xc0000022;
    test_expect_locale_cleared = false;
    EXPECT_EQ(spawn_with_test_locale(), 0);
    EXPECT_EQ(test_writes, 0);
    EXPECT_STREQ_A(test_locale, "C");

    reset_wine_test("C");
    test_clear_status = (LONG)0xc0000022;
    test_expect_locale_cleared = false;
    EXPECT_EQ(spawn_with_test_locale(), 0);
    EXPECT_EQ(test_writes, 1);
    EXPECT_STREQ_A(test_locale, "C");

    reset_wine_test("C");
    test_restore_status = (LONG)0xc0000022;
    test_spawn_result = FALSE;
    EXPECT_EQ(spawn_with_test_locale(), 0);
    EXPECT_EQ(test_writes, 2);
    EXPECT_TRUE(!test_locale_present);
    return 0;
}

static DWORD WINAPI completed_thread(void *value) {
    return (DWORD)(uintptr_t)value;
}

static int test_remote_thread_wait(void) {
    DWORD exit_code = 0;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    EXPECT_NOT_NULL(process);
    HANDLE thread = CreateThread(NULL, 0, completed_thread, (void *)(uintptr_t)1, 0, NULL);
    EXPECT_NOT_NULL(thread);
    EXPECT_TRUE(ml_launcher_wait_for_remote_thread(process, thread, &exit_code));
    EXPECT_EQ(exit_code, 1);
    EXPECT_TRUE(!ml_launcher_wait_for_remote_thread(NULL, thread, &exit_code));
    EXPECT_EQ(GetLastError(), ERROR_INVALID_HANDLE);

    HANDLE exited = CreateEventW(NULL, TRUE, TRUE, NULL);
    EXPECT_NOT_NULL(exited);
    /* Both handles are signaled: game exit must take precedence. */
    EXPECT_TRUE(!ml_launcher_wait_for_remote_thread(exited, thread, &exit_code));
    EXPECT_EQ(GetLastError(), ERROR_PROCESS_ABORTED);
    EXPECT_TRUE(CloseHandle(exited));
    EXPECT_TRUE(CloseHandle(thread));

    /* A zero exit code is returned rather than judged: for LoadLibraryW it is
     * the low half of a 4 GiB-aligned module address. */
    thread = CreateThread(NULL, 0, completed_thread, NULL, 0, NULL);
    EXPECT_NOT_NULL(thread);
    exit_code = 1;
    EXPECT_TRUE(ml_launcher_wait_for_remote_thread(process, thread, &exit_code));
    EXPECT_EQ(exit_code, 0);
    EXPECT_TRUE(ml_launcher_wait_for_remote_thread(process, thread, NULL));
    EXPECT_TRUE(CloseHandle(thread));
    EXPECT_TRUE(CloseHandle(process));
    return 0;
}

static int test_game_exit_before_attach(void) {
    wchar_t executable[MAX_PATH];
    wchar_t command_line[MAX_PATH + 40];
    STARTUPINFOW startup = { 0 };
    PROCESS_INFORMATION process = { 0 };
    HANDLE pending_attach = CreateEventW(NULL, TRUE, FALSE, NULL);
    EXPECT_NOT_NULL(pending_attach);
    EXPECT_TRUE(GetModuleFileNameW(NULL, executable, MAX_PATH) != 0);
    EXPECT_TRUE(_snwprintf(command_line, MAX_PATH + 40, L"\"%ls\" --exit-before-attach", executable) > 0);
    startup.cb = sizeof(startup);
    EXPECT_TRUE(CreateProcessW(executable, command_line, NULL, NULL, FALSE, 0,
                                NULL, NULL, &startup, &process));
    EXPECT_TRUE(!ml_launcher_wait_for_remote_thread(process.hProcess, pending_attach, NULL));
    EXPECT_EQ(GetLastError(), ERROR_PROCESS_ABORTED);
    EXPECT_TRUE(CloseHandle(pending_attach));
    EXPECT_TRUE(CloseHandle(process.hThread));
    EXPECT_TRUE(CloseHandle(process.hProcess));
    return 0;
}

/* This executable stands in for YAFSML.dll. In the suspended child it runs
 * before the C runtime is initialized, so it may only call kernel32. */
__declspec(dllexport) DWORD WINAPI YAFSMLInit(LPVOID parameter) {
    (void)parameter;
    return GetEnvironmentVariableW(L"YAFSML_TEST_INIT_FAIL", NULL, 0) == 0 ? 1 : 0;
}

static int make_temp_dir(wchar_t path[MAX_PATH]) {
    wchar_t temp_dir[MAX_PATH];
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp_dir) != 0);
    EXPECT_TRUE(GetTempFileNameW(temp_dir, L"yaf", 0, path) != 0);
    EXPECT_TRUE(DeleteFileW(path));
    EXPECT_TRUE(CreateDirectoryW(path, NULL));
    return 0;
}

static int test_find_module(void) {
    const HANDLE process = GetCurrentProcess();
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    wchar_t path[MAX_PATH];
    wchar_t folder[MAX_PATH];
    wchar_t copy[MAX_PATH];
    HMODULE system_module;
    HMODULE copy_module;
    EXPECT_NOT_NULL(kernel32);
    EXPECT_TRUE(GetModuleFileNameW(kernel32, path, MAX_PATH) != 0);
    EXPECT_EQ(ml_launcher_find_remote_module(process, path), kernel32);
    for (wchar_t *c = path; *c != L'\0'; c++) *c = (wchar_t)towupper(*c);
    EXPECT_EQ(ml_launcher_find_remote_module(process, path), kernel32);
    /* Only the full path identifies a module; the base name alone is ambiguous. */
    EXPECT_NULL(ml_launcher_find_remote_module(process, L"kernel32.dll"));
    EXPECT_EQ(GetLastError(), ERROR_MOD_NOT_FOUND);
    EXPECT_NULL(ml_launcher_find_remote_module(process, L"C:\\YAFSML\\kernel32.dll"));
    EXPECT_TRUE(GetModuleFileNameW(NULL, path, MAX_PATH) != 0);
    EXPECT_EQ(ml_launcher_find_remote_module(process, path), GetModuleHandleW(NULL));

    /* Two loaded DLLs with one base name, like `-d ...\dxgi.dll` next to
     * System32's dxgi.dll, are told apart. */
    EXPECT_TRUE(GetSystemDirectoryW(path, MAX_PATH) != 0);
    EXPECT_TRUE(PathAppendW(path, L"version.dll"));
    EXPECT_EQ(make_temp_dir(folder), 0);
    EXPECT_NOT_NULL(PathCombineW(copy, folder, L"version.dll"));
    EXPECT_TRUE(CopyFileW(path, copy, FALSE));
    system_module = LoadLibraryW(path);
    copy_module = LoadLibraryExW(copy, NULL, DONT_RESOLVE_DLL_REFERENCES);
    EXPECT_NOT_NULL(system_module);
    EXPECT_NOT_NULL(copy_module);
    EXPECT_TRUE(system_module != copy_module);
    EXPECT_EQ(ml_launcher_find_remote_module(process, path), system_module);
    EXPECT_EQ(ml_launcher_find_remote_module(process, copy), copy_module);
    EXPECT_TRUE(FreeLibrary(copy_module));
    EXPECT_TRUE(FreeLibrary(system_module));
    EXPECT_TRUE(DeleteFileW(copy));
    EXPECT_TRUE(RemoveDirectoryW(folder));
    return 0;
}

static int test_proxy_copy(void) {
    wchar_t folder[MAX_PATH];
    wchar_t path[MAX_PATH];
    wchar_t source[MAX_PATH];
    wchar_t *found;
    const wchar_t *names[] = { L"dinput8.dll", L"dxgi.dll", L"winhttp.dll" };
    FILE *file;
    EXPECT_EQ(make_temp_dir(folder), 0);
    EXPECT_NULL(ml_launcher_find_proxy_copy_alloc(folder));

    /* Unloadable files and other DLLs with proxy names are not YAFSML copies. */
    EXPECT_NOT_NULL(PathCombineW(path, folder, L"winhttp.dll"));
    file = _wfopen(path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_EQ(fclose(file), 0);
    EXPECT_TRUE(GetSystemDirectoryW(source, MAX_PATH) != 0);
    EXPECT_TRUE(PathAppendW(source, L"version.dll"));
    EXPECT_NOT_NULL(PathCombineW(path, folder, L"dxgi.dll"));
    EXPECT_TRUE(CopyFileW(source, path, FALSE));
    EXPECT_NULL(ml_launcher_find_proxy_copy_alloc(folder));

    /* This executable exports YAFSMLInit like YAFSML.dll. */
    EXPECT_TRUE(GetModuleFileNameW(NULL, source, MAX_PATH) != 0);
    EXPECT_NOT_NULL(PathCombineW(path, folder, L"dinput8.dll"));
    EXPECT_TRUE(CopyFileW(source, path, FALSE));
    found = ml_launcher_find_proxy_copy_alloc(folder);
    EXPECT_NOT_NULL(found);
    EXPECT_STREQ_W(found, path);
    free(found);

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        EXPECT_NOT_NULL(PathCombineW(path, folder, names[i]));
        EXPECT_TRUE(DeleteFileW(path));
    }
    EXPECT_TRUE(RemoveDirectoryW(folder));
    return 0;
}

static int spawn_suspended_child(PROCESS_INFORMATION *process) {
    wchar_t executable[MAX_PATH];
    STARTUPINFOW startup = { 0 };
    EXPECT_TRUE(GetModuleFileNameW(NULL, executable, MAX_PATH) != 0);
    startup.cb = sizeof(startup);
    /* The child never resumes: only the injected threads run in it. */
    EXPECT_TRUE(CreateProcessW(executable, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED,
                                NULL, NULL, &startup, process));
    return 0;
}

static void end_child(PROCESS_INFORMATION *process) {
    TerminateProcess(process->hProcess, 0);
    WaitForSingleObject(process->hProcess, 10000);
    CloseHandle(process->hThread);
    CloseHandle(process->hProcess);
}

static int expect_injection(const wchar_t *dll_path, bool expected, DWORD expected_error) {
    PROCESS_INFORMATION process = { 0 };
    bool injected;
    DWORD error;
    EXPECT_EQ(spawn_suspended_child(&process), 0);
    injected = ml_launcher_inject_dll(process.hProcess, dll_path);
    error = GetLastError();
    end_child(&process);
    EXPECT_EQ(injected, expected);
    if (!expected) EXPECT_EQ(error, expected_error);
    return 0;
}

static int test_injection(void) {
    wchar_t executable[MAX_PATH];
    wchar_t folder[MAX_PATH];
    wchar_t path[MAX_PATH];
    EXPECT_TRUE(GetModuleFileNameW(NULL, executable, MAX_PATH) != 0);
    EXPECT_TRUE(SetEnvironmentVariableW(L"YAFSML_TEST_INIT_FAIL", NULL) ||
                GetLastError() == ERROR_ENVVAR_NOT_FOUND);
    EXPECT_EQ(expect_injection(executable, true, 0), 0);
    /* The child inherits the variable, so its YAFSMLInit returns zero. */
    EXPECT_TRUE(SetEnvironmentVariableW(L"YAFSML_TEST_INIT_FAIL", L"1"));
    EXPECT_EQ(expect_injection(executable, false, ERROR_DLL_INIT_FAILED), 0);
    EXPECT_TRUE(SetEnvironmentVariableW(L"YAFSML_TEST_INIT_FAIL", NULL));

    EXPECT_EQ(make_temp_dir(folder), 0);
    EXPECT_NOT_NULL(PathCombineW(path, folder, L"YAFSML.dll"));
    EXPECT_EQ(expect_injection(path, false, ERROR_MOD_NOT_FOUND), 0);
    EXPECT_TRUE(GetSystemDirectoryW(path, MAX_PATH) != 0);
    EXPECT_TRUE(PathAppendW(path, L"version.dll"));
    EXPECT_EQ(expect_injection(path, false, ERROR_PROC_NOT_FOUND), 0);

    EXPECT_TRUE(RemoveDirectoryW(folder));
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--exit-before-attach") == 0) return 0;
    EXPECT_EQ(test_wine_locale(), 0);
    EXPECT_EQ(test_plain_spawn_paths(), 0);
    EXPECT_EQ(test_remote_thread_wait(), 0);
    EXPECT_EQ(test_game_exit_before_attach(), 0);
    EXPECT_EQ(test_find_module(), 0);
    EXPECT_EQ(test_proxy_copy(), 0);
    EXPECT_EQ(test_injection(), 0);
    printf("smoke_launcher_process: all tests passed\n");
    return 0;
}
