#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "config_model.h"
#include "locale.h"
#include "updater.h"

#ifndef YAFSML_MANAGER_VERSION_TEXT
#define YAFSML_MANAGER_VERSION_TEXT L"0.1.0"
#endif
#ifndef YAFSML_LOADER_VERSION_TEXT
#define YAFSML_LOADER_VERSION_TEXT L"0.0.0"
#endif

#pragma comment(lib, "comctl32.lib")

enum {
    IDC_PATH = 100,
    IDC_GAME_DIRECTORY,
    IDC_GAME_DIRECTORY_BROWSE,
    IDC_GAME_DIRECTORY_OVERRIDE,
    IDC_OPEN,
    IDC_SAVE,
    IDC_SAVE_AS,
    IDC_LAUNCH,
    IDC_SHORTCUT,
    IDC_UPDATE,
    IDC_MANAGER_UPDATE,
    IDC_DEFENDER,
    IDC_GAME,
    IDC_PATCH_SKIP_INTRO,
    IDC_PATCH_REG_SAVE,
    IDC_PATCH_MEM,
    IDC_PATCH_DEDICATED,
    IDC_PATCH_HEAP,
    IDC_PATCH_BOOT,
    IDC_PATCH_ARXAN,
    IDC_PATCH_IME,
    IDC_SAVE_NAME,
    IDC_SEAMLESS_NAME,
    IDC_CPU,
    IDC_CONSOLE,
    IDC_LOG_FILE,
    IDC_LOG_LEVEL,
    IDC_DLL_LIST,
    IDC_MOD_LIST,
    IDC_STATUS,
    IDC_DIRECTORY,
    IDC_ADD_DLL,
    IDC_REMOVE_DLL,
    IDC_MOVE_DLL_UP,
    IDC_MOVE_DLL_DOWN,
    IDC_ADD_MOD,
    IDC_REMOVE_MOD,
    IDC_MOVE_MOD_UP,
    IDC_MOVE_MOD_DOWN,
    IDC_DLL_NAME,
    IDC_DLL_PATH,
    IDC_DLL_BROWSE,
    IDC_DLL_STAGE,
    IDC_DLL_DELAY,
    IDC_DLL_DELAY_SPIN,
    IDC_DLL_AFTER,
    IDC_MOD_NAME,
    IDC_MOD_PATH,
    IDC_MOD_BROWSE,
    IDC_COMPAT_STATUS
};

#define WM_MANAGER_STARTUP_UPDATE (WM_APP + 1)
#define WM_MANAGER_DEFENDER_STATUS (WM_APP + 2)

static HINSTANCE app_instance;
static HWND main_window;
static manager_config_t app_config;
static wchar_t config_path[MAX_PATH];
static wchar_t target_dir[MAX_PATH];
static wchar_t game_dir[MAX_PATH];
static bool override_game_dir;
static bool dirty;
static wchar_t settings_path[MAX_PATH];
static bool loading_controls;
static manager_language_t current_language = MANAGER_LANGUAGE_ENGLISH;
static HFONT ui_font;
static bool own_ui_font;
static volatile LONG defender_query_in_progress;
static int defender_status_state = -2;

static void save_config(bool save_as);
static void start_defender_status_query(void);

static const wchar_t *text(manager_string_id_t id) { return manager_text(current_language, id); }

static BOOL CALLBACK apply_font_to_child(HWND child, LPARAM parameter) {
    SendMessageW(child, WM_SETFONT, parameter, TRUE);
    return TRUE;
}

static void initialize_ui_font(HWND hwnd) {
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        ui_font = CreateFontIndirectW(&metrics.lfMessageFont);
        own_ui_font = ui_font != NULL;
    }
    if (ui_font == NULL) ui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)ui_font, TRUE);
    EnumChildWindows(hwnd, apply_font_to_child, (LPARAM)ui_font);
}

static HWND control(int id) { return GetDlgItem(main_window, id); }

static void set_status(const wchar_t *text) {
    SetWindowTextW(control(IDC_STATUS), text != NULL ? text : L"");
}

static void settings_location(void) {
    wchar_t local_app_data[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local_app_data) == S_OK) {
        lstrcpynW(settings_path, local_app_data, MAX_PATH);
        PathAppendW(settings_path, L"YAFSML Manager");
        CreateDirectoryW(settings_path, NULL);
        PathAppendW(settings_path, L"settings.ini");
    }
}

static void load_settings(void) {
    wchar_t saved_dir[MAX_PATH] = L"";
    wchar_t saved_game_dir[MAX_PATH] = L"";
    wchar_t saved_config[MAX_PATH] = L"";
    if (settings_path[0] == L'\0') return;
    GetPrivateProfileStringW(L"manager", L"directory", L"", saved_dir, MAX_PATH, settings_path);
    GetPrivateProfileStringW(L"manager", L"game_directory", L"", saved_game_dir, MAX_PATH, settings_path);
    override_game_dir = GetPrivateProfileIntW(L"manager", L"override_game_directory", 0, settings_path) != 0;
    GetPrivateProfileStringW(L"manager", L"configuration", L"", saved_config, MAX_PATH, settings_path);
    if (saved_dir[0] != L'\0' && PathIsDirectoryW(saved_dir)) lstrcpynW(target_dir, saved_dir, MAX_PATH);
    if (saved_game_dir[0] != L'\0' && PathIsDirectoryW(saved_game_dir)) lstrcpynW(game_dir, saved_game_dir, MAX_PATH);
    if (saved_config[0] != L'\0' && PathFileExistsW(saved_config)) lstrcpynW(config_path, saved_config, MAX_PATH);
}

static void save_settings(void) {
    if (settings_path[0] == L'\0') return;
    WritePrivateProfileStringW(L"manager", L"directory", target_dir, settings_path);
    WritePrivateProfileStringW(L"manager", L"game_directory", game_dir, settings_path);
    WritePrivateProfileStringW(L"manager", L"override_game_directory", override_game_dir ? L"1" : L"0", settings_path);
    WritePrivateProfileStringW(L"manager", L"configuration", config_path, settings_path);
}

