/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "cpu_sets.h"

static bool is_usable_cpu_set(const process_cpu_set_info_t *set) {
    return set != NULL && (!set->allocated || set->allocated_to_target_process);
}

static bool find_efficiency_classes(const process_cpu_set_info_t *sets,
                                    const size_t set_count,
                                    uint8_t *min_class,
                                    uint8_t *max_class) {
    bool found = false;
    for (size_t i = 0; i < set_count; i++) {
        if (!is_usable_cpu_set(&sets[i])) continue;
        if (!found || sets[i].efficiency_class < *min_class) {
            *min_class = sets[i].efficiency_class;
        }
        if (!found || sets[i].efficiency_class > *max_class) {
            *max_class = sets[i].efficiency_class;
        }
        found = true;
    }
    if (!found) return false;
    return true;
}

static bool is_lower_logical_cpu(const process_cpu_set_info_t *lhs,
                                 const process_cpu_set_info_t *rhs) {
    if (lhs->group != rhs->group) return lhs->group < rhs->group;
    if (lhs->logical_processor_index != rhs->logical_processor_index) {
        return lhs->logical_processor_index < rhs->logical_processor_index;
    }
    return lhs->id < rhs->id;
}

static bool find_first_matching_cpu_set(const process_cpu_set_info_t *sets,
                                        const size_t set_count,
                                        const bool match_class,
                                        const uint8_t selected_class,
                                        process_cpu_set_info_t *first) {
    bool found = false;
    for (size_t i = 0; i < set_count; i++) {
        if (!is_usable_cpu_set(&sets[i])) continue;
        if (match_class && sets[i].efficiency_class != selected_class) continue;
        if (!found || is_lower_logical_cpu(&sets[i], first)) {
            *first = sets[i];
            found = true;
        }
    }
    return found;
}

static bool should_select_cpu_set(const int strategy,
                                  const process_cpu_set_info_t *set,
                                  const uint8_t selected_class,
                                  const process_cpu_set_info_t *excluded) {
    if (!is_usable_cpu_set(set)) return false;
    if (excluded != NULL && set->id == excluded->id) return false;
    switch (strategy) {
        case 1:
            return true;
        case 2:
            return set->efficiency_class == selected_class;
        case 3:
        case 4:
            return set->efficiency_class == selected_class;
        default:
            return false;
    }
}

size_t select_process_cpu_set_ids(const int strategy,
                                  const process_cpu_set_info_t *sets,
                                  const size_t set_count,
                                  process_cpu_set_id_t *ids,
                                  const size_t ids_capacity) {
    if (sets == NULL || strategy < 1 || strategy > 4) return 0;

    uint8_t min_class = 0;
    uint8_t max_class = 0;
    if (!find_efficiency_classes(sets, set_count, &min_class, &max_class)) {
        return 0;
    }
    if (strategy >= 2 && min_class == max_class) return 0;

    const uint8_t selected_class = strategy == 2 ? min_class : max_class;

    process_cpu_set_info_t excluded = {0};
    process_cpu_set_info_t *excluded_ptr = NULL;
    if ((strategy == 1 || strategy == 4) &&
        find_first_matching_cpu_set(sets, set_count, strategy == 4,
                                    selected_class,
                                    &excluded)) {
        excluded_ptr = &excluded;
    }

    size_t selected_count = 0;
    for (size_t i = 0; i < set_count; i++) {
        if (!should_select_cpu_set(strategy, &sets[i], selected_class,
                                   excluded_ptr)) {
            continue;
        }
        if (ids != NULL && selected_count < ids_capacity) {
            ids[selected_count] = sets[i].id;
        }
        selected_count++;
    }
    return selected_count;
}
