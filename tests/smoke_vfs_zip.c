#include <stdio.h>

#include <windows.h>
#include <shlwapi.h>
#include <miniz.h>

#include "test_common.h"
#include "test_vfs.h"
#include "modloader/vfs.h"
#include "modloader/zip_package.h"

static size_t allocation_failure_after;
static bool allocation_failure_triggered;
static bool track_allocations;
static size_t allocated_bytes;

static bool fail_allocation(void) {
    if (allocation_failure_after == 0 || --allocation_failure_after != 0) return false;
    allocation_failure_triggered = true;
    return true;
}

void *ml_mem_alloc(UINT flags, size_t size) {
    if (fail_allocation()) return NULL;
    if (track_allocations) allocated_bytes += size;
    return flags & LMEM_ZEROINIT ? calloc(1, size) : malloc(size);
}

void *ml_mem_realloc(void *pointer, size_t size, UINT flags) {
    if (fail_allocation()) return NULL;
    if (track_allocations) allocated_bytes += size;
    return flags & LMEM_ZEROINIT ? _recalloc(pointer, 1, size) : realloc(pointer, size);
}

void ml_mem_free(void *pointer) { free(pointer); }

char *ml_mem_strdup_a(const char *value) {
    return fail_allocation() ? NULL : _strdup(value);
}

wchar_t *ml_mem_strdup_w(const wchar_t *value) {
    return fail_allocation() ? NULL : _wcsdup(value);
}

typedef struct fixture_entry_s {
    const char *name;
    const char *contents;
    mz_uint level;
} fixture_entry_t;

static int write_zip(const wchar_t *path, const fixture_entry_t *entries, size_t count, mz_uint flags) {
    FILE *file = _wfopen(path, L"wb");
    mz_zip_archive archive = { 0 };
    EXPECT_NOT_NULL(file);
    EXPECT_TRUE(mz_zip_writer_init_cfile(&archive, file, flags));
    for (size_t i = 0; i < count; i++) {
        EXPECT_TRUE(mz_zip_writer_add_mem(&archive, entries[i].name, entries[i].contents,
                                        strlen(entries[i].contents), entries[i].level));
    }
    EXPECT_TRUE(mz_zip_writer_finalize_archive(&archive));
    EXPECT_TRUE(mz_zip_writer_end(&archive));
    EXPECT_EQ(fclose(file), 0);
    return 0;
}

static bool read_matches(const wchar_t *path, const char *contents) {
    char data[256];
    size_t read;
    return test_vfs_read(path, data, sizeof(data), &read) &&
           read == strlen(contents) && memcmp(data, contents, read) == 0;
}

static int test_allocation_rollback(const wchar_t *path, const wchar_t *loose) {
    static const fixture_entry_t entries[] = {
        { "parts/new.bin", "first new", 0 },
        { "parts/base.bin", "replacement", 0 },
        { "PARTS/NEW.bin", "second new", 6 },
        { "parts/last.bin", "last", 0 },
    };
    bool success = false;
    EXPECT_EQ(write_zip(path, entries, 4, 0), 0);
    for (size_t allocation = 1; allocation < 256; allocation++) {
        const wchar_t *uid;
        vfs_init();
        EXPECT_TRUE(vfs_add_package(loose));
        uid = vfs_virtual_to_uid(L"parts/base.bin");
        EXPECT_NOT_NULL(uid);
        allocation_failure_after = allocation;
        allocation_failure_triggered = false;
        success = vfs_add_package(path);
        allocation_failure_after = 0;
        if (success) {
            EXPECT_EQ(vfs_entry_count(), 3);
            EXPECT_TRUE(read_matches(vfs_uid_to_path(uid), "replacement"));
            EXPECT_TRUE(read_matches(vfs_lookup(L"parts/new.bin"), "second new"));
            EXPECT_TRUE(read_matches(vfs_lookup(L"parts/last.bin"), "last"));
        } else {
            EXPECT_EQ(vfs_entry_count(), 1);
            EXPECT_TRUE(read_matches(vfs_uid_to_path(uid), "loose"));
            EXPECT_TRUE(read_matches(vfs_lookup(L"parts/base.bin"), "loose"));
            EXPECT_NULL(vfs_lookup(L"parts/new.bin"));
            EXPECT_NULL(vfs_lookup(L"parts/last.bin"));
        }
        vfs_uninit();
        if (success && !allocation_failure_triggered) break;
    }
    EXPECT_TRUE(success);
    return 0;
}