static void set_check(int id, bool value) {
    SendMessageW(control(id), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
}

static bool get_check(int id) {
    return SendMessageW(control(id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void set_text(int id, const wchar_t *value) {
    SetWindowTextW(control(id), value != NULL ? value : L"");
}

static void apply_game_directory_controls(void) {
    EnableWindow(control(IDC_GAME_DIRECTORY), override_game_dir);
    EnableWindow(control(IDC_GAME_DIRECTORY_BROWSE), override_game_dir);
}

static void get_text(int id, wchar_t *value, int count) {
    GetWindowTextW(control(id), value, count);
    value[count - 1] = L'\0';
}

static void get_combo_text(int id, wchar_t *value, int count) {
    HWND combo = control(id);
    int index = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index >= 0) SendMessageW(combo, CB_GETLBTEXT, (WPARAM)index, (LPARAM)value);
    else value[0] = L'\0';
    value[count - 1] = L'\0';
}

static void set_combo_text(int id, const wchar_t *value) {
    HWND combo = control(id);
    int index = (int)SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)value);
    if (index >= 0) SendMessageW(combo, CB_SETCURSEL, index, 0);
}

static void refresh_dynamic_lists(void) {
    HWND list = control(IDC_DLL_LIST);
    HWND mods = control(IDC_MOD_LIST);
    size_t i;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < app_config.dll_count; i++) {
        wchar_t line[1200];
        _snwprintf(line, sizeof(line) / sizeof(line[0]), L"%ls = %ls%ls%ls",
                   app_config.dlls[i].name, app_config.dlls[i].path,
                   app_config.dlls[i].condition[0] ? L" | " : L"",
                   app_config.dlls[i].condition);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    SendMessageW(mods, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < app_config.mod_count; i++) {
        wchar_t line[1200];
        _snwprintf(line, sizeof(line) / sizeof(line[0]), L"%ls = %ls", app_config.mods[i].name, app_config.mods[i].path);
        SendMessageW(mods, LB_ADDSTRING, 0, (LPARAM)line);
    }
}

static int dll_stage_index(const wchar_t *condition) {
    wchar_t copy[YAFSML_MANAGER_MAX_CONDITION];
    wchar_t *context = NULL;
    wchar_t *token;
    int stage = 0;
    bool has_after = false;
    if (condition == NULL) return 0;
    lstrcpynW(copy, condition, (int)(sizeof(copy) / sizeof(copy[0])));
    token = wcstok(copy, L"|", &context);
    while (token != NULL) {
        if (_wcsicmp(token, L"early") == 0) stage = 1;
        else if (_wcsicmp(token, L"data_ready") == 0) stage = 2;
        else if (_wcsnicmp(token, L"delay,", 6) == 0) stage = 3;
        else if (_wcsnicmp(token, L"after,", 6) == 0 && token[6] != L'\0') has_after = true;
        token = wcstok(NULL, L"|", &context);
    }
    return stage == 0 && has_after ? 4 : stage;
}

static bool dll_condition_has_after(const wchar_t *condition) {
    wchar_t copy[YAFSML_MANAGER_MAX_CONDITION];
    wchar_t *context = NULL;
    wchar_t *token;
    if (condition == NULL) return false;
    lstrcpynW(copy, condition, (int)(sizeof(copy) / sizeof(copy[0])));
    token = wcstok(copy, L"|", &context);
    while (token != NULL) {
        if (_wcsnicmp(token, L"after,", 6) == 0 && token[6] != L'\0') return true;
        token = wcstok(NULL, L"|", &context);
    }
    return false;
}

static void set_dll_stage_combo(const wchar_t *condition) {
    SendMessageW(control(IDC_DLL_STAGE), CB_SETCURSEL, dll_stage_index(condition), 0);
}

static void update_dll_stage_controls(int stage, bool has_after) {
    bool delay_visible = stage == 3;
    bool after_visible = stage == 4 || (stage == 3 && has_after);
    HWND delay = control(IDC_DLL_DELAY);
    HWND spin = control(IDC_DLL_DELAY_SPIN);
    HWND after = control(IDC_DLL_AFTER);
    ShowWindow(delay, delay_visible ? SW_SHOW : SW_HIDE);
    ShowWindow(spin, delay_visible ? SW_SHOW : SW_HIDE);
    ShowWindow(after, after_visible ? SW_SHOW : SW_HIDE);
    EnableWindow(after, after_visible);
    if (!after_visible) SendMessageW(after, CB_SETCURSEL, 0, 0);
}

static void populate_dll_after_combo(int selected, const wchar_t *condition) {
    HWND combo = control(IDC_DLL_AFTER);
    wchar_t after[64] = L"";
    size_t i;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"(none)");
    if (condition != NULL) {
        wchar_t copy[YAFSML_MANAGER_MAX_CONDITION];
        wchar_t *context = NULL;
        wchar_t *token;
        lstrcpynW(copy, condition, (int)(sizeof(copy) / sizeof(copy[0])));
        token = wcstok(copy, L"|", &context);
        while (token != NULL) {
            if (_wcsnicmp(token, L"after,", 6) == 0) lstrcpynW(after, token + 6, (int)(sizeof(after) / sizeof(after[0])));
            token = wcstok(NULL, L"|", &context);
        }
    }
    for (i = 0; i < app_config.dll_count; i++) {
        if ((int)i == selected) continue;
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)app_config.dlls[i].name);
    }
    if (after[0] == L'\0') SendMessageW(combo, CB_SETCURSEL, 0, 0);
    else {
        int match = (int)SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)after);
        SendMessageW(combo, CB_SETCURSEL, match >= 0 ? match : 0, 0);
    }
}

static void load_dll_detail(int selected) {
    wchar_t delay[32] = L"0";
    wchar_t copy[YAFSML_MANAGER_MAX_CONDITION];
    wchar_t *context = NULL;
    wchar_t *token;
    if (selected < 0 || (size_t)selected >= app_config.dll_count) {
        set_text(IDC_DLL_NAME, L""); set_text(IDC_DLL_PATH, L""); set_text(IDC_DLL_DELAY, L"0");
        SendMessageW(control(IDC_DLL_STAGE), CB_SETCURSEL, 0, 0);
        SendMessageW(control(IDC_DLL_AFTER), CB_SETCURSEL, 0, 0);
        ShowWindow(control(IDC_DLL_DELAY), SW_HIDE);
        ShowWindow(control(IDC_DLL_DELAY_SPIN), SW_HIDE);
        ShowWindow(control(IDC_DLL_AFTER), SW_HIDE);
        return;
    }
    set_text(IDC_DLL_NAME, app_config.dlls[selected].name);
    set_text(IDC_DLL_PATH, app_config.dlls[selected].path);
    lstrcpynW(copy, app_config.dlls[selected].condition, (int)(sizeof(copy) / sizeof(copy[0])));
    token = wcstok(copy, L"|", &context);
    while (token != NULL) {
        if (_wcsnicmp(token, L"delay,", 6) == 0) lstrcpynW(delay, token + 6, (int)(sizeof(delay) / sizeof(delay[0])));
        token = wcstok(NULL, L"|", &context);
    }
    set_dll_stage_combo(app_config.dlls[selected].condition);
    set_text(IDC_DLL_DELAY, delay);
    populate_dll_after_combo(selected, app_config.dlls[selected].condition);
    {
        int stage = (int)SendMessageW(control(IDC_DLL_STAGE), CB_GETCURSEL, 0, 0);
        update_dll_stage_controls(stage, dll_condition_has_after(app_config.dlls[selected].condition));
    }
}

static void sync_dll_condition_from_controls(void) {
    HWND list = control(IDC_DLL_LIST);
    int selected = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    int stage = (int)SendMessageW(control(IDC_DLL_STAGE), CB_GETCURSEL, 0, 0);
    int after = (int)SendMessageW(control(IDC_DLL_AFTER), CB_GETCURSEL, 0, 0);
    wchar_t delay[32], condition[YAFSML_MANAGER_MAX_CONDITION] = L"";
    wchar_t name[64] = L"";
    if (loading_controls || selected < 0 || (size_t)selected >= app_config.dll_count) return;
    if (stage == 1) lstrcpyW(condition, L"early");
    else if (stage == 2) lstrcpyW(condition, L"data_ready");
    else if (stage == 3) {
        get_text(IDC_DLL_DELAY, delay, (int)(sizeof(delay) / sizeof(delay[0])));
        if (delay[0] == L'\0') lstrcpyW(delay, L"0");
        _snwprintf(condition, sizeof(condition) / sizeof(condition[0]), L"delay,%ls", delay);
    }
    if (stage == 4 && after > 0) {
        SendMessageW(control(IDC_DLL_AFTER), CB_GETLBTEXT, (WPARAM)after, (LPARAM)name);
        if (condition[0] != L'\0') lstrcatW(condition, L"|");
        lstrcatW(condition, L"after,"); lstrcatW(condition, name);
    } else if (stage == 3 && after > 0) {
        SendMessageW(control(IDC_DLL_AFTER), CB_GETLBTEXT, (WPARAM)after, (LPARAM)name);
        if (condition[0] != L'\0') lstrcatW(condition, L"|after,");
        lstrcatW(condition, name);
    }
    lstrcpynW(app_config.dlls[selected].condition, condition, (int)(sizeof(app_config.dlls[selected].condition) / sizeof(app_config.dlls[selected].condition[0])));
    refresh_dynamic_lists();
    SendMessageW(list, LB_SETCURSEL, selected, 0);
    update_dll_stage_controls(stage, stage == 4 || (stage == 3 && after > 0));
    dirty = true;
}

static void apply_game_compatibility(void) {
    bool allocator_supported = _wcsicmp(app_config.game, L"armoredcore6") != 0 &&
                               _wcsicmp(app_config.game, L"nightreign") != 0;
    bool arxan_configurable = _wcsicmp(app_config.game, L"darksouls3") != 0;
    EnableWindow(control(IDC_PATCH_MEM), allocator_supported);
    EnableWindow(control(IDC_PATCH_DEDICATED), allocator_supported && app_config.patch_mem);
    EnableWindow(control(IDC_PATCH_HEAP), allocator_supported && app_config.patch_mem && app_config.patch_mem_dedicated_heap);
    EnableWindow(control(IDC_PATCH_ARXAN), arxan_configurable);
    if (!allocator_supported) {
        app_config.patch_mem = false;
        app_config.patch_mem_dedicated_heap = false;
        app_config.patch_mem_heap_size = 0;
        set_check(IDC_PATCH_MEM, false);
        set_check(IDC_PATCH_DEDICATED, false);
        set_text(IDC_PATCH_HEAP, L"0");
    }
    if (!arxan_configurable) {
        app_config.disable_arxan = true;
        set_check(IDC_PATCH_ARXAN, true);
    }
    if (!allocator_supported) set_text(IDC_COMPAT_STATUS, L"Allocator patch is unsupported for this game and will be saved disabled.");
    else if (!arxan_configurable) set_text(IDC_COMPAT_STATUS, L"Dark Souls III always enables Arxan neutralization.");
    else set_text(IDC_COMPAT_STATUS, L"");
}

