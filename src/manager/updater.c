#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <miniz.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "updater.h"

typedef struct byte_buffer_s {
    char *data;
    size_t size;
    size_t capacity;
} byte_buffer_t;

static void set_error(wchar_t *error, size_t count, const wchar_t *message) {
    if (error != NULL && count != 0) lstrcpynW(error, message, (int)count);
}

static bool append_bytes(byte_buffer_t *buffer, const char *data, size_t size) {
    size_t needed;
    size_t capacity;
    char *new_data;
    if (size > SIZE_MAX - buffer->size - 1) return false;
    needed = buffer->size + size + 1;
    if (needed > buffer->capacity) {
        capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) return false;
            capacity *= 2;
        }
        new_data = (char *)realloc(buffer->data, capacity);
        if (new_data == NULL) return false;
        buffer->data = new_data;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return true;
}

static bool utf8_to_wide(const char *value, wchar_t *output, int output_count) {
    int result;
    result = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, output, output_count);
    if (result == 0) result = MultiByteToWideChar(CP_ACP, 0, value, -1, output, output_count);
    if (output_count > 0) output[output_count - 1] = L'\0';
    return result != 0;
}

static bool wide_to_utf8(const wchar_t *value, char *output, int output_count) {
    return WideCharToMultiByte(CP_UTF8, 0, value, -1, output, output_count, NULL, NULL) != 0;
}

static bool http_get(const wchar_t *host, const wchar_t *path, byte_buffer_t *body) {
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    DWORD available = 0;
    DWORD read = 0;
    char buffer[16384];
    bool ok = false;

    session = WinHttpOpen(L"YAFSML-Manager/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (session == NULL) goto done;
    connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection == NULL) goto done;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request == NULL) goto done;
    if (!WinHttpAddRequestHeaders(request,
            L"Accept: application/vnd.github+json\r\nUser-Agent: YAFSML-Manager\r\n",
            (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) goto done;
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0)) goto done;
    if (!WinHttpReceiveResponse(request, NULL)) goto done;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              NULL, &status, &status_size, NULL) || status < 200 || status >= 300) goto done;
    for (;;) {
        if (!WinHttpQueryDataAvailable(request, &available)) goto done;
        if (available == 0) break;
        while (available != 0) {
            DWORD chunk = available > sizeof(buffer) ? (DWORD)sizeof(buffer) : available;
            if (!WinHttpReadData(request, buffer, chunk, &read) || read == 0) goto done;
            if (!append_bytes(body, buffer, read)) goto done;
            available -= read;
        }
    }
    ok = true;
done:
    if (request != NULL) WinHttpCloseHandle(request);
    if (connection != NULL) WinHttpCloseHandle(connection);
    if (session != NULL) WinHttpCloseHandle(session);
    return ok;
}

static bool json_value(const char *object, const char *key, char *output, size_t output_size) {
    const char *key_start;
    const char *value_start;
    const char *value_end;
    size_t length;

    if (object == NULL || key == NULL || output == NULL || output_size == 0) return false;
    key_start = strstr(object, key);
    if (key_start == NULL) return false;
    value_start = key_start + strlen(key);
    while (*value_start != '\0' && (*value_start == ' ' || *value_start == '\t' ||
                                    *value_start == '\r' || *value_start == '\n')) {
        value_start++;
    }
    if (*value_start != ':') return false;
    value_start++;
    while (*value_start != '\0' && (*value_start == ' ' || *value_start == '\t' ||
                                    *value_start == '\r' || *value_start == '\n')) {
        value_start++;
    }
    if (*value_start != '"') return false;
    value_start++;
    value_end = strchr(value_start, '"');
    if (value_end == NULL) return false;
    length = (size_t)(value_end - value_start);
    if (length + 1 > output_size) return false;
    memcpy(output, value_start, length);
    output[length] = '\0';
    return true;
}

