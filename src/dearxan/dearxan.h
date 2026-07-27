/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DearxanStatus {
    DearxanInvalid,
    DearxanSuccess,
    DearxanError,
    DearxanPanic,
    DearxanMaxStatus
} DearxanStatus;

typedef struct DearxanResult {
    size_t result_size;
    int status;
    const char *error_msg;
    size_t error_msg_size;
    bool is_arxan_detected;
    bool is_executing_entrypoint;
    char _last_for_offsetof;
} DearxanResult;

#define DEARXAN_RESULT_SIZE offsetof(DearxanResult, _last_for_offsetof)
#define DEARXAN_RESULT_FIELD(ptr, field, then_expr, else_expr) do { \
        if (offsetof(DearxanResult, field) < (ptr)->result_size) {  \
            then_expr;                                             \
        } else {                                                   \
            else_expr;                                             \
        }                                                          \
    } while (0)

typedef void (*DearxanUserCallback)(const DearxanResult *result, void *opaque);
typedef void (*DearxanScheduleCallback)(bool is_arxan_detected,
                                        bool is_executing_entrypoint,
                                        void *opaque);

/* The scheduler is also used by me3 when Arxan neutralization is disabled. */
void dearxan_neuter_arxan(DearxanUserCallback callback, void *opaque);
void dearxan_schedule_after_arxan(DearxanScheduleCallback callback, void *opaque);

#ifdef __cplusplus
}
#endif