static bool write_text_file(const wchar_t *directory, const wchar_t *name, const char *contents) {
    wchar_t path[MAX_PATH];
    DWORD written;
    HANDLE file;
    bool success;
    _snwprintf(path, MAX_PATH, L"%ls\\%ls", directory, name);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    success = WriteFile(file, contents, (DWORD)strlen(contents), &written, NULL) && written == strlen(contents);
    CloseHandle(file);
    return success;
}

/* A directory package is indexed completely or not at all, like a ZIP. */
static int test_directory_rollback(const wchar_t *root, const wchar_t *loose) {
    wchar_t package[MAX_PATH];
    wchar_t parts[MAX_PATH];
    bool success = false;
    _snwprintf(package, MAX_PATH, L"%ls\\directory-package", root);
    _snwprintf(parts, MAX_PATH, L"%ls\\parts", package);
    EXPECT_TRUE(CreateDirectoryW(package, NULL));
    EXPECT_TRUE(CreateDirectoryW(parts, NULL));
    EXPECT_TRUE(write_text_file(parts, L"new.bin", "dir new"));
    EXPECT_TRUE(write_text_file(parts, L"base.bin", "dir replacement"));
    EXPECT_TRUE(write_text_file(parts, L"last.bin", "dir last"));
    for (size_t allocation = 1; allocation < 256; allocation++) {
        const wchar_t *uid;
        vfs_init();
        EXPECT_TRUE(vfs_add_package(loose));
        uid = vfs_virtual_to_uid(L"parts/base.bin");
        EXPECT_NOT_NULL(uid);
        allocation_failure_after = allocation;
        allocation_failure_triggered = false;
        success = vfs_add_package(package);
        allocation_failure_after = 0;
        if (success) {
            EXPECT_EQ(vfs_entry_count(), 3);
            EXPECT_TRUE(read_matches(vfs_uid_to_path(uid), "dir replacement"));
            EXPECT_TRUE(read_matches(vfs_lookup(L"parts/new.bin"), "dir new"));
            EXPECT_TRUE(read_matches(vfs_lookup(L"parts/last.bin"), "dir last"));
        } else {
            EXPECT_EQ(vfs_entry_count(), 1);
            EXPECT_TRUE(read_matches(vfs_uid_to_path(uid), "loose"));
            EXPECT_NULL(vfs_lookup(L"parts/new.bin"));
            EXPECT_NULL(vfs_lookup(L"parts/last.bin"));
        }
        vfs_uninit();
        if (success && !allocation_failure_triggered) break;
    }
    EXPECT_TRUE(success);
    for (size_t i = 0; i < 3; i++) {
        static const wchar_t *names[] = { L"new.bin", L"base.bin", L"last.bin" };
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", parts, names[i]);
        EXPECT_TRUE(DeleteFileW(path));
    }
    EXPECT_TRUE(RemoveDirectoryW(parts));
    EXPECT_TRUE(RemoveDirectoryW(package));
    return 0;
}

static volatile LONG thread_failures;
static const wchar_t *thread_paths[8];
static const wchar_t *thread_uid;

static DWORD WINAPI first_read_worker(void *parameter) {
    size_t thread = (size_t)parameter;
    const wchar_t *path = thread % 2 == 0 ? vfs_uid_to_path(thread_uid) : vfs_lookup(L"parts/thread.bin");
    if (!read_matches(path, "shared compressed contents")) InterlockedIncrement(&thread_failures);
    thread_paths[thread] = path;
    return 0;
}