bool manager_latest_release(manager_release_t *release, wchar_t *error, size_t error_count) {
    byte_buffer_t body = { 0 };
    char tag[64] = { 0 };
    char asset_name[256] = { 0 };
    char asset_url[1024] = { 0 };
    char checksum_name[256] = { 0 };
    char checksum_url[1024] = { 0 };
    const char *cursor;
    if (release == NULL) return false;
    memset(release, 0, sizeof(*release));
    if (!http_get(L"api.github.com", L"/repos/soarqin/YAFSML/releases/latest", &body)) {
        set_error(error, error_count, L"Could not query the latest GitHub release.");
        free(body.data);
        return false;
    }
    if (!json_value(body.data, "\"tag_name\"", tag, sizeof(tag))) {
        set_error(error, error_count, L"The GitHub response did not contain a release tag.");
        free(body.data);
        return false;
    }
    cursor = body.data;
    while ((cursor = strstr(cursor, "\"name\"")) != NULL) {
        char parsed_name[sizeof(asset_name)] = { 0 };
        const char *asset_object;
        if (!json_value(cursor, "\"name\"", parsed_name, sizeof(parsed_name))) {
            cursor += strlen("\"name\"");
            continue;
        }
        lstrcpyA(asset_name, parsed_name);
        if (strncmp(asset_name, "YAFSML-", 7) == 0 && strstr(asset_name, ".zip") != NULL) {
            asset_object = strstr(cursor, "\"browser_download_url\"");
            if (asset_object != NULL && json_value(asset_object, "\"browser_download_url\"", asset_url, sizeof(asset_url))) break;
        }
        cursor += strlen("\"name\"");
    }
    if (asset_url[0] == '\0') {
        set_error(error, error_count, L"The latest release did not contain a YAFSML ZIP asset.");
        free(body.data);
        return false;
    }
    _snprintf_s(checksum_name, sizeof(checksum_name), _TRUNCATE, "%s.sha256", asset_name);
    cursor = body.data;
    while ((cursor = strstr(cursor, "\"name\"")) != NULL) {
        char parsed_name[sizeof(checksum_name)] = { 0 };
        const char *asset_object;
        if (!json_value(cursor, "\"name\"", parsed_name, sizeof(parsed_name))) {
            cursor += strlen("\"name\"");
            continue;
        }
        if (strcmp(parsed_name, checksum_name) == 0) {
            asset_object = strstr(cursor, "\"browser_download_url\"");
            if (asset_object != NULL) json_value(asset_object, "\"browser_download_url\"", checksum_url, sizeof(checksum_url));
            break;
        }
        cursor += strlen("\"name\"");
    }
    if (!utf8_to_wide(tag, release->tag, (int)(sizeof(release->tag) / sizeof(release->tag[0]))) ||
        !utf8_to_wide(asset_name, release->asset_name, (int)(sizeof(release->asset_name) / sizeof(release->asset_name[0]))) ||
        !utf8_to_wide(asset_url, release->asset_url, (int)(sizeof(release->asset_url) / sizeof(release->asset_url[0])))) {
        set_error(error, error_count, L"The GitHub response contained invalid text.");
        free(body.data);
        return false;
    }
    if (checksum_url[0] != '\0' && utf8_to_wide(checksum_url, release->checksum_url, (int)(sizeof(release->checksum_url) / sizeof(release->checksum_url[0]))) ) release->has_checksum = true;
    free(body.data);
    return true;
}

