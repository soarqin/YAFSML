#include "regulation.h"
#include "modloader/config.h"
#include "modloader/mod.h"

#ifndef ML_REGULATION_TEST
#include "regulation_sig.h"
#include "log.h"

#include "common/allocator.h"
#include "modloader/hook.h"

#include "process/fd4_step.h"
#include "process/image.h"
#include "process/pe.h"
#include "process/singleton.h"
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct dl_vector_ptr_msvc2015_s {
    void *allocator;
    void **first;
    void **last;
    void **end;
} dl_vector_ptr_msvc2015_t;

typedef struct cs_regulation_manager_s {
    void *vtable;
    void *regulation_step;
    dl_vector_ptr_msvc2015_t param_res_caps;
    uint8_t *raw_regulation;
    size_t raw_regulation_len;
} cs_regulation_manager_t;

_Static_assert(offsetof(cs_regulation_manager_t, raw_regulation) == 0x30, "CSRegulationManager raw_regulation offset mismatch");
_Static_assert(offsetof(cs_regulation_manager_t, raw_regulation_len) == 0x38, "CSRegulationManager raw_regulation_len offset mismatch");

static void suppress_fd4_regulation_save(cs_regulation_manager_t *manager) {
    if (manager != NULL) manager->raw_regulation_len = 0;
}

static bool __cdecl skip_regulation_write(void *state) {
    (void)state;
    return true;
}

bool ml_regulation_override_present(void) {
    return mods_file_search(L"regulation.bin") != NULL;
}

bool ml_regulation_requested(void) {
    return config.prevent_regulation_save_write && ml_regulation_override_present();
}

#ifdef ML_REGULATION_TEST
void ml_regulation_test_suppress_fd4_save(void *manager) {
    suppress_fd4_regulation_save((cs_regulation_manager_t *)manager);
}

bool ml_regulation_test_skip_write(void *state) {
    return skip_regulation_write(state);
}
#else
static fd4_step_fn_t old_regulation_step_idle;

static void __cdecl regulation_step_idle_hooked(void *this_ptr, fd4_time_t *time) {
    cs_regulation_manager_t *manager;

    old_regulation_step_idle(this_ptr, time);
    manager = singleton_find("CSRegulationManager");
    suppress_fd4_regulation_save(manager);
}

static bool install_fd4(void) {
    void *step = fd4_step_find(L"CSRegulationStep::STEP_Idle");
    return step != NULL &&
           ml_hook_install(step, regulation_step_idle_hooked, (void **)&old_regulation_step_idle) == ML_HOOK_APPLIED;
}

static bool install_sprj(void) {
    size_t image_size = 0;
    void *image = get_module_image_base(NULL, &image_size);
    const IMAGE_SECTION_HEADER *text = pe_section_by_name(image, ".text");
    size_t text_size = 0;
    uint8_t *base = pe_section_data(image, text, &text_size);
    uint8_t **writers;
    size_t writer_capacity;
    size_t installed = 0;
    size_t matched = 0;

    if (base == NULL || text_size == 0) return false;
    writer_capacity =
        ml_regulation_sprj_find_writers(base, text_size, NULL, 0);
    if (writer_capacity == 0 ||
        writer_capacity > SIZE_MAX / sizeof(*writers)) {
        return false;
    }
    writers = ml_mem_alloc(0, writer_capacity * sizeof(*writers));
    if (writers == NULL) {
        ML_LOG_WARN(L"regulation",
                    L"SPRJ regulation writer target allocation failed");
        return false;
    }
    matched = ml_regulation_sprj_find_writers(
        base, text_size, writers, writer_capacity);
    for (size_t i = 0; i < matched; i++) {
        if (ml_hook_install(writers[i], skip_regulation_write, NULL) ==
            ML_HOOK_APPLIED) {
            installed++;
        }
    }
    ml_mem_free(writers);
    ML_LOG_DEBUG(L"regulation",
                 L"SPRJ regulation writer scan matched %zu target(s), "
                 L"installed %zu hook(s)",
                 matched, installed);
    if (matched != 0 && installed == 0) {
        ML_LOG_WARN(L"regulation", L"SPRJ regulation writer matched but hooks were blocked");
    } else if (installed != matched) {
        ML_LOG_WARN(L"regulation",
                    L"SPRJ regulation writer hook coverage is partial: "
                    L"%zu of %zu installed",
                    installed, matched);
    }
    return matched != 0 && installed == matched;
}

bool ml_regulation_install(const ml_game_descriptor_t *game) {
    bool result = false;
    if (game == NULL) return false;
    if (!config.prevent_regulation_save_write) {
        ML_LOG_INFO(L"regulation", L"protection SKIPPED_DISABLED for %ls", game->title);
        return true;
    }
    if (!ml_regulation_override_present()) {
        ML_LOG_INFO(L"regulation", L"protection SKIPPED_NO_OVERRIDE for %ls", game->title);
        return true;
    }
    if (game->regulation_strategy == ML_REGULATION_STRATEGY_FD4) result = install_fd4();
    if (game->regulation_strategy == ML_REGULATION_STRATEGY_SPRJ) result = install_sprj();
    ml_log_write(result ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 L"regulation", result
                     ? L"protection APPLIED for %ls"
                     : L"protection SIGNATURE_NOT_FOUND for %ls", game->title);
    return result;
}
#endif