static int test_failed_read_retry(const wchar_t *path) {
    static const fixture_entry_t entries[] = { { "retry.bin", "correct", 0 } };
    mz_zip_archive archive = { 0 };
    mz_zip_archive_file_stat stat;
    mz_uint64 data_offset;
    FILE *file;
    zip_package_t *package;
    char data[16];
    size_t read;
    EXPECT_EQ(write_zip(path, entries, 1, 0), 0);
    file = _wfopen(path, L"r+b");
    EXPECT_NOT_NULL(file);
    EXPECT_TRUE(mz_zip_reader_init_cfile(&archive, file, 0, 0));
    EXPECT_TRUE(mz_zip_reader_file_stat(&archive, 0, &stat));
    data_offset = stat.m_local_header_ofs + 30 + strlen(entries[0].name);
    EXPECT_TRUE(mz_zip_reader_end(&archive));
    EXPECT_EQ(_fseeki64(file, (mz_int64)data_offset, SEEK_SET), 0);
    EXPECT_TRUE(fputc('x', file) != EOF);
    EXPECT_EQ(fclose(file), 0);

    vfs_init();
    EXPECT_TRUE(vfs_add_package(path));
    vfs_reset_lookup_caches();
    for (size_t i = 0; i < 2; i++) {
        vfs_stream_t *stream = vfs_stream_open(vfs_lookup(L"retry.bin"));
        EXPECT_NOT_NULL(stream);
        EXPECT_TRUE(!vfs_stream_read(stream, data, sizeof(data), &read));
        EXPECT_EQ(GetLastError(), ERROR_CRC);
        vfs_stream_close(stream);
    }
    EXPECT_NOT_NULL(vfs_uid_to_path(vfs_virtual_to_uid(L"retry.bin")));
    vfs_uninit();
    EXPECT_EQ(write_zip(path, entries, 1, 0), 0);
    package = zip_package_open(path, L"");
    EXPECT_NOT_NULL(package);
    /* Allocation failure is retryable and a stream retains its source archive. */
    allocation_failure_after = 1;
    EXPECT_NULL(zip_stream_open(package, 0));
    allocation_failure_after = 0;
    {
        zip_stream_t *stream = zip_stream_open(package, 0);
        EXPECT_NOT_NULL(stream);
        zip_package_close(package);
        EXPECT_TRUE(zip_stream_read_at(stream, 0, data, sizeof(data), &read));
        EXPECT_EQ(read, 7);
        EXPECT_TRUE(memcmp(data, "correct", 7) == 0);
        zip_stream_close(stream);
    }
    return 0;
}

static int test_nested_roots(const wchar_t *path) {
    static const fixture_entry_t entries[] = {
        { "def/subdir/mod/parts/nested.bin", "nested", 6 },
        { "def/subdir/mod/action/script/player.hks", "nested script", 6 },
        { "def/subdir/mod-extra/parts/leak.bin", "adjacent root", 0 },
        { "def/subdir/parts/leak.bin", "parent root", 0 },
    };
    static const wchar_t *suffixes[] = {
        L":def/subdir/mod",
        L":/def/subdir/mod",
        L":def\\subdir\\mod",
        L":\\def\\subdir\\mod",
        L":/def\\subdir\\mod",
        L":\\def/subdir\\mod",
        L":\\DEF\\SUBDIR\\MOD\\",
    };
    wchar_t spec[MAX_PATH * 2];
    EXPECT_EQ(write_zip(path, entries, sizeof(entries) / sizeof(entries[0]), 0), 0);
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        _snwprintf(spec, MAX_PATH * 2, L"%ls%ls", path, suffixes[i]);
        vfs_init();
        EXPECT_TRUE(vfs_add_package(spec));
        EXPECT_EQ(vfs_entry_count(), 2);
        EXPECT_TRUE(read_matches(vfs_lookup(L"parts\\nested.bin"), "nested"));
        EXPECT_TRUE(read_matches(vfs_lookup(L"action/script\\player.hks"), "nested script"));
        EXPECT_TRUE(read_matches(vfs_uid_to_path(vfs_virtual_to_uid(L"parts/nested.bin")), "nested"));
        EXPECT_NULL(vfs_lookup(L"parts/leak.bin"));
        EXPECT_NULL(vfs_lookup(L"def/subdir/mod/parts/nested.bin"));
        vfs_uninit();
    }
    return 0;
}