bool manager_latest_manager_release(manager_release_t *release, wchar_t *error, size_t error_count) {
    byte_buffer_t body = { 0 };
    const char *cursor;
    const char *release_end = NULL;
    char tag[64] = { 0 };
    char asset_name[256] = { 0 };
    char asset_url[1024] = { 0 };
    char checksum_url[1024] = { 0 };
    char checksum_name[256] = { 0 };
    if (release == NULL) return false;
    memset(release, 0, sizeof(*release));
    if (!http_get(L"api.github.com", L"/repos/soarqin/YAFSML/releases?per_page=100", &body)) {
        set_error(error, error_count, L"Could not query manager releases from GitHub.");
        free(body.data); return false;
    }
    cursor = body.data;
    while ((cursor = strstr(cursor, "\"tag_name\"")) != NULL) {
        char parsed_tag[sizeof(tag)] = { 0 };
        if (json_value(cursor, "\"tag_name\"", parsed_tag, sizeof(parsed_tag)) &&
            strncmp(parsed_tag, "manager-v", 9) == 0) {
            lstrcpyA(tag, parsed_tag);
            release_end = strstr(cursor + strlen("\"tag_name\""), "\"tag_name\"");
            break;
        }
        cursor += strlen("\"tag_name\"");
    }
    if (tag[0] == '\0') {
        set_error(error, error_count, L"No manager release was found.");
        free(body.data); return false;
    }
    cursor += strlen("\"tag_name\"");
    while ((cursor = strstr(cursor, "\"name\"")) != NULL && (release_end == NULL || cursor < release_end)) {
        char parsed_name[sizeof(asset_name)] = { 0 };
        const char *asset_object;
        if (!json_value(cursor, "\"name\"", parsed_name, sizeof(parsed_name))) {
            cursor += strlen("\"name\"");
            continue;
        }
        if ((strncmp(parsed_name, "YAFSML-Manager-", 15) == 0 ||
             strncmp(parsed_name, "YAFSML.Manager-", 15) == 0) &&
            strstr(parsed_name, ".zip") != NULL) {
            lstrcpyA(asset_name, parsed_name);
            asset_object = strstr(cursor, "\"browser_download_url\"");
            if (asset_object != NULL && json_value(asset_object, "\"browser_download_url\"", asset_url, sizeof(asset_url))) break;
        }
        cursor += strlen("\"name\"");
    }
    if (asset_url[0] == '\0') {
        set_error(error, error_count, L"The manager release did not contain a ZIP asset.");
        free(body.data); return false;
    }
    _snprintf_s(checksum_name, sizeof(checksum_name), _TRUNCATE, "%s.sha256", asset_name);
    cursor = body.data;
    while ((cursor = strstr(cursor, "\"name\"")) != NULL && (release_end == NULL || cursor < release_end)) {
        char parsed_name[sizeof(checksum_name)] = { 0 };
        const char *asset_object;
        if (!json_value(cursor, "\"name\"", parsed_name, sizeof(parsed_name))) {
            cursor += strlen("\"name\"");
            continue;
        }
        if (strcmp(parsed_name, checksum_name) == 0) {
            asset_object = strstr(cursor, "\"browser_download_url\"");
            if (asset_object != NULL) json_value(asset_object, "\"browser_download_url\"", checksum_url, sizeof(checksum_url));
            break;
        }
        cursor += strlen("\"name\"");
    }
    if (!utf8_to_wide(tag, release->tag, (int)(sizeof(release->tag) / sizeof(release->tag[0]))) ||
        !utf8_to_wide(asset_name, release->asset_name, (int)(sizeof(release->asset_name) / sizeof(release->asset_name[0]))) ||
        !utf8_to_wide(asset_url, release->asset_url, (int)(sizeof(release->asset_url) / sizeof(release->asset_url[0])))) {
        set_error(error, error_count, L"The GitHub response contained invalid manager release text."); free(body.data); return false;
    }
    if (checksum_url[0] != '\0' && utf8_to_wide(checksum_url, release->checksum_url, (int)(sizeof(release->checksum_url) / sizeof(release->checksum_url[0]))) ) release->has_checksum = true;
    free(body.data); return true;
}

