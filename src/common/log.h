#pragma once

#include <stdbool.h>
#include <stdarg.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ml_log_level_e {
    ML_LOG_LEVEL_TRACE = 0,
    ML_LOG_LEVEL_DEBUG,
    ML_LOG_LEVEL_INFO,
    ML_LOG_LEVEL_WARN,
    ML_LOG_LEVEL_ERROR,
    ML_LOG_LEVEL_OFF
} ml_log_level_t;

/* Receives every formatted message at or above the current level, in addition
 * to the console and file outputs. `message` excludes the level and component. */
typedef void (*ml_log_sink_t)(ml_log_level_t level, const wchar_t *component,
                              const wchar_t *message);

bool ml_log_level_parse(const char *value, ml_log_level_t *level);
const wchar_t *ml_log_level_name(ml_log_level_t level);
void ml_log_enable_file(const wchar_t *path);
void ml_log_enable_console(void);
void ml_log_set_sink(ml_log_sink_t sink);
void ml_log_set_level(ml_log_level_t level);
ml_log_level_t ml_log_get_level(void);

extern volatile ml_log_level_t ml_log_current_level;
void ml_log_vwrite(ml_log_level_t level, const wchar_t *component,
                   const wchar_t *format, va_list args);
void ml_log_write(ml_log_level_t level, const wchar_t *component,
                  const wchar_t *format, ...);

/* The level check happens before the arguments are evaluated, so a filtered
 * message costs one comparison. Arguments must therefore be free of side
 * effects. */
#define ML_LOG(level, component, ...)                                          \
    do {                                                                       \
        const ml_log_level_t ml_log_level_ = (level);                          \
        if (ml_log_level_ >= ml_log_current_level)                             \
            ml_log_write(ml_log_level_, (component), __VA_ARGS__);             \
    } while (0)
#define ML_LOG_TRACE(component, ...) ML_LOG(ML_LOG_LEVEL_TRACE, (component), __VA_ARGS__)
#define ML_LOG_DEBUG(component, ...) ML_LOG(ML_LOG_LEVEL_DEBUG, (component), __VA_ARGS__)
#define ML_LOG_INFO(component, ...) ML_LOG(ML_LOG_LEVEL_INFO, (component), __VA_ARGS__)
#define ML_LOG_WARN(component, ...) ML_LOG(ML_LOG_LEVEL_WARN, (component), __VA_ARGS__)
#define ML_LOG_ERROR(component, ...) ML_LOG(ML_LOG_LEVEL_ERROR, (component), __VA_ARGS__)

#ifdef __cplusplus
}
#endif