static int test_large_streams(const wchar_t *path) {
    const size_t length = 3 * 1024 * 1024 + 179;
    unsigned char *expected = malloc(length);
    unsigned char *buffer = malloc(70001);
    uint32_t random = 12345;
    mz_zip_archive archive = { 0 };
    FILE *file;
    wchar_t old_temp[32768], old_tmp[32768];
    DWORD temp_length = GetEnvironmentVariableW(L"TEMP", old_temp, 32768);
    DWORD tmp_length = GetEnvironmentVariableW(L"TMP", old_tmp, 32768);
    EXPECT_NOT_NULL(expected);
    EXPECT_NOT_NULL(buffer);
    for (size_t i = 0; i < length; i++) {
        random = random * 1664525 + 1013904223;
        expected[i] = (i / 4096) % 2 ? (unsigned char)(random >> 24) : (unsigned char)(i % 29);
    }
    file = _wfopen(path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_TRUE(mz_zip_writer_init_cfile(&archive, file, MZ_ZIP_FLAG_WRITE_ZIP64));
    EXPECT_TRUE(mz_zip_writer_add_mem(&archive, "large.bin", expected, length, 6));
    EXPECT_TRUE(mz_zip_writer_add_mem(&archive, "stored.bin", expected, length, 0));
    EXPECT_TRUE(mz_zip_writer_add_mem(&archive, "empty.bin", "", 0, 6));
    EXPECT_TRUE(mz_zip_writer_finalize_archive(&archive));
    EXPECT_TRUE(mz_zip_writer_end(&archive));
    EXPECT_EQ(fclose(file), 0);
    /* The temp locations deliberately name a file, so no extraction can work. */
    EXPECT_TRUE(SetEnvironmentVariableW(L"TEMP", path));
    EXPECT_TRUE(SetEnvironmentVariableW(L"TMP", path));
    vfs_init();
    EXPECT_TRUE(vfs_add_package(path));
    for (size_t variant = 0; variant < 2; variant++) {
        const wchar_t *name = variant == 0 ? L"large.bin" : L"stored.bin";
        vfs_stream_t *stream, *other;
        size_t read;
        uint64_t position;
        allocated_bytes = 0;
        track_allocations = true;
        stream = vfs_stream_open(name);
        track_allocations = false;
        EXPECT_NOT_NULL(stream);
        EXPECT_TRUE(allocated_bytes < (variant == 0 ? 256 * 1024 : 16 * 1024));
        EXPECT_EQ(vfs_stream_size(stream), length);
        EXPECT_TRUE(!vfs_stream_read(stream, NULL, 1, &read));
        EXPECT_EQ(GetLastError(), ERROR_INVALID_PARAMETER);
        EXPECT_EQ(read, 0);
        other = vfs_stream_open(name);
        EXPECT_NOT_NULL(other);
        for (size_t offset = 0; offset < length;) {
            size_t chunk = offset % 3 == 0 ? 1 : offset % 3 == 1 ? 70001 : 32767;
            if (chunk > length - offset) chunk = length - offset;
            EXPECT_TRUE(vfs_stream_read(stream, buffer, chunk, &read));
            EXPECT_EQ(read, chunk);
            EXPECT_TRUE(memcmp(buffer, expected + offset, read) == 0);
            offset += read;
        }
        EXPECT_TRUE(vfs_stream_read(stream, buffer, 1, &read));
        EXPECT_EQ(read, 0);
        EXPECT_TRUE(vfs_stream_read(other, buffer, 37, &read));
        EXPECT_TRUE(memcmp(buffer, expected, 37) == 0);
        vfs_stream_close(other);
        for (size_t i = 0; i < 25; i++) {
            random = random * 1664525 + 1013904223;
            size_t offset = random % (length - 70001);
            EXPECT_TRUE(vfs_stream_seek(stream, (int64_t)offset, FILE_BEGIN, &position));
            EXPECT_EQ(position, offset);
            EXPECT_TRUE(vfs_stream_read(stream, buffer, 70001, &read));
            EXPECT_EQ(read, 70001);
            EXPECT_TRUE(memcmp(buffer, expected + offset, read) == 0);
        }
        EXPECT_TRUE(vfs_stream_seek(stream, -73, FILE_END, &position));
        EXPECT_TRUE(vfs_stream_read_at(stream, 3, buffer, 41, &read));
        EXPECT_TRUE(memcmp(buffer, expected + 3, 41) == 0);
        EXPECT_TRUE(vfs_stream_read(stream, buffer, 70001, &read));
        EXPECT_EQ(read, 73);
        EXPECT_TRUE(memcmp(buffer, expected + length - 73, 73) == 0);
        EXPECT_TRUE(!vfs_stream_seek(stream, -1, FILE_BEGIN, NULL));
        EXPECT_EQ(GetLastError(), ERROR_NEGATIVE_SEEK);
        EXPECT_TRUE(!vfs_stream_seek(stream, INT64_MIN, FILE_CURRENT, NULL));
        EXPECT_TRUE(!vfs_stream_seek(stream, INT64_MAX, FILE_END, NULL));
        EXPECT_TRUE(vfs_stream_seek(stream, (int64_t)length + 5, FILE_BEGIN, NULL));
        EXPECT_TRUE(vfs_stream_read(stream, buffer, 10, &read));
        EXPECT_EQ(read, 0);
        vfs_stream_close(stream);
    }
    EXPECT_TRUE(read_matches(vfs_lookup(L"empty.bin"), ""));
    /* Open files remain readable after the index releases its package reference. */
    vfs_stream_t *retained = vfs_stream_open(L"large.bin");
    EXPECT_NOT_NULL(retained);
    vfs_uninit();
    size_t read;
    EXPECT_TRUE(vfs_stream_read(retained, buffer, 70001, &read));
    EXPECT_TRUE(memcmp(buffer, expected, read) == 0);
    vfs_stream_close(retained);
    EXPECT_TRUE(SetEnvironmentVariableW(L"TEMP", temp_length == 0 ? NULL : old_temp));
    EXPECT_TRUE(SetEnvironmentVariableW(L"TMP", tmp_length == 0 ? NULL : old_tmp));
    free(buffer);
    free(expected);
    return 0;
}

static int test_deflate_integrity(const wchar_t *path) {
    char contents[32768], buffer[32768];
    fixture_entry_t entry = { "integrity.bin", contents, 6 };
    memset(contents, 'z', sizeof(contents) - 1);
    contents[sizeof(contents) - 1] = '\0';
    for (size_t variant = 0; variant < 4; variant++) {
        FILE *file;
        mz_zip_archive archive = { 0 };
        mz_zip_archive_file_stat stat;
        mz_uint64 central;
        size_t read;
        uint32_t replacement;
        vfs_stream_t *stream;
        EXPECT_EQ(write_zip(path, &entry, 1, 0), 0);
        file = _wfopen(path, L"r+b");
        EXPECT_NOT_NULL(file);
        EXPECT_TRUE(mz_zip_reader_init_cfile(&archive, file, 0, 0));
        EXPECT_TRUE(mz_zip_reader_file_stat(&archive, 0, &stat));
        EXPECT_EQ(stat.m_method, MZ_DEFLATED);
        central = archive.m_central_directory_file_ofs + stat.m_central_dir_ofs;
        EXPECT_TRUE(mz_zip_reader_end(&archive));
        /* Corrupt CRC, shorten/extend the declared output, or truncate Deflate. */
        if (variant == 0) replacement = stat.m_crc32 ^ 1;
        else if (variant == 1) replacement = (uint32_t)stat.m_uncomp_size - 1;
        else if (variant == 2) replacement = (uint32_t)stat.m_uncomp_size + 1;
        else replacement = (uint32_t)stat.m_comp_size - 1;
        EXPECT_EQ(_fseeki64(file, (mz_int64)central + (variant == 0 ? 16 : variant == 3 ? 20 : 24), SEEK_SET), 0);
        EXPECT_EQ(fwrite(&replacement, sizeof(replacement), 1, file), 1);
        EXPECT_EQ(fclose(file), 0);
        vfs_init();
        EXPECT_TRUE(vfs_add_package(path));
        stream = vfs_stream_open(L"integrity.bin");
        EXPECT_NOT_NULL(stream);
        EXPECT_TRUE(!vfs_stream_read(stream, buffer, sizeof(buffer), &read));
        EXPECT_EQ(GetLastError(), variant == 0 ? ERROR_CRC : ERROR_INVALID_DATA);
        vfs_stream_close(stream);
        vfs_uninit();
    }
    return 0;
}

int main(void) {
    static const fixture_entry_t entries[] = {
        { "./", "", 0 },
        { "./parts/base.bin", "base stored", 0 },
        { "def/parts/base.bin", "subroot compressed", 6 },
        { "def/parts/zero.bin", "", 0 },
        { "def/parts/thread.bin", "shared compressed contents", 6 },
        { "def/sd/wem/00/001.wem", "sound", 6 },
        { "def/parts/\xE6\xB5\x8B\xE8\xAF\x95.bin", "unicode", 6 },
        { "def/parts/duplicate.bin", "first", 0 },
        { "def/PARTS/DUPLICATE.bin", "last", 6 },
        { "default/parts/leak.bin", "outside root", 0 },
        { "empty/", "", 0 },
    };
    static const fixture_entry_t unsafe[] = {
        { "parts/base.bin", "must not override", 0 },
        { "../escape.bin", "unsafe", 0 },
    };
    wchar_t temp[MAX_PATH];
    wchar_t root[MAX_PATH];
    wchar_t archive_path[MAX_PATH];
    wchar_t spec[MAX_PATH * 2];
    wchar_t loose[MAX_PATH];
    wchar_t loose_file[MAX_PATH];
    wchar_t invalid[MAX_PATH];
    const wchar_t *physical;
    const wchar_t *uid;
    HANDLE file;
    DWORD written;

    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp) != 0);
    EXPECT_TRUE(GetTempFileNameW(temp, L"zip", 0, root) != 0);
    EXPECT_TRUE(DeleteFileW(root));
    EXPECT_TRUE(CreateDirectoryW(root, NULL));
    _snwprintf(archive_path, MAX_PATH, L"%ls\\模组.ZIP", root);
    EXPECT_EQ(write_zip(archive_path, entries, sizeof(entries) / sizeof(entries[0]), MZ_ZIP_FLAG_WRITE_ZIP64), 0);
    {
        zip_package_t *package = zip_package_open(archive_path, L"def");
        EXPECT_NOT_NULL(package);
        EXPECT_TRUE(zip_package_count(package) != 0);
        zip_package_close(package);
    }

    vfs_init();
    EXPECT_TRUE(vfs_add_package(archive_path));
    EXPECT_EQ(vfs_entry_count(), 8);
    EXPECT_TRUE(read_matches(vfs_lookup(L"data0:/parts/base.bin"), "base stored"));
    EXPECT_TRUE(read_matches(vfs_lookup(L"def/parts/base.bin"), "subroot compressed"));
    vfs_uninit();

    for (size_t variant = 0; variant < 3; variant++) {
        const wchar_t *suffix = variant == 0 ? L":def" : variant == 1 ? L":/def" : L":\\DEF/./";
        _snwprintf(spec, MAX_PATH * 2, L"%ls%ls", archive_path, suffix);
        vfs_init();
        EXPECT_TRUE(vfs_add_package(spec));
        EXPECT_EQ(vfs_entry_count(), 6);
        EXPECT_NULL(vfs_lookup(L"def/parts/base.bin"));
        EXPECT_NULL(vfs_lookup(L"parts/leak.bin"));
        uid = vfs_virtual_to_uid(L"PARTS/base.bin");
        EXPECT_NOT_NULL(uid);
        physical = vfs_uid_to_path(uid);
        EXPECT_TRUE(read_matches(physical, "subroot compressed"));
        EXPECT_TRUE(physical == uid);
        EXPECT_TRUE(vfs_is_zip_path(physical));
        EXPECT_TRUE(read_matches(vfs_lookup(L"parts/zero.bin"), ""));
        EXPECT_TRUE(read_matches(vfs_lookup(L"parts/测试.bin"), "unicode"));
        EXPECT_TRUE(read_matches(vfs_lookup(L"parts/duplicate.bin"), "last"));
        EXPECT_TRUE(vfs_has_wwise_entries());
        EXPECT_TRUE(read_matches(vfs_lookup_domain(L"sd/wem/00/001.wem", VFS_LOOKUP_WWISE), "sound"));
        vfs_reset_lookup_caches();
        for (int domain = 0; domain < VFS_LOOKUP_DOMAIN_COUNT; domain++) {
            EXPECT_TRUE(vfs_lookup_domain(L"parts/base.bin", (vfs_lookup_domain_t)domain) == physical);
        }
        EXPECT_TRUE(vfs_lookup(uid) == physical);
        EXPECT_TRUE(read_matches(vfs_route_read_path_prefixed(L"C:\\game\\parts\\base.bin", L"C:\\game",
                                                             GENERIC_READ, OPEN_EXISTING), "subroot compressed"));
        EXPECT_NULL(vfs_route_read_path(L"parts/base.bin", GENERIC_WRITE, OPEN_EXISTING));
        EXPECT_NULL(vfs_route_read_path(L"parts/base.bin", GENERIC_READ, CREATE_ALWAYS));
        vfs_uninit();
    }

    /* Mixed directory/ZIP packages keep the existing last-declaration priority. */
    _snwprintf(loose, MAX_PATH, L"%ls\\directory.zip", root);
    EXPECT_TRUE(CreateDirectoryW(loose, NULL));
    _snwprintf(loose_file, MAX_PATH, L"%ls\\parts", loose);
    EXPECT_TRUE(CreateDirectoryW(loose_file, NULL));
    PathAppendW(loose_file, L"base.bin");
    file = CreateFileW(loose_file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    EXPECT_TRUE(WriteFile(file, "loose", 5, &written, NULL));
    CloseHandle(file);
    _snwprintf(spec, MAX_PATH * 2, L"%ls:def", archive_path);
    vfs_init();
    EXPECT_TRUE(vfs_add_package(loose));
    uid = vfs_virtual_to_uid(L"parts/base.bin");
    EXPECT_NOT_NULL(uid);
    EXPECT_TRUE(vfs_add_package(spec));
    EXPECT_TRUE(read_matches(vfs_uid_to_path(uid), "subroot compressed"));
    EXPECT_TRUE(vfs_add_package(loose));
    EXPECT_STREQ_W(vfs_uid_to_path(uid), loose_file);
    EXPECT_TRUE(read_matches(vfs_lookup(L"parts/base.bin"), "loose"));

    _snwprintf(invalid, MAX_PATH, L"%ls\\invalid.zip", root);
    EXPECT_EQ(write_zip(invalid, unsafe, 2, 0), 0);
    EXPECT_TRUE(!vfs_add_package(invalid));
    EXPECT_TRUE(read_matches(vfs_lookup(L"parts/base.bin"), "loose"));
    EXPECT_NULL(vfs_lookup(L"escape.bin"));
    EXPECT_TRUE(!vfs_add_package(L""));
    EXPECT_TRUE(!vfs_add_package(NULL));
    EXPECT_TRUE(!vfs_add_package(loose_file));
    {
        static const fixture_entry_t absolute_entry[] = { { "C:/escape.bin", "unsafe", 0 } };
        EXPECT_EQ(write_zip(invalid, absolute_entry, 1, 0), 0);
        EXPECT_TRUE(!vfs_add_package(invalid));
        EXPECT_NULL(vfs_lookup(L"escape.bin"));
    }
    {
        FILE *broken = _wfopen(invalid, L"wb");
        EXPECT_NOT_NULL(broken);
        EXPECT_TRUE(fputs("not a ZIP", broken) >= 0);
        EXPECT_EQ(fclose(broken), 0);
        EXPECT_TRUE(!vfs_add_package(invalid));
        EXPECT_TRUE(read_matches(vfs_lookup(L"parts/base.bin"), "loose"));
    }
    _snwprintf(spec, MAX_PATH * 2, L"%ls:/missing", archive_path);
    EXPECT_TRUE(!vfs_add_package(spec));
    _snwprintf(spec, MAX_PATH * 2, L"%ls:../def", archive_path);
    EXPECT_TRUE(!vfs_add_package(spec));
    _snwprintf(spec, MAX_PATH * 2, L"%ls:/empty", archive_path);
    EXPECT_TRUE(vfs_add_package(spec));
    vfs_uninit();

    /* Simultaneous UID/path reads use independent decompression states. */
    _snwprintf(spec, MAX_PATH * 2, L"%ls:def", archive_path);
    vfs_init();
    EXPECT_TRUE(vfs_add_package(spec));
    vfs_reset_lookup_caches();
    EXPECT_TRUE(!vfs_add_package(spec));
    thread_uid = vfs_virtual_to_uid(L"parts/thread.bin");
    EXPECT_NOT_NULL(thread_uid);
    {
        HANDLE threads[8];
        for (size_t i = 0; i < 8; i++) {
            threads[i] = CreateThread(NULL, 0, first_read_worker, (void *)i, 0, NULL);
            EXPECT_NOT_NULL(threads[i]);
        }
        EXPECT_EQ(WaitForMultipleObjects(8, threads, TRUE, 30000), WAIT_OBJECT_0);
        for (size_t i = 0; i < 8; i++) {
            CloseHandle(threads[i]);
            EXPECT_TRUE(thread_paths[i] == thread_paths[0]);
        }
        EXPECT_EQ(thread_failures, 0);
    }
    vfs_uninit();

    /* Corruption is reported on read and never becomes a cached lookup miss. */
    EXPECT_EQ(test_nested_roots(invalid), 0);
    EXPECT_EQ(test_failed_read_retry(invalid), 0);
    EXPECT_EQ(test_allocation_rollback(invalid, loose), 0);
    EXPECT_EQ(test_directory_rollback(root, loose), 0);
    EXPECT_EQ(test_large_streams(invalid), 0);
    EXPECT_EQ(test_deflate_integrity(invalid), 0);
    EXPECT_TRUE(DeleteFileW(invalid));
    EXPECT_TRUE(DeleteFileW(archive_path));
    EXPECT_TRUE(DeleteFileW(loose_file));
    PathRemoveFileSpecW(loose_file);
    EXPECT_TRUE(RemoveDirectoryW(loose_file));
    EXPECT_TRUE(RemoveDirectoryW(loose));
    EXPECT_TRUE(RemoveDirectoryW(root));
    printf("smoke_vfs_zip: all tests passed\n");
    return 0;
}