static bool download_file(const wchar_t *url, const wchar_t *destination) {
    URL_COMPONENTS components = { sizeof(components) };
    wchar_t host[256] = { 0 };
    wchar_t path[2048] = { 0 };
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD available = 0;
    DWORD read = 0;
    DWORD written = 0;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    unsigned char buffer[65536];
    bool ok = false;
    components.lpszHostName = host;
    components.dwHostNameLength = sizeof(host) / sizeof(host[0]);
    components.lpszUrlPath = path;
    components.dwUrlPathLength = sizeof(path) / sizeof(path[0]);
    if (!WinHttpCrackUrl(url, 0, 0, &components)) return false;
    session = WinHttpOpen(L"YAFSML-Manager/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (session == NULL) goto done;
    connection = WinHttpConnect(session, host, components.nPort, 0);
    if (connection == NULL) goto done;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (request == NULL || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) || !WinHttpReceiveResponse(request, NULL) ||
        !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &status_size, NULL) || status < 200 || status >= 300) goto done;
    file = CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    for (;;) {
        if (!WinHttpQueryDataAvailable(request, &available)) goto done;
        if (available == 0) break;
        while (available != 0) {
            DWORD chunk = available > sizeof(buffer) ? (DWORD)sizeof(buffer) : available;
            if (!WinHttpReadData(request, buffer, chunk, &read) || read == 0) goto done;
            if (!WriteFile(file, buffer, read, &written, NULL) || written != read) goto done;
            available -= read;
        }
    }
    ok = true;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (request != NULL) WinHttpCloseHandle(request);
    if (connection != NULL) WinHttpCloseHandle(connection);
    if (session != NULL) WinHttpCloseHandle(session);
    return ok;
}

static bool sha256_file(const wchar_t *path, unsigned char digest[32]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned char *object = NULL;
    unsigned char buffer[65536];
    DWORD object_size = 0;
    DWORD result = 0;
    DWORD read = 0;
    bool ok = false;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size, sizeof(object_size), &result, 0) != 0) goto done;
    object = (unsigned char *)malloc(object_size);
    if (object == NULL || BCryptCreateHash(algorithm, &hash, object, object_size, NULL, 0, 0) != 0) goto done;
    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL)) goto done;
        if (read == 0) break;
        if (BCryptHashData(hash, buffer, read, 0) != 0) goto done;
    }
    if (BCryptFinishHash(hash, digest, 32, 0) != 0) goto done;
    ok = true;
done:
    if (hash != NULL) BCryptDestroyHash(hash);
    if (algorithm != NULL) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(object);
    return ok;
}

static int hex_digit_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

/* GitHub checksum assets commonly contain either a bare digest or
 * "<digest>  <filename>". Only the first 64 hexadecimal characters are
 * significant; surrounding whitespace and the filename are ignored. */
