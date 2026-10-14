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

static HMODULE WINAPI test_get_module_handle(LPCWSTR name) {
    return wcscmp(name, L"ntdll.dll") == 0 ? (HMODULE)(uintptr_t)1 : NULL;
}

static FARPROC WINAPI test_get_proc_address(HMODULE module, LPCSTR name) {
    (void)module;
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
        command_line == NULL && process_attributes == NULL && thread_attributes == NULL &&
        !inherit_handles && creation_flags == CREATE_SUSPENDED && environment == NULL &&
        wcscmp(working_directory, L"C:\\模组") == 0 && startup->cb == sizeof(*startup) &&
        process != NULL && (!test_expect_locale_cleared || !test_locale_present);
    SetLastError(test_spawn_result ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
    return test_spawn_result;
}

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

static int spawn_with_test_locale(void) {
    STARTUPINFOW startup = { 0 };
    PROCESS_INFORMATION process = { 0 };
    startup.cb = sizeof(startup);
    EXPECT_EQ(ml_launcher_create_game_process(L"C:\\模组\\game.exe", L"C:\\模组",
                                               &startup, &process), test_spawn_result);
    EXPECT_EQ(GetLastError(), test_spawn_result ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
    EXPECT_TRUE(test_spawn_arguments_valid);
    EXPECT_EQ(test_spawns, 1);
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
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    EXPECT_NOT_NULL(process);
    HANDLE thread = CreateThread(NULL, 0, completed_thread, (void *)(uintptr_t)1, 0, NULL);
    EXPECT_NOT_NULL(thread);
    EXPECT_TRUE(ml_launcher_wait_for_remote_thread(process, thread));
    EXPECT_TRUE(!ml_launcher_wait_for_remote_thread(NULL, thread));
    EXPECT_EQ(GetLastError(), ERROR_INVALID_HANDLE);

    HANDLE exited = CreateEventW(NULL, TRUE, TRUE, NULL);
    EXPECT_NOT_NULL(exited);
    /* Both handles are signaled: game exit must take precedence. */
    EXPECT_TRUE(!ml_launcher_wait_for_remote_thread(exited, thread));
    EXPECT_EQ(GetLastError(), ERROR_PROCESS_ABORTED);
    EXPECT_TRUE(CloseHandle(exited));
    EXPECT_TRUE(CloseHandle(thread));

    thread = CreateThread(NULL, 0, completed_thread, NULL, 0, NULL);
    EXPECT_NOT_NULL(thread);
    EXPECT_TRUE(!ml_launcher_wait_for_remote_thread(process, thread));
    EXPECT_EQ(GetLastError(), ERROR_DLL_INIT_FAILED);
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
    EXPECT_TRUE(!ml_launcher_wait_for_remote_thread(process.hProcess, pending_attach));
    EXPECT_EQ(GetLastError(), ERROR_PROCESS_ABORTED);
    EXPECT_TRUE(CloseHandle(pending_attach));
    EXPECT_TRUE(CloseHandle(process.hThread));
    EXPECT_TRUE(CloseHandle(process.hProcess));
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--exit-before-attach") == 0) return 0;
    EXPECT_EQ(test_wine_locale(), 0);
    EXPECT_EQ(test_remote_thread_wait(), 0);
    EXPECT_EQ(test_game_exit_before_attach(), 0);
    printf("smoke_launcher_process: all tests passed\n");
    return 0;
}
