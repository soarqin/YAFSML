#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <wchar.h>

#define LOG_STACK_CAPACITY 1024

volatile ml_log_level_t ml_log_current_level = ML_LOG_LEVEL_INFO;
static SRWLOCK console_lock = SRWLOCK_INIT;
static SRWLOCK file_lock = SRWLOCK_INIT;
static const wchar_t *level_names_w[] = { L"TRACE", L"DEBUG", L"INFO", L"WARN", L"ERROR", L"OFF" };
static FILE *log_stream = NULL;
static HANDLE console_output = INVALID_HANDLE_VALUE;
static ml_log_sink_t log_sink = NULL;

bool ml_log_level_parse(const char *value, ml_log_level_t *level) {
    static const char *names[] = { "trace", "debug", "info", "warn", "error", "off" };
    if (value == NULL || level == NULL) return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (_stricmp(value, names[i]) == 0) {
            *level = (ml_log_level_t)i;
            return true;
        }
    }
    return false;
}

const wchar_t *ml_log_level_name(ml_log_level_t level) {
    return level >= ML_LOG_LEVEL_TRACE && level <= ML_LOG_LEVEL_OFF ? level_names_w[level] : L"UNKNOWN";
}

void ml_log_enable_file(const wchar_t *path) {
    FILE *stream = path == NULL ? NULL : _wfopen(path, L"a, ccs=UTF-8");
    FILE *previous;
    if (stream == NULL) return;
    AcquireSRWLockExclusive(&file_lock);
    previous = log_stream;
    log_stream = stream;
    ReleaseSRWLockExclusive(&file_lock);
    if (previous != NULL) fclose(previous);
}

void ml_log_enable_console(void) {
    if (console_output != INVALID_HANDLE_VALUE) return;
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    freopen("CONIN$", "r", stdin);
    /* Write through the console API: the CRT streams convert through the C
     * locale and stop at the first character outside Latin-1. */
    console_output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

void ml_log_set_sink(ml_log_sink_t sink) {
    log_sink = sink;
}

void ml_log_set_level(ml_log_level_t level) {
    if (level < ML_LOG_LEVEL_TRACE || level > ML_LOG_LEVEL_OFF) return;
    ml_log_current_level = level;
}

ml_log_level_t ml_log_get_level(void) {
    return ml_log_current_level;
}

static void write_console(const wchar_t *text) {
    size_t length = wcslen(text);
    while (length != 0) {
        DWORD written = 0;
        DWORD chunk = length > 8192 ? 8192 : (DWORD)length;
        if (!WriteConsoleW(console_output, text, chunk, &written, NULL) || written == 0) return;
        text += written;
        length -= written;
    }
}

void ml_log_vwrite(ml_log_level_t level, const wchar_t *component,
                   const wchar_t *format, va_list args) {
    wchar_t stack[LOG_STACK_CAPACITY];
    wchar_t *message = stack;
    size_t capacity = LOG_STACK_CAPACITY;
    const ml_log_sink_t sink = log_sink;
    const bool console = console_output != INVALID_HANDLE_VALUE;
    const bool has_component = component != NULL && component[0] != L'\0';
    va_list copy;
    int length;
    if (format == NULL || (!console && sink == NULL && log_stream == NULL)) return;
    va_copy(copy, args);
    length = _vscwprintf(format, copy);
    va_end(copy);
    if (length < 0) return;
    if ((size_t)length >= capacity) {
        wchar_t *heap = HeapAlloc(GetProcessHeap(), 0, ((size_t)length + 1) * sizeof(*heap));
        /* Without memory the message is truncated rather than dropped. */
        if (heap != NULL) {
            message = heap;
            capacity = (size_t)length + 1;
        }
    }
    _vsnwprintf_s(message, capacity, _TRUNCATE, format, args);
    if (console) {
        AcquireSRWLockExclusive(&console_lock);
        write_console(ml_log_level_name(level));
        write_console(L": ");
        if (has_component) {
            write_console(L"[");
            write_console(component);
            write_console(L"] ");
        }
        write_console(message);
        write_console(L"\n");
        ReleaseSRWLockExclusive(&console_lock);
    }
    AcquireSRWLockExclusive(&file_lock);
    if (log_stream != NULL) {
        fwprintf(log_stream, L"%ls: ", ml_log_level_name(level));
        if (has_component) fwprintf(log_stream, L"[%ls] ", component);
        fputws(message, log_stream);
        fputwc(L'\n', log_stream);
        fflush(log_stream);
    }
    ReleaseSRWLockExclusive(&file_lock);
    if (sink != NULL) sink(level, component, message);
    if (message != stack) HeapFree(GetProcessHeap(), 0, message);
}

void ml_log_write(ml_log_level_t level, const wchar_t *component,
                  const wchar_t *format, ...) {
    va_list args;
    if (level < ml_log_current_level) return;
    va_start(args, format);
    ml_log_vwrite(level, component, format, args);
    va_end(args);
}