static void sync_dynamic_detail(bool dll) {
    HWND list = control(dll ? IDC_DLL_LIST : IDC_MOD_LIST);
    int selected = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    loading_controls = true;
    if (dll && selected >= 0 && (size_t)selected < app_config.dll_count) {
        load_dll_detail(selected);
    } else if (!dll && selected >= 0 && (size_t)selected < app_config.mod_count) {
        set_text(IDC_MOD_NAME, app_config.mods[selected].name);
        set_text(IDC_MOD_PATH, app_config.mods[selected].path);
    } else if (dll) {
        set_text(IDC_DLL_NAME, L""); set_text(IDC_DLL_PATH, L""); set_text(IDC_DLL_DELAY, L"0");
        SendMessageW(control(IDC_DLL_STAGE), CB_SETCURSEL, 0, 0);
        SendMessageW(control(IDC_DLL_AFTER), CB_RESETCONTENT, 0, 0);
        SendMessageW(control(IDC_DLL_AFTER), CB_ADDSTRING, 0, (LPARAM)L"(none)");
        SendMessageW(control(IDC_DLL_AFTER), CB_SETCURSEL, 0, 0);
        ShowWindow(control(IDC_DLL_DELAY), SW_HIDE);
        ShowWindow(control(IDC_DLL_DELAY_SPIN), SW_HIDE);
        ShowWindow(control(IDC_DLL_AFTER), SW_HIDE);
    } else {
        set_text(IDC_MOD_NAME, L""); set_text(IDC_MOD_PATH, L"");
    }
    loading_controls = false;
}

static void sync_dynamic_from_detail(bool dll, int control_id) {
    HWND list = control(dll ? IDC_DLL_LIST : IDC_MOD_LIST);
    int selected = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (loading_controls || selected < 0) return;
    if (dll && (size_t)selected < app_config.dll_count) {
        manager_dll_item_t *item = &app_config.dlls[selected];
        if (control_id == IDC_DLL_NAME) get_text(IDC_DLL_NAME, item->name, (int)(sizeof(item->name) / sizeof(item->name[0])));
        else if (control_id == IDC_DLL_PATH) get_text(IDC_DLL_PATH, item->path, (int)(sizeof(item->path) / sizeof(item->path[0])));
    } else if (!dll && (size_t)selected < app_config.mod_count) {
        manager_mod_item_t *item = &app_config.mods[selected];
        if (control_id == IDC_MOD_NAME) get_text(IDC_MOD_NAME, item->name, (int)(sizeof(item->name) / sizeof(item->name[0])));
        else if (control_id == IDC_MOD_PATH) get_text(IDC_MOD_PATH, item->path, (int)(sizeof(item->path) / sizeof(item->path[0])));
    }
    refresh_dynamic_lists();
    SendMessageW(list, LB_SETCURSEL, selected, 0);
    dirty = true;
}

static void load_controls(void) {
    loading_controls = true;
    set_combo_text(IDC_GAME, app_config.game);
    set_check(IDC_PATCH_SKIP_INTRO, app_config.skip_intro);
    set_check(IDC_PATCH_REG_SAVE, app_config.prevent_regulation_save_write);
    set_check(IDC_PATCH_MEM, app_config.patch_mem);
    set_check(IDC_PATCH_DEDICATED, app_config.patch_mem_dedicated_heap);
    set_check(IDC_PATCH_BOOT, app_config.boot_boost);
    set_check(IDC_PATCH_ARXAN, app_config.disable_arxan);
    set_check(IDC_PATCH_IME, app_config.enable_ime);
    set_text(IDC_PATCH_HEAP, L"");
    { wchar_t number[32]; _snwprintf(number, 32, L"%u", app_config.patch_mem_heap_size); set_text(IDC_PATCH_HEAP, number); }
    set_text(IDC_SAVE_NAME, app_config.replace_save_filename);
    set_text(IDC_SEAMLESS_NAME, app_config.replace_seamless_coop_save_filename);
    SendMessageW(control(IDC_CPU), CB_SETCURSEL,
                 (WPARAM)(app_config.cpu_affinity >= 0 && app_config.cpu_affinity <= 4 ? app_config.cpu_affinity : 0), 0);
    set_check(IDC_CONSOLE, app_config.console);
    set_check(IDC_LOG_FILE, app_config.log_file);
    set_combo_text(IDC_LOG_LEVEL, app_config.log_level);
    apply_game_compatibility();
    refresh_dynamic_lists();
    SendMessageW(control(IDC_DLL_LIST), LB_SETCURSEL, app_config.dll_count ? 0 : (WPARAM)-1, 0);
    SendMessageW(control(IDC_MOD_LIST), LB_SETCURSEL, app_config.mod_count ? 0 : (WPARAM)-1, 0);
    set_text(IDC_PATH, target_dir);
    set_check(IDC_GAME_DIRECTORY_OVERRIDE, override_game_dir);
    set_text(IDC_GAME_DIRECTORY, game_dir);
    apply_game_directory_controls();
    SendMessageW(control(IDC_DLL_LIST), LB_SETCURSEL, app_config.dll_count ? 0 : (WPARAM)-1, 0);
    SendMessageW(control(IDC_MOD_LIST), LB_SETCURSEL, app_config.mod_count ? 0 : (WPARAM)-1, 0);
    /* Populate the detail panes after the list boxes have been filled. */
    {
        int selected = (int)SendMessageW(control(IDC_DLL_LIST), LB_GETCURSEL, 0, 0);
        if (selected >= 0) {
            load_dll_detail(selected);
        } else {
            set_text(IDC_DLL_NAME, L""); set_text(IDC_DLL_PATH, L""); set_text(IDC_DLL_DELAY, L"0");
            ShowWindow(control(IDC_DLL_DELAY), SW_HIDE);
            ShowWindow(control(IDC_DLL_DELAY_SPIN), SW_HIDE);
            ShowWindow(control(IDC_DLL_AFTER), SW_HIDE);
        }
        selected = (int)SendMessageW(control(IDC_MOD_LIST), LB_GETCURSEL, 0, 0);
        if (selected >= 0) {
            set_text(IDC_MOD_NAME, app_config.mods[selected].name);
            set_text(IDC_MOD_PATH, app_config.mods[selected].path);
        } else {
            set_text(IDC_MOD_NAME, L""); set_text(IDC_MOD_PATH, L"");
        }
    }
    /* Selection/detail text changes above must not make a freshly loaded file dirty. */
    dirty = false;
    loading_controls = false;
}

static void read_controls(void) {
    wchar_t number[64];
    get_combo_text(IDC_GAME, app_config.game, (int)(sizeof(app_config.game) / sizeof(app_config.game[0])));
    app_config.skip_intro = get_check(IDC_PATCH_SKIP_INTRO);
    app_config.prevent_regulation_save_write = get_check(IDC_PATCH_REG_SAVE);
    app_config.patch_mem = get_check(IDC_PATCH_MEM);
    app_config.patch_mem_dedicated_heap = get_check(IDC_PATCH_DEDICATED);
    app_config.boot_boost = get_check(IDC_PATCH_BOOT);
    app_config.disable_arxan = get_check(IDC_PATCH_ARXAN);
    app_config.enable_ime = get_check(IDC_PATCH_IME);
    get_text(IDC_PATCH_HEAP, number, 64); app_config.patch_mem_heap_size = (uint32_t)wcstoul(number, NULL, 10);
    get_text(IDC_SAVE_NAME, app_config.replace_save_filename, (int)(sizeof(app_config.replace_save_filename) / sizeof(app_config.replace_save_filename[0])));
    get_text(IDC_SEAMLESS_NAME, app_config.replace_seamless_coop_save_filename, (int)(sizeof(app_config.replace_seamless_coop_save_filename) / sizeof(app_config.replace_seamless_coop_save_filename[0])));
    {
        int cpu = (int)SendMessageW(control(IDC_CPU), CB_GETCURSEL, 0, 0);
        app_config.cpu_affinity = cpu >= 0 && cpu <= 4 ? cpu : 0;
    }
    app_config.console = get_check(IDC_CONSOLE);
    app_config.log_file = get_check(IDC_LOG_FILE);
    get_combo_text(IDC_LOG_LEVEL, app_config.log_level, (int)(sizeof(app_config.log_level) / sizeof(app_config.log_level[0])));
}