static bool read_sha256_asset(const wchar_t *path, unsigned char digest[32]) {
    FILE *file;
    char text[4096];
    size_t length;
    size_t offset = 0;
    size_t i;
    int high;
    int low;

    if (path == NULL || digest == NULL) return false;
    file = _wfopen(path, L"rb");
    if (file == NULL) return false;
    length = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[length] = '\0';
    while (offset < length && (text[offset] == ' ' || text[offset] == '\t' ||
                               text[offset] == '\r' || text[offset] == '\n')) {
        offset++;
    }
    if (length - offset < 64) return false;
    for (i = 0; i < 32; i++) {
        high = hex_digit_value(text[offset + i * 2]);
        low = hex_digit_value(text[offset + i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        digest[i] = (unsigned char)((high << 4) | low);
    }
    return true;
}

static bool safe_archive_path(const char *name) {
    const char *cursor;
    if (name == NULL || name[0] == '\0' || name[0] == '/' || name[0] == '\\' ||
        strchr(name, '\\') != NULL || (name[0] != '\0' && name[1] == ':')) return false;
    cursor = name;
    while (*cursor != '\0') {
        const char *start = cursor;
        size_t length;
        while (*cursor != '\0' && *cursor != '/') cursor++;
        length = (size_t)(cursor - start);
        if (length == 0 || (length == 1 && start[0] == '.') ||
            (length == 2 && start[0] == '.' && start[1] == '.')) return false;
        if (*cursor == '/') cursor++;
    }
    return true;
}

static bool extract_loader_files(const wchar_t *zip_path, const wchar_t *directory) {
    char zip_utf8[MAX_PATH];
    char output_utf8[MAX_PATH];
    mz_zip_archive archive;
    mz_uint index;
    mz_uint count;
    bool found_exe = false;
    bool found_dll = false;
    if (!wide_to_utf8(zip_path, zip_utf8, sizeof(zip_utf8))) return false;
    mz_zip_zero_struct(&archive);
    if (!mz_zip_reader_init_file(&archive, zip_utf8, 0) || !mz_zip_validate_archive(&archive, 0)) {
        mz_zip_end(&archive);
        return false;
    }
    count = mz_zip_reader_get_num_files(&archive);
    for (index = 0; index < count; index++) {
        mz_zip_archive_file_stat stat;
        wchar_t output[MAX_PATH];
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) { mz_zip_end(&archive); return false; }
        if (stat.m_is_directory) continue;
        if (!safe_archive_path(stat.m_filename)) { mz_zip_end(&archive); return false; }
        if (strcmp(stat.m_filename, "YAFSML.exe") != 0 && strcmp(stat.m_filename, "YAFSML.dll") != 0) continue;
        if (strcmp(stat.m_filename, "YAFSML.exe") == 0) {
            if (found_exe) { mz_zip_end(&archive); return false; }
            found_exe = true;
        } else {
            if (found_dll) { mz_zip_end(&archive); return false; }
            found_dll = true;
        }
        lstrcpynW(output, directory, MAX_PATH);
        PathAppendW(output, strcmp(stat.m_filename, "YAFSML.exe") == 0 ? L"YAFSML.exe" : L"YAFSML.dll");
        if (!wide_to_utf8(output, output_utf8, sizeof(output_utf8)) || !mz_zip_reader_extract_to_file(&archive, index, output_utf8, 0)) { mz_zip_end(&archive); return false; }
    }
    mz_zip_end(&archive);
    return found_exe && found_dll;
}

static bool extract_manager_file(const wchar_t *zip_path, const wchar_t *directory) {
    char zip_utf8[MAX_PATH], output_utf8[MAX_PATH];
    mz_zip_archive archive;
    mz_uint index, count;
    bool found = false;
    if (!wide_to_utf8(zip_path, zip_utf8, sizeof(zip_utf8))) return false;
    mz_zip_zero_struct(&archive);
    if (!mz_zip_reader_init_file(&archive, zip_utf8, 0) || !mz_zip_validate_archive(&archive, 0)) { mz_zip_end(&archive); return false; }
    count = mz_zip_reader_get_num_files(&archive);
    for (index = 0; index < count; index++) {
        mz_zip_archive_file_stat stat;
        wchar_t output[MAX_PATH];
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) { mz_zip_end(&archive); return false; }
        if (stat.m_is_directory) continue;
        if (!safe_archive_path(stat.m_filename)) { mz_zip_end(&archive); return false; }
        if (strcmp(stat.m_filename, "YAFSML.Manager.exe") != 0) continue;
        if (found) { mz_zip_end(&archive); return false; }
        found = true;
        lstrcpynW(output, directory, MAX_PATH); PathAppendW(output, L"YAFSML.Manager.exe");
        if (!wide_to_utf8(output, output_utf8, sizeof(output_utf8)) || !mz_zip_reader_extract_to_file(&archive, index, output_utf8, 0)) { mz_zip_end(&archive); return false; }
    }
    mz_zip_end(&archive);
    return found;
}

static void powershell_quote(const wchar_t *value, wchar_t *output, size_t count) {
    size_t i, at = 0;
    if (count == 0) return;
    output[at++] = L'\'';
    for (i = 0; value != NULL && value[i] != L'\0' && at + 2 < count; i++) {
        if (value[i] == L'\'') output[at++] = L'\'';
        output[at++] = value[i];
    }
    if (at + 1 < count) output[at++] = L'\'';
    output[at] = L'\0';
}

