#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef struct manager_release_s {
    wchar_t tag[64];
    wchar_t asset_url[1024];
    wchar_t asset_name[256];
    wchar_t checksum_url[1024];
    bool has_checksum;
} manager_release_t;

bool manager_latest_release(manager_release_t *release, wchar_t *error, size_t error_count);
bool manager_update_loader(const wchar_t *target_dir, const manager_release_t *release,
                           wchar_t *error, size_t error_count);
bool manager_latest_manager_release(manager_release_t *release, wchar_t *error, size_t error_count);
bool manager_update_manager(const wchar_t *manager_path, const manager_release_t *release,
                            wchar_t *error, size_t error_count);