static bool choose_file(wchar_t *path, bool save) {
    OPENFILENAMEW dialog = { sizeof(dialog) };
    wchar_t title[MAX_PATH] = L"";
    dialog.hwndOwner = main_window;
    dialog.lpstrFilter = L"YAFSML INI (*.ini)\0*.ini\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = save ? L"Save configuration" : L"Open configuration";
    dialog.Flags = save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (save) return GetSaveFileNameW(&dialog) != FALSE;
    (void)title;
    return GetOpenFileNameW(&dialog) != FALSE;
}

static bool choose_asset_file(wchar_t *path, int count, bool dll) {
    OPENFILENAMEW dialog = { sizeof(dialog) };
    dialog.hwndOwner = main_window;
    dialog.lpstrFilter = dll
        ? L"DLL files (*.dll)\0*.dll\0All files (*.*)\0*.*\0\0"
        : L"Mod files (*.*)\0*.*\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = count;
    dialog.lpstrTitle = dll ? L"Select DLL" : L"Select mod file";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) != FALSE;
}

static bool make_asset_config_path(const wchar_t *selected, wchar_t *output, int count) {
    wchar_t full_path[1024];
    wchar_t base_path[MAX_PATH];
    wchar_t relative_path[1024];
    DWORD length;
    if (selected == NULL || output == NULL || count <= 0) return false;
    length = GetFullPathNameW(selected, (DWORD)(sizeof(full_path) / sizeof(full_path[0])), full_path, NULL);
    if (length == 0 || length >= sizeof(full_path) / sizeof(full_path[0])) return false;
    if (target_dir[0] != L'\0') {
        length = GetFullPathNameW(target_dir, (DWORD)(sizeof(base_path) / sizeof(base_path[0])), base_path, NULL);
        if (length > 0 && length < sizeof(base_path) / sizeof(base_path[0]) &&
            PathRelativePathToW(relative_path, base_path, FILE_ATTRIBUTE_DIRECTORY,
                                full_path, FILE_ATTRIBUTE_NORMAL)) {
            lstrcpynW(output, relative_path, count);
            return true;
        }
    }
    lstrcpynW(output, full_path, count);
    return true;
}