static bool launch_manager_replacer(const wchar_t *manager_path, const wchar_t *source_path, const wchar_t *cleanup_dir, wchar_t *error, size_t error_count) {
    wchar_t manager_q[MAX_PATH * 2], source_q[MAX_PATH * 2], cleanup_q[MAX_PATH * 2], script[8192], command[9216];
    SHELLEXECUTEINFOW execute = { sizeof(execute) };
    powershell_quote(manager_path, manager_q, sizeof(manager_q) / sizeof(manager_q[0]));
    powershell_quote(source_path, source_q, sizeof(source_q) / sizeof(source_q[0]));
    powershell_quote(cleanup_dir, cleanup_q, sizeof(cleanup_q) / sizeof(cleanup_q[0]));
    _snwprintf(script, sizeof(script) / sizeof(script[0]), L"$targetPid=%lu; while(Get-Process -Id $targetPid -ErrorAction SilentlyContinue){Start-Sleep -Milliseconds 300}; $backup=%ls+'.update.bak'; try { if(Test-Path -LiteralPath %ls){Move-Item -LiteralPath %ls -Destination $backup -Force}; Move-Item -LiteralPath %ls -Destination %ls -Force; Start-Process -FilePath %ls; Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue } catch { if(Test-Path -LiteralPath $backup){Move-Item -LiteralPath $backup -Destination %ls -Force} }; Remove-Item -LiteralPath %ls -Recurse -Force -ErrorAction SilentlyContinue", (unsigned long)GetCurrentProcessId(), manager_q, manager_q, manager_q, source_q, manager_q, manager_q, manager_q, cleanup_q);
    _snwprintf(command, sizeof(command) / sizeof(command[0]), L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command \"%ls\"", script);
    execute.lpFile = L"powershell.exe";
    execute.lpParameters = command;
    execute.nShow = SW_HIDE;
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&execute)) { set_error(error, error_count, L"Could not start the manager replacement helper."); return false; }
    if (execute.hProcess != NULL) CloseHandle(execute.hProcess);
    return true;
}

bool manager_update_manager(const wchar_t *manager_path, const manager_release_t *release, wchar_t *error, size_t error_count) {
    wchar_t temp_dir[MAX_PATH] = L"", temp_path[MAX_PATH] = L"", checksum_path[MAX_PATH] = L"", extract_dir[MAX_PATH] = L"", source_path[MAX_PATH];
    unsigned char digest[32], expected_digest[32];
    if (manager_path == NULL || release == NULL || release->asset_url[0] == L'\0' || !release->has_checksum) { set_error(error, error_count, L"The manager release is missing a signed ZIP asset."); return false; }
    if (GetTempPathW(MAX_PATH, temp_dir) == 0 || GetTempFileNameW(temp_dir, L"ymg", 0, temp_path) == 0) { set_error(error, error_count, L"Could not create a temporary manager download path."); return false; }
    DeleteFileW(temp_path);
    if (!download_file(release->asset_url, temp_path) || !sha256_file(temp_path, digest) || GetTempFileNameW(temp_dir, L"ymc", 0, checksum_path) == 0 || !download_file(release->checksum_url, checksum_path) || !read_sha256_asset(checksum_path, expected_digest) || memcmp(digest, expected_digest, sizeof(digest)) != 0) {
        set_error(error, error_count, L"The downloaded manager failed SHA-256 verification."); DeleteFileW(temp_path); DeleteFileW(checksum_path); return false;
    }
    DeleteFileW(checksum_path);
    if (GetTempFileNameW(temp_dir, L"yme", 0, extract_dir) == 0) { DeleteFileW(temp_path); return false; }
    DeleteFileW(extract_dir);
    if (!CreateDirectoryW(extract_dir, NULL) || !extract_manager_file(temp_path, extract_dir)) { set_error(error, error_count, L"The manager ZIP failed validation."); RemoveDirectoryW(extract_dir); DeleteFileW(temp_path); return false; }
    lstrcpynW(source_path, extract_dir, MAX_PATH); PathAppendW(source_path, L"YAFSML.Manager.exe");
    if (!launch_manager_replacer(manager_path, source_path, extract_dir, error, error_count)) { RemoveDirectoryW(extract_dir); DeleteFileW(temp_path); return false; }
    DeleteFileW(temp_path);
    return true;
}