static void browse_dynamic_file(bool dll) {
    HWND list = control(dll ? IDC_DLL_LIST : IDC_MOD_LIST);
    int selected = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    wchar_t selected_path[1024] = L"";
    wchar_t absolute_path[1024] = L"";
    if (selected < 0) {
        MessageBoxW(main_window, dll ? L"Select a DLL entry first." : L"Select a mod entry first.",
                    L"YAFSML Manager", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (dll) get_text(IDC_DLL_PATH, selected_path, (int)(sizeof(selected_path) / sizeof(selected_path[0])));
    else get_text(IDC_MOD_PATH, selected_path, (int)(sizeof(selected_path) / sizeof(selected_path[0])));
    if (selected_path[0] != L'\0' && PathIsRelativeW(selected_path)) {
        lstrcpynW(absolute_path, target_dir, (int)(sizeof(absolute_path) / sizeof(absolute_path[0])));
        PathAppendW(absolute_path, selected_path);
        lstrcpynW(selected_path, absolute_path, (int)(sizeof(selected_path) / sizeof(selected_path[0])));
    }
    if (!choose_asset_file(selected_path, (int)(sizeof(selected_path) / sizeof(selected_path[0])), dll)) return;
    if (!make_asset_config_path(selected_path, absolute_path, (int)(sizeof(absolute_path) / sizeof(absolute_path[0])))) return;
    if (dll) set_text(IDC_DLL_PATH, absolute_path);
    else set_text(IDC_MOD_PATH, absolute_path);
    sync_dynamic_from_detail(dll, dll ? IDC_DLL_PATH : IDC_MOD_PATH);
}

static bool ensure_saved(void) {
    if (!dirty) return true;
    {
        int result = MessageBoxW(main_window, L"The configuration has unsaved changes. Save now?", L"YAFSML Manager", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (result == IDCANCEL) return false;
        if (result == IDYES) {
            save_config(config_path[0] == L'\0');
            if (dirty) return false;
        }
    }
    return true;
}

static void open_config(void) {
    wchar_t path[MAX_PATH] = L"";
    if (!ensure_saved() || !choose_file(path, false)) return;
    if (!manager_config_load(&app_config, path)) {
        MessageBoxW(main_window, L"Could not parse the configuration.", L"YAFSML Manager", MB_OK | MB_ICONERROR);
        return;
    }
    lstrcpynW(config_path, path, MAX_PATH);
    load_controls();
    set_status(app_config.unknown_count ? L"Loaded; unsupported options will be removed on save." : L"Loaded");
}

static void save_config(bool save_as) {
    wchar_t path[MAX_PATH];
    if (save_as || config_path[0] == L'\0') {
        if (config_path[0] != L'\0') lstrcpynW(path, config_path, MAX_PATH);
        else {
            lstrcpynW(path, target_dir, MAX_PATH);
            PathAppendW(path, L"YAFSML.ini");
        }
        if (!choose_file(path, true)) return;
        lstrcpynW(config_path, path, MAX_PATH);
    }
    read_controls();
    if (!manager_config_save(&app_config, config_path)) {
        MessageBoxW(main_window, L"Could not save the configuration.", L"YAFSML Manager", MB_OK | MB_ICONERROR);
        return;
    }
    dirty = false;
    set_status(L"Saved");
}

static void build_launcher_arguments(wchar_t *arguments, int count) {
    get_text(IDC_GAME_DIRECTORY, game_dir, MAX_PATH);
    if (override_game_dir && game_dir[0] != L'\0') {
        _snwprintf(arguments, count, L"--config \"%ls\" --game-path \"%ls\"", config_path, game_dir);
    } else {
        _snwprintf(arguments, count, L"--config \"%ls\"", config_path);
    }
    arguments[count - 1] = L'\0';
}

static void launch_config(void) {
    wchar_t exe[MAX_PATH];
    wchar_t arguments[MAX_PATH * 3];
    if (!ensure_saved()) return;
    if (config_path[0] == L'\0') { MessageBoxW(main_window, L"Open or save a configuration first.", L"YAFSML Manager", MB_OK | MB_ICONWARNING); return; }
    lstrcpynW(exe, target_dir, MAX_PATH); PathAppendW(exe, L"YAFSML.exe");
    if (!PathFileExistsW(exe)) { MessageBoxW(main_window, L"YAFSML.exe was not found in the selected directory.", L"YAFSML Manager", MB_OK | MB_ICONERROR); return; }
    build_launcher_arguments(arguments, (int)(sizeof(arguments) / sizeof(arguments[0])));
    if ((INT_PTR)ShellExecuteW(main_window, L"open", exe, arguments, target_dir, SW_SHOWNORMAL) <= 32)
        MessageBoxW(main_window, L"Could not launch YAFSML.", L"YAFSML Manager", MB_OK | MB_ICONERROR);
}

static void create_shortcut(void) {
    IUnknown *unknown = NULL;
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    wchar_t shortcut[MAX_PATH];
    wchar_t exe[MAX_PATH];
    if (!ensure_saved() || config_path[0] == L'\0') return;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return;
    lstrcpynW(exe, target_dir, MAX_PATH); PathAppendW(exe, L"YAFSML.exe");
    link->lpVtbl->SetPath(link, exe);
    { wchar_t args[MAX_PATH * 3]; build_launcher_arguments(args, (int)(sizeof(args) / sizeof(args[0]))); link->lpVtbl->SetArguments(link, args); }
    link->lpVtbl->SetWorkingDirectory(link, target_dir);
    link->lpVtbl->SetIconLocation(link, exe, 0);
    if (SUCCEEDED(link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
        lstrcpynW(shortcut, config_path, MAX_PATH); PathRemoveExtensionW(shortcut); lstrcatW(shortcut, L".lnk");
        file->lpVtbl->Save(file, shortcut, TRUE);
        file->lpVtbl->Release(file);
    }
    link->lpVtbl->Release(link);
    (void)unknown;
    set_status(L"Shortcut created beside the configuration file");
}

static void browse_directory_for(bool game) {
    BROWSEINFOW browse = { 0 };
    PIDLIST_ABSOLUTE id;
    browse.hwndOwner = main_window;
    browse.lpszTitle = game ? text(MGR_STR_GAME_DIRECTORY) : text(MGR_STR_YAFSML_DIRECTORY);
    id = SHBrowseForFolderW(&browse);
    if (id != NULL) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(id, path)) {
            if (game) {
                lstrcpynW(game_dir, path, MAX_PATH);
                set_text(IDC_GAME_DIRECTORY, game_dir);
            } else {
                lstrcpynW(target_dir, path, MAX_PATH);
                set_text(IDC_PATH, target_dir);
                start_defender_status_query();
            }
        }
        CoTaskMemFree(id);
    }
}

static void browse_directory(void) { browse_directory_for(false); }
static void browse_game_directory(void) { browse_directory_for(true); }

static void add_dynamic(bool dll) {
    wchar_t name[64] = L"";
    wchar_t path[1024] = L"";
    if (dll && app_config.dll_count < YAFSML_MANAGER_MAX_ITEMS) {
        _snwprintf(name, 64, L"dll%u", (unsigned)(app_config.dll_count + 1));
        lstrcpyW(path, L"example.dll");
        lstrcpynW(app_config.dlls[app_config.dll_count].name, name, 64);
        lstrcpynW(app_config.dlls[app_config.dll_count].path, path, 1024);
        app_config.dll_count++;
    } else if (!dll && app_config.mod_count < YAFSML_MANAGER_MAX_ITEMS) {
        _snwprintf(name, 64, L"mod%u", (unsigned)(app_config.mod_count + 1));
        lstrcpyW(path, L"mod");
        lstrcpynW(app_config.mods[app_config.mod_count].name, name, 64);
        lstrcpynW(app_config.mods[app_config.mod_count].path, path, 1024);
        app_config.mod_count++;
    }
    refresh_dynamic_lists();
    if (dll) {
        SendMessageW(control(IDC_DLL_LIST), LB_SETCURSEL, app_config.dll_count ? (WPARAM)(app_config.dll_count - 1) : (WPARAM)-1, 0);
        sync_dynamic_detail(true);
    } else {
        SendMessageW(control(IDC_MOD_LIST), LB_SETCURSEL, app_config.mod_count ? (WPARAM)(app_config.mod_count - 1) : (WPARAM)-1, 0);
        sync_dynamic_detail(false);
    }
    dirty = true;
}

static void move_dynamic(bool dll, int direction) {
    HWND list = control(dll ? IDC_DLL_LIST : IDC_MOD_LIST);
    int selected = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    int target;
    if (selected < 0) return;
    target = selected + direction;
    if (dll) {
        manager_dll_item_t item;
        if (target < 0 || (size_t)target >= app_config.dll_count) return;
        item = app_config.dlls[selected]; app_config.dlls[selected] = app_config.dlls[target]; app_config.dlls[target] = item;
        SendMessageW(list, LB_SETCURSEL, target, 0);
    } else {
        manager_mod_item_t item;
        if (target < 0 || (size_t)target >= app_config.mod_count) return;
        item = app_config.mods[selected]; app_config.mods[selected] = app_config.mods[target]; app_config.mods[target] = item;
        SendMessageW(list, LB_SETCURSEL, target, 0);
    }
    refresh_dynamic_lists();
    SendMessageW(list, LB_SETCURSEL, target, 0);
    sync_dynamic_detail(dll);
    dirty = true;
}

static void remove_dynamic(bool dll) {
    HWND list = control(dll ? IDC_DLL_LIST : IDC_MOD_LIST);
    int selected = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    size_t i;
    if (selected < 0) return;
    if (dll && (size_t)selected < app_config.dll_count) {
        for (i = (size_t)selected + 1; i < app_config.dll_count; i++) app_config.dlls[i - 1] = app_config.dlls[i];
        app_config.dll_count--;
    } else if (!dll && (size_t)selected < app_config.mod_count) {
        for (i = (size_t)selected + 1; i < app_config.mod_count; i++) app_config.mods[i - 1] = app_config.mods[i];
        app_config.mod_count--;
    }
    refresh_dynamic_lists();
    if (dll) {
        selected = selected < (int)app_config.dll_count ? selected : (int)app_config.dll_count - 1;
        SendMessageW(control(IDC_DLL_LIST), LB_SETCURSEL, selected >= 0 ? (WPARAM)selected : (WPARAM)-1, 0);
        sync_dynamic_detail(true);
    } else {
        selected = selected < (int)app_config.mod_count ? selected : (int)app_config.mod_count - 1;
        SendMessageW(control(IDC_MOD_LIST), LB_SETCURSEL, selected >= 0 ? (WPARAM)selected : (WPARAM)-1, 0);
        sync_dynamic_detail(false);
    }
    dirty = true;
}

static void check_updates(void) {
    manager_release_t release;
    wchar_t error[256];
    if (!manager_latest_release(&release, error, sizeof(error) / sizeof(error[0]))) {
        MessageBoxW(main_window, error, L"YAFSML Manager", MB_OK | MB_ICONERROR);
        return;
    }
    {
        wchar_t current_tag[64];
        _snwprintf(current_tag, sizeof(current_tag) / sizeof(current_tag[0]), L"v%ls", YAFSML_LOADER_VERSION_TEXT);
        if (_wcsicmp(current_tag, release.tag) == 0) {
            set_status(L"YAFSML loader is up to date");
            return;
        }
    }
    {
        wchar_t message[512];
        _snwprintf(message, sizeof(message) / sizeof(message[0]), L"Latest loader release: %ls\n\nDownload and install it now?", release.tag);
        if (MessageBoxW(main_window, message, L"YAFSML Manager", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    if (!manager_update_loader(target_dir, &release, error, sizeof(error) / sizeof(error[0]))) {
        MessageBoxW(main_window, error, L"YAFSML Manager", MB_OK | MB_ICONERROR);
        return;
    }
    set_status(L"YAFSML loader updated");
    MessageBoxW(main_window, L"YAFSML has been updated. User configuration and mod files were preserved.", L"YAFSML Manager", MB_OK | MB_ICONINFORMATION);
}

static void check_manager_updates(void) {
    manager_release_t release;
    wchar_t error[256];
    wchar_t manager_path[MAX_PATH];
    if (!manager_latest_manager_release(&release, error, sizeof(error) / sizeof(error[0]))) {
        MessageBoxW(main_window, error, L"YAFSML Manager", MB_OK | MB_ICONERROR);
        return;
    }
    {
        wchar_t current_tag[64];
        _snwprintf(current_tag, sizeof(current_tag) / sizeof(current_tag[0]), L"manager-v%ls", YAFSML_MANAGER_VERSION_TEXT);
        if (_wcsicmp(current_tag, release.tag) == 0) {
            set_status(L"YAFSML Manager is up to date");
            return;
        }
    }
    {
        wchar_t message[512];
        _snwprintf(message, sizeof(message) / sizeof(message[0]), L"Latest manager release: %ls\n\nDownload and install it after this window closes?", release.tag);
        if (MessageBoxW(main_window, message, L"YAFSML Manager", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    GetModuleFileNameW(NULL, manager_path, MAX_PATH);
    if (!manager_update_manager(manager_path, &release, error, sizeof(error) / sizeof(error[0]))) {
        MessageBoxW(main_window, error, L"YAFSML Manager", MB_OK | MB_ICONERROR);
        return;
    }
    MessageBoxW(main_window, L"The manager will close, replace itself, and restart.", L"YAFSML Manager", MB_OK | MB_ICONINFORMATION);
    DestroyWindow(main_window);
}

static void check_startup_updates(void) {
    manager_release_t release;
    manager_release_t manager_release;
    wchar_t error[256];
    wchar_t current_tag[64];
    if (manager_latest_release(&release, error, sizeof(error) / sizeof(error[0])) && release.has_checksum) {
        _snwprintf(current_tag, sizeof(current_tag) / sizeof(current_tag[0]), L"v%ls", YAFSML_LOADER_VERSION_TEXT);
        if (_wcsicmp(current_tag, release.tag) != 0) {
            wchar_t message[512];
            _snwprintf(message, sizeof(message) / sizeof(message[0]), L"A newer YAFSML loader release (%ls) is available. Download it now?", release.tag);
            if (MessageBoxW(main_window, message, L"YAFSML Manager", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                if (manager_update_loader(target_dir, &release, error, sizeof(error) / sizeof(error[0]))) set_status(L"YAFSML loader updated");
                else MessageBoxW(main_window, error, L"YAFSML Manager", MB_OK | MB_ICONERROR);
            }
        }
    }
    if (manager_latest_manager_release(&manager_release, error, sizeof(error) / sizeof(error[0])) && manager_release.has_checksum) {
        _snwprintf(current_tag, sizeof(current_tag) / sizeof(current_tag[0]), L"manager-v%ls", YAFSML_MANAGER_VERSION_TEXT);
        if (_wcsicmp(current_tag, manager_release.tag) != 0) {
            wchar_t message[512], manager_path[MAX_PATH];
            _snwprintf(message, sizeof(message) / sizeof(message[0]), L"A newer YAFSML Manager release (%ls) is available. Download it now?", manager_release.tag);
            if (MessageBoxW(main_window, message, L"YAFSML Manager", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                GetModuleFileNameW(NULL, manager_path, MAX_PATH);
                if (manager_update_manager(manager_path, &manager_release, error, sizeof(error) / sizeof(error[0]))) {
                    MessageBoxW(main_window, L"The manager will close, replace itself, and restart.", L"YAFSML Manager", MB_OK | MB_ICONINFORMATION);
                    DestroyWindow(main_window);
                } else {
                    MessageBoxW(main_window, error, L"YAFSML Manager", MB_OK | MB_ICONERROR);
                }
            }
        }
    }
}

static void powershell_literal(const wchar_t *value, wchar_t *output, size_t count) {
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

static bool run_powershell(const wchar_t *command, bool elevated, DWORD *exit_code) {
    /*
     * Passing a script through -Command relies on a second command-line
     * parser.  Paths containing quotes, ampersands, or other shell syntax
     * can therefore be mangled before PowerShell sees them.  Encode the
     * script as UTF-16LE and use -EncodedCommand instead; this is also the
     * form that survives ShellExecuteEx's runas elevation reliably.
     */
    static const wchar_t base64[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    wchar_t encoded[16384];
    wchar_t parameters[17000];
    size_t byte_count = command == NULL ? 0 : wcslen(command) * sizeof(wchar_t);
    size_t encoded_count = ((byte_count + 2) / 3) * 4;
    size_t i, at = 0;
    SHELLEXECUTEINFOW execute = { sizeof(execute) };
    if (exit_code != NULL) *exit_code = (DWORD)-1;
    if (encoded_count + 1 > sizeof(encoded) / sizeof(encoded[0])) return false;
    for (i = 0; i < byte_count; i += 3) {
        const unsigned char *bytes = (const unsigned char *)command;
        unsigned int b0 = bytes[i];
        unsigned int b1 = (i + 1 < byte_count) ? bytes[i + 1] : 0;
        unsigned int b2 = (i + 2 < byte_count) ? bytes[i + 2] : 0;
        encoded[at++] = base64[(b0 >> 2) & 0x3f];
        encoded[at++] = base64[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0f)];
        encoded[at++] = (i + 1 < byte_count) ? base64[((b1 & 0x0f) << 2) | ((b2 >> 6) & 0x03)] : L'=';
        encoded[at++] = (i + 2 < byte_count) ? base64[b2 & 0x3f] : L'=';
    }
    encoded[at] = L'\0';
    _snwprintf(parameters, sizeof(parameters) / sizeof(parameters[0]), L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand %ls", encoded);
    execute.hwnd = main_window;
    execute.lpVerb = elevated ? L"runas" : NULL;
    execute.lpFile = L"powershell.exe";
    execute.lpParameters = parameters;
    execute.lpDirectory = target_dir;
    execute.nShow = SW_HIDE;
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    if (!ShellExecuteExW(&execute)) return false;
    if (execute.hProcess != NULL) {
        WaitForSingleObject(execute.hProcess, INFINITE);
        if (exit_code != NULL) GetExitCodeProcess(execute.hProcess, exit_code);
        CloseHandle(execute.hProcess);
    }
    /*
     * ShellExecuteEx can legitimately return without a process handle when
     * the runas verb hands the operation to the elevation broker.  Treat the
     * launch itself as successful and let the caller verify Defender's state
     * instead of reporting a false failure immediately after UAC approval.
     */
    return true;
}

static int defender_exclusion_state(void) {
    wchar_t exe[MAX_PATH], dll[MAX_PATH], exe_q[MAX_PATH * 2], dll_q[MAX_PATH * 2], command[4096];
    DWORD exit_code = 1;
    lstrcpynW(exe, target_dir, MAX_PATH); PathAppendW(exe, L"YAFSML.exe");
    lstrcpynW(dll, target_dir, MAX_PATH); PathAppendW(dll, L"YAFSML.dll");
    powershell_literal(exe, exe_q, sizeof(exe_q) / sizeof(exe_q[0]));
    powershell_literal(dll, dll_q, sizeof(dll_q) / sizeof(dll_q[0]));
    _snwprintf(command, sizeof(command) / sizeof(command[0]), L"$ErrorActionPreference='Stop'; try { $p=@(%ls,%ls); $e=(Get-MpPreference).ExclusionPath; if($p|Where-Object{$e -notcontains $_}){exit 1}else{exit 0} } catch { exit 2 }", exe_q, dll_q);
    if (!run_powershell(command, false, &exit_code)) return -1;
    if (exit_code == 0) return 1;
    if (exit_code == 1) return 0;
    return -1;
}

static bool defender_exclusions_present(void) {
    return defender_exclusion_state() == 1;
}

static bool wait_for_defender_state(int expected_state) {
    int attempt;
    for (attempt = 0; attempt < 20; attempt++) {
        if (defender_exclusion_state() == expected_state) return true;
        Sleep(250);
    }
    return false;
}

static bool set_defender_exclusions(bool add) {
    wchar_t exe[MAX_PATH], dll[MAX_PATH], exe_q[MAX_PATH * 2], dll_q[MAX_PATH * 2], command[4096];
    DWORD exit_code = 1;
    lstrcpynW(exe, target_dir, MAX_PATH); PathAppendW(exe, L"YAFSML.exe");
    lstrcpynW(dll, target_dir, MAX_PATH); PathAppendW(dll, L"YAFSML.dll");
    powershell_literal(exe, exe_q, sizeof(exe_q) / sizeof(exe_q[0]));
    powershell_literal(dll, dll_q, sizeof(dll_q) / sizeof(dll_q[0]));
    _snwprintf(command, sizeof(command) / sizeof(command[0]), add
        ? L"$ErrorActionPreference='Stop'; try { $p=@(%ls,%ls); $e=@((Get-MpPreference -ErrorAction Stop).ExclusionPath); foreach($v in $p){ if($e -notcontains $v){ Add-MpPreference -ExclusionPath $v -ErrorAction Stop } }; exit 0 } catch { exit 1 }"
        : L"$ErrorActionPreference='Stop'; try { $p=@(%ls,%ls); $e=@((Get-MpPreference -ErrorAction Stop).ExclusionPath); foreach($v in $p){ if($e -contains $v){ Remove-MpPreference -ExclusionPath $v -ErrorAction Stop } }; exit 0 } catch { exit 1 }", exe_q, dll_q);
    if (!run_powershell(command, true, &exit_code)) return false;
    /* Verify the actual Defender state.  This covers the runas case where
       ShellExecuteEx does not expose the elevated process handle. */
    return wait_for_defender_state(add ? 1 : 0);
}

static void apply_defender_status(int state) {
    defender_status_state = state;
    if (state > 0) {
        SetWindowTextW(control(IDC_DEFENDER), text(MGR_STR_REMOVE_EXCLUSION));
        EnableWindow(control(IDC_DEFENDER), TRUE);
        set_status(L"Windows Defender: exact YAFSML file exclusions are present");
    } else if (state == 0) {
        SetWindowTextW(control(IDC_DEFENDER), text(MGR_STR_DEFENDER));
        EnableWindow(control(IDC_DEFENDER), TRUE);
        set_status(L"Windows Defender: exact YAFSML file exclusions are not present");
    } else if (state == -1) {
        SetWindowTextW(control(IDC_DEFENDER), text(MGR_STR_DEFENDER_UNAVAILABLE));
        EnableWindow(control(IDC_DEFENDER), FALSE);
        set_status(L"Windows Defender: status could not be queried (service unavailable or policy-disabled)");
    } else {
        SetWindowTextW(control(IDC_DEFENDER), text(MGR_STR_DEFENDER_CHECKING));
        EnableWindow(control(IDC_DEFENDER), FALSE);
        set_status(L"Windows Defender: checking service status...");
    }
}

static DWORD WINAPI defender_status_thread_proc(LPVOID parameter) {
    HWND hwnd = (HWND)parameter;
    int state = defender_exclusion_state();
    PostMessageW(hwnd, WM_MANAGER_DEFENDER_STATUS, (WPARAM)(state + 2), 0);
    return 0;
}

static void start_defender_status_query(void) {
    HANDLE thread;
    if (InterlockedCompareExchange(&defender_query_in_progress, 1, 0) != 0) return;
    apply_defender_status(-2);
    thread = CreateThread(NULL, 0, defender_status_thread_proc, main_window, 0, NULL);
    if (thread != NULL) CloseHandle(thread);
    else {
        InterlockedExchange(&defender_query_in_progress, 0);
        apply_defender_status(-1);
    }
}

static void check_defender(void) {
    bool add;
    if (defender_status_state < 0) return;
    add = defender_status_state == 0;
    if (MessageBoxW(main_window,
                    add ? L"YAFSML.exe and YAFSML.dll are not excluded. Add exact-file exclusions with administrator approval?"
                        : L"Remove the exact YAFSML.exe and YAFSML.dll exclusions?",
                    L"YAFSML Manager", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (!set_defender_exclusions(add)) {
        MessageBoxW(main_window,
                    add ? L"Defender exclusion was not changed. Administrator approval may be required."
                        : L"Defender exclusions could not be removed. Administrator approval may be required.",
                    L"YAFSML Manager", MB_OK | MB_ICONWARNING);
        start_defender_status_query();
        return;
    }
    apply_defender_status(add ? 1 : 0);
}

static void add_label(HWND parent, int id, const wchar_t *text, int x, int y, int width) {
    CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
                  x, y, width, 24, parent, NULL, app_instance, NULL);
    (void)id;
}

static HWND add_group(HWND parent, const wchar_t *title, int x, int y, int width, int height) {
    return CreateWindowW(L"BUTTON", title, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                         x, y, width, height, parent, NULL, app_instance, NULL);
}

static HWND add_edit(HWND parent, int id, int x, int y, int width, int height) {
    (void)height;
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y, width, 24, parent, (HMENU)(INT_PTR)id, app_instance, NULL);
}

static HWND add_number_edit(HWND parent, int id, int x, int y, int width, int height) {
    (void)height;
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                           x, y, width, 24, parent, (HMENU)(INT_PTR)id, app_instance, NULL);
}

static HWND add_spin(HWND parent, int id, HWND buddy, int x, int y, int width, int height) {
    HWND spin = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT,
                                x, y, width, height, parent, (HMENU)(INT_PTR)id, app_instance, NULL);
    if (spin != NULL) {
        SendMessageW(spin, UDM_SETBUDDY, (WPARAM)buddy, 0);
        SendMessageW(spin, UDM_SETRANGE32, 0, 0x7fffffff);
    }
    return spin;
}

static HWND add_check(HWND parent, int id, const wchar_t *text, int x, int y, int width) {
    return CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, y, width, 24, parent, (HMENU)(INT_PTR)id, app_instance, NULL);
}

static void create_controls(HWND hwnd) {
    int y = 12;
    HWND combo;
    CreateWindowW(L"BUTTON", text(MGR_STR_OPEN), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 16, y, 72, 28, hwnd, (HMENU)(INT_PTR)IDC_OPEN, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_SAVE), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 94, y, 72, 28, hwnd, (HMENU)(INT_PTR)IDC_SAVE, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_SAVE_AS), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 172, y, 92, 28, hwnd, (HMENU)(INT_PTR)IDC_SAVE_AS, app_instance, NULL);

    add_label(hwnd, IDC_PATH, text(MGR_STR_YAFSML_DIRECTORY), 16, 52, 116); add_edit(hwnd, IDC_PATH, 140, 52, 400, 24);
    CreateWindowW(L"BUTTON", text(MGR_STR_BROWSE), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 548, 52, 68, 24, hwnd, (HMENU)(INT_PTR)IDC_DIRECTORY, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_UPDATE), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 626, 50, 116, 28, hwnd, (HMENU)(INT_PTR)IDC_UPDATE, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_UPDATE_MANAGER), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 750, 50, 148, 28, hwnd, (HMENU)(INT_PTR)IDC_MANAGER_UPDATE, app_instance, NULL);
    add_check(hwnd, IDC_GAME_DIRECTORY_OVERRIDE, text(MGR_STR_OVERRIDE_GAME_DIRECTORY), 16, 82, 156);
    add_edit(hwnd, IDC_GAME_DIRECTORY, 180, 82, 360, 24);
    CreateWindowW(L"BUTTON", text(MGR_STR_BROWSE), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 548, 82, 68, 24, hwnd, (HMENU)(INT_PTR)IDC_GAME_DIRECTORY_BROWSE, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_DEFENDER_CHECKING), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 626, 82, 272, 28, hwnd, (HMENU)(INT_PTR)IDC_DEFENDER, app_instance, NULL);

    add_group(hwnd, text(MGR_STR_GENERAL), 16, 114, 420, 78);
    add_label(hwnd, 0, text(MGR_STR_GAME), 32, 138, 54);
    combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 92, 138, 220, 24, hwnd, (HMENU)(INT_PTR)IDC_GAME, app_instance, NULL);
    SendMessageW(combo, CB_SETMINVISIBLE, 5, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"eldenring"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"armoredcore6"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"nightreign"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"sekiro"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"darksouls3");
    add_group(hwnd, text(MGR_STR_PATCH), 16, 198, 882, 130);
    add_check(hwnd, IDC_PATCH_SKIP_INTRO, L"Skip intro", 32, 224, 180); add_check(hwnd, IDC_PATCH_REG_SAVE, L"Prevent regulation save write", 224, 224, 260);
    add_check(hwnd, IDC_PATCH_MEM, L"Patch allocator", 32, 250, 180); add_check(hwnd, IDC_PATCH_DEDICATED, L"Dedicated heap", 224, 250, 180);
    add_label(hwnd, 0, L"Heap size (MB):", 430, 248, 100); add_number_edit(hwnd, IDC_PATCH_HEAP, 536, 248, 100, 24);
    add_check(hwnd, IDC_PATCH_BOOT, L"Boot boost", 32, 276, 180); add_check(hwnd, IDC_PATCH_ARXAN, L"Disable Arxan", 224, 276, 180); add_check(hwnd, IDC_PATCH_IME, L"Enable IME", 416, 276, 150);
    add_label(hwnd, 0, L"Save filename:", 32, 302, 96); add_edit(hwnd, IDC_SAVE_NAME, 136, 302, 220, 24); add_label(hwnd, 0, L"Seamless save:", 382, 302, 106); add_edit(hwnd, IDC_SEAMLESS_NAME, 494, 302, 220, 24);

    add_group(hwnd, text(MGR_STR_TWEAK), 16, 336, 420, 92);
    add_label(hwnd, 0, L"CPU affinity:", 32, 360, 120);
    combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 158, 360, 250, 24, hwnd, (HMENU)(INT_PTR)IDC_CPU, app_instance, NULL);
    SendMessageW(combo, CB_SETMINVISIBLE, 5, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text(MGR_STR_CPU_KEEP)); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text(MGR_STR_CPU_EXCEPT_FIRST)); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text(MGR_STR_CPU_EFFICIENT)); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text(MGR_STR_CPU_PERFORMANCE)); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text(MGR_STR_CPU_PERFORMANCE_EXCEPT_FIRST));
    add_group(hwnd, text(MGR_STR_LOG), 452, 336, 446, 92);
    add_check(hwnd, IDC_CONSOLE, L"Console", 470, 360, 90); add_check(hwnd, IDC_LOG_FILE, L"Log file", 566, 360, 90);
    add_label(hwnd, 0, L"Level:", 668, 360, 54);
    combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 728, 360, 126, 24, hwnd, (HMENU)(INT_PTR)IDC_LOG_LEVEL, app_instance, NULL);
    SendMessageW(combo, CB_SETMINVISIBLE, 6, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"trace"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"debug"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"info"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"warn"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"error"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"off");
    add_group(hwnd, text(MGR_STR_DLL), 16, 438, 882, 196);
    CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_VSCROLL, 30, 464, 250, 138, hwnd, (HMENU)(INT_PTR)IDC_DLL_LIST, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_ADD), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 292, 464, 68, 26, hwnd, (HMENU)(INT_PTR)IDC_ADD_DLL, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_REMOVE), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 368, 464, 78, 26, hwnd, (HMENU)(INT_PTR)IDC_REMOVE_DLL, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_UP), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 454, 464, 58, 26, hwnd, (HMENU)(INT_PTR)IDC_MOVE_DLL_UP, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_DOWN), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 520, 464, 68, 26, hwnd, (HMENU)(INT_PTR)IDC_MOVE_DLL_DOWN, app_instance, NULL);
    add_label(hwnd, 0, text(MGR_STR_NAME), 292, 502, 50); add_edit(hwnd, IDC_DLL_NAME, 348, 502, 532, 24);
    add_label(hwnd, 0, text(MGR_STR_PATH), 292, 534, 50); add_edit(hwnd, IDC_DLL_PATH, 348, 534, 462, 24);
    CreateWindowW(L"BUTTON", text(MGR_STR_BROWSE), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 812, 534, 68, 24, hwnd, (HMENU)(INT_PTR)IDC_DLL_BROWSE, app_instance, NULL);
    add_label(hwnd, 0, text(MGR_STR_STAGE), 292, 566, 50);
    combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 348, 566, 120, 24, hwnd, (HMENU)(INT_PTR)IDC_DLL_STAGE, app_instance, NULL);
    SendMessageW(combo, CB_SETMINVISIBLE, 5, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"none"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"early"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"data_ready"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"delay"); SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"after");
    add_number_edit(hwnd, IDC_DLL_DELAY, 476, 566, 56, 24); add_spin(hwnd, IDC_DLL_DELAY_SPIN, GetDlgItem(hwnd, IDC_DLL_DELAY), 514, 566, 18, 24);
    combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 476, 566, 160, 24, hwnd, (HMENU)(INT_PTR)IDC_DLL_AFTER, app_instance, NULL);
    SendMessageW(combo, CB_SETMINVISIBLE, 6, 0);
    add_group(hwnd, text(MGR_STR_MOD), 16, 644, 882, 118);
    CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_VSCROLL, 30, 670, 250, 86, hwnd, (HMENU)(INT_PTR)IDC_MOD_LIST, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_ADD), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 292, 670, 68, 26, hwnd, (HMENU)(INT_PTR)IDC_ADD_MOD, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_REMOVE), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 368, 670, 78, 26, hwnd, (HMENU)(INT_PTR)IDC_REMOVE_MOD, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_UP), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 454, 670, 58, 26, hwnd, (HMENU)(INT_PTR)IDC_MOVE_MOD_UP, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_DOWN), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 520, 670, 68, 26, hwnd, (HMENU)(INT_PTR)IDC_MOVE_MOD_DOWN, app_instance, NULL);
    add_label(hwnd, 0, text(MGR_STR_NAME), 292, 708, 50); add_edit(hwnd, IDC_MOD_NAME, 348, 708, 532, 24);
    add_label(hwnd, 0, text(MGR_STR_PATH), 292, 740, 50); add_edit(hwnd, IDC_MOD_PATH, 348, 740, 462, 24);
    CreateWindowW(L"BUTTON", text(MGR_STR_BROWSE), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 812, 740, 68, 24, hwnd, (HMENU)(INT_PTR)IDC_MOD_BROWSE, app_instance, NULL);
    CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 16, 772, 640, 22, hwnd, (HMENU)(INT_PTR)IDC_COMPAT_STATUS, app_instance, NULL);
    CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 16, 798, 640, 22, hwnd, (HMENU)(INT_PTR)IDC_STATUS, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_SHORTCUT), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 680, 786, 112, 28, hwnd, (HMENU)(INT_PTR)IDC_SHORTCUT, app_instance, NULL);
    CreateWindowW(L"BUTTON", text(MGR_STR_LAUNCH), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 800, 786, 98, 28, hwnd, (HMENU)(INT_PTR)IDC_LAUNCH, app_instance, NULL);
}

static LRESULT CALLBACK manager_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: main_window = hwnd; create_controls(hwnd); initialize_ui_font(hwnd); load_controls(); start_defender_status_query(); return 0;
        case WM_MANAGER_STARTUP_UPDATE: check_startup_updates(); return 0;
        case WM_MANAGER_DEFENDER_STATUS:
            InterlockedExchange(&defender_query_in_progress, 0);
            apply_defender_status((int)wparam - 2);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDC_OPEN: open_config(); break; case IDC_SAVE: save_config(false); break; case IDC_SAVE_AS: save_config(true); break;
                case IDC_LAUNCH: launch_config(); break; case IDC_SHORTCUT: create_shortcut(); break; case IDC_UPDATE: check_updates(); break; case IDC_MANAGER_UPDATE: check_manager_updates(); break; case IDC_DEFENDER: check_defender(); break; case IDC_DIRECTORY: browse_directory(); break; case IDC_GAME_DIRECTORY_BROWSE: if (HIWORD(wparam) == BN_CLICKED) browse_game_directory(); break;
                case IDC_ADD_DLL: add_dynamic(true); break; case IDC_REMOVE_DLL: remove_dynamic(true); break; case IDC_MOVE_DLL_UP: move_dynamic(true, -1); break; case IDC_MOVE_DLL_DOWN: move_dynamic(true, 1); break; case IDC_ADD_MOD: add_dynamic(false); break; case IDC_REMOVE_MOD: remove_dynamic(false); break; case IDC_MOVE_MOD_UP: move_dynamic(false, -1); break; case IDC_MOVE_MOD_DOWN: move_dynamic(false, 1); break;
                case IDC_DLL_LIST: if (HIWORD(wparam) == LBN_SELCHANGE) sync_dynamic_detail(true); break;
                case IDC_MOD_LIST: if (HIWORD(wparam) == LBN_SELCHANGE) sync_dynamic_detail(false); break;
                case IDC_GAME: if (HIWORD(wparam) == CBN_SELCHANGE) { read_controls(); apply_game_compatibility(); dirty = true; } break;
                case IDC_PATCH_MEM: if (HIWORD(wparam) == BN_CLICKED) { read_controls(); apply_game_compatibility(); dirty = true; } break;
                case IDC_PATCH_DEDICATED: if (HIWORD(wparam) == BN_CLICKED) { read_controls(); apply_game_compatibility(); dirty = true; } break;
                case IDC_DLL_NAME: case IDC_DLL_PATH: if (HIWORD(wparam) == EN_CHANGE) sync_dynamic_from_detail(true, LOWORD(wparam)); break;
                case IDC_DLL_BROWSE: if (HIWORD(wparam) == BN_CLICKED) browse_dynamic_file(true); break;
                case IDC_DLL_DELAY: if (HIWORD(wparam) == EN_CHANGE) sync_dll_condition_from_controls(); break;
                case IDC_DLL_STAGE: case IDC_DLL_AFTER: if (HIWORD(wparam) == CBN_SELCHANGE) sync_dll_condition_from_controls(); break;
                case IDC_GAME_DIRECTORY: if (HIWORD(wparam) == EN_CHANGE && !loading_controls) get_text(IDC_GAME_DIRECTORY, game_dir, MAX_PATH); break;
                case IDC_GAME_DIRECTORY_OVERRIDE: if (HIWORD(wparam) == BN_CLICKED) { override_game_dir = get_check(IDC_GAME_DIRECTORY_OVERRIDE); apply_game_directory_controls(); } break;
                case IDC_MOD_NAME: case IDC_MOD_PATH: if (HIWORD(wparam) == EN_CHANGE) sync_dynamic_from_detail(false, LOWORD(wparam)); break;
                case IDC_MOD_BROWSE: if (HIWORD(wparam) == BN_CLICKED) browse_dynamic_file(false); break;
                default: if (!loading_controls && (HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == BN_CLICKED)) dirty = true; break;
            } return 0;
        case WM_CLOSE: if (ensure_saved()) DestroyWindow(hwnd); return 0;
        case WM_ERASEBKGND: {
            RECT rect;
            GetClientRect(hwnd, &rect);
            FillRect((HDC)wparam, &rect, GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkMode((HDC)wparam, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        case WM_DESTROY:
            save_settings();
            if (own_ui_font && ui_font != NULL) DeleteObject(ui_font);
            PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show_command) {
    WNDCLASSW window_class = { 0 };
    MSG message;
    wchar_t module_path[MAX_PATH];
    (void)previous; (void)command_line;
    app_instance = instance;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitCommonControls();
    GetModuleFileNameW(NULL, module_path, MAX_PATH); lstrcpynW(target_dir, module_path, MAX_PATH); PathRemoveFileSpecW(target_dir);
    config_path[0] = L'\0';
    settings_location();
    load_settings();
    if (config_path[0] == L'\0') {
        wchar_t default_config[MAX_PATH];
        lstrcpynW(default_config, target_dir, MAX_PATH);
        PathAppendW(default_config, L"YAFSML.ini");
        if (PathFileExistsW(default_config)) lstrcpynW(config_path, default_config, MAX_PATH);
    }
    if (config_path[0] == L'\0' || !manager_config_load(&app_config, config_path)) manager_config_defaults(&app_config);
    window_class.hInstance = instance; window_class.lpfnWndProc = manager_window_proc; window_class.lpszClassName = L"YAFSMLManagerWindow"; window_class.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(IDC_ARROW));
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&window_class);
    main_window = CreateWindowW(window_class.lpszClassName, text(MGR_STR_TITLE), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 930, 860, NULL, NULL, instance, NULL);
    ShowWindow(main_window, show_command); UpdateWindow(main_window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    save_settings();
    CoUninitialize(); return (int)message.wParam;
}