bool manager_update_loader(const wchar_t *target_dir, const manager_release_t *release, wchar_t *error, size_t error_count) {
    wchar_t temp_dir[MAX_PATH] = { 0 };
    wchar_t temp_path[MAX_PATH] = { 0 };
    wchar_t checksum_path[MAX_PATH] = { 0 };
    wchar_t extract_dir[MAX_PATH] = { 0 };
    wchar_t old_exe[MAX_PATH], old_dll[MAX_PATH], backup_exe[MAX_PATH], backup_dll[MAX_PATH];
    unsigned char digest[32];
    unsigned char expected_digest[32];
    bool have_old_exe;
    bool have_old_dll;
    if (target_dir == NULL || release == NULL || release->asset_url[0] == L'\0') return false;
    if (!release->has_checksum) { set_error(error, error_count, L"The release has no SHA-256 asset."); return false; }
    if (GetTempPathW(MAX_PATH, temp_dir) == 0 || GetTempFileNameW(temp_dir, L"yaf", 0, temp_path) == 0) { set_error(error, error_count, L"Could not create a temporary download path."); return false; }
    DeleteFileW(temp_path);
    if (!download_file(release->asset_url, temp_path) || !sha256_file(temp_path, digest)) { set_error(error, error_count, L"Could not download or hash the release."); DeleteFileW(temp_path); return false; }
    if (GetTempFileNameW(temp_dir, L"sum", 0, checksum_path) == 0) {
        set_error(error, error_count, L"Could not create a temporary checksum path.");
        DeleteFileW(temp_path);
        return false;
    }
    if (!download_file(release->checksum_url, checksum_path) ||
        !read_sha256_asset(checksum_path, expected_digest) ||
        memcmp(digest, expected_digest, sizeof(digest)) != 0) {
        set_error(error, error_count, L"The downloaded release failed SHA-256 verification.");
        DeleteFileW(checksum_path);
        DeleteFileW(temp_path);
        return false;
    }
    DeleteFileW(checksum_path);
    if (GetTempFileNameW(temp_dir, L"ext", 0, extract_dir) == 0) { DeleteFileW(temp_path); return false; }
    DeleteFileW(extract_dir);
    if (!CreateDirectoryW(extract_dir, NULL) || !extract_loader_files(temp_path, extract_dir)) { RemoveDirectoryW(extract_dir); DeleteFileW(temp_path); set_error(error, error_count, L"The release ZIP failed validation."); return false; }
    lstrcpynW(old_exe, target_dir, MAX_PATH); PathAppendW(old_exe, L"YAFSML.exe");
    lstrcpynW(old_dll, target_dir, MAX_PATH); PathAppendW(old_dll, L"YAFSML.dll");
    lstrcpynW(backup_exe, old_exe, MAX_PATH); lstrcatW(backup_exe, L".update.bak");
    lstrcpynW(backup_dll, old_dll, MAX_PATH); lstrcatW(backup_dll, L".update.bak");
    have_old_exe = PathFileExistsW(old_exe) != FALSE;
    have_old_dll = PathFileExistsW(old_dll) != FALSE;
    if (have_old_exe && !MoveFileExW(old_exe, backup_exe, MOVEFILE_REPLACE_EXISTING)) goto rollback;
    if (have_old_dll && !MoveFileExW(old_dll, backup_dll, MOVEFILE_REPLACE_EXISTING)) goto rollback;
    {
        wchar_t source[MAX_PATH];
        lstrcpynW(source, extract_dir, MAX_PATH); PathAppendW(source, L"YAFSML.exe");
        if (!MoveFileExW(source, old_exe, MOVEFILE_REPLACE_EXISTING)) goto rollback;
        lstrcpynW(source, extract_dir, MAX_PATH); PathAppendW(source, L"YAFSML.dll");
        if (!MoveFileExW(source, old_dll, MOVEFILE_REPLACE_EXISTING)) goto rollback;
    }
    DeleteFileW(backup_exe); DeleteFileW(backup_dll); RemoveDirectoryW(extract_dir); DeleteFileW(temp_path);
    return true;
rollback:
    DeleteFileW(old_exe); DeleteFileW(old_dll);
    if (have_old_exe) MoveFileExW(backup_exe, old_exe, MOVEFILE_REPLACE_EXISTING);
    if (have_old_dll) MoveFileExW(backup_dll, old_dll, MOVEFILE_REPLACE_EXISTING);
    RemoveDirectoryW(extract_dir); DeleteFileW(temp_path);
    set_error(error, error_count, L"The loader files are in use or could not be replaced.");
    return false;
}
