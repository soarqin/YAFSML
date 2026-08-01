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

bool ml_log_level_parse(const char *value, ml_log_level_t *level);
const wchar_t *ml_log_level_name(ml_log_level_t level);
void ml_log_enable_file(const wchar_t *path);
void ml_log_enable_console();
void ml_log_set_level(ml_log_level_t level);
ml_log_level_t ml_log_get_level();

#if defined(__GNUC__)
    #define FORCE_INLINE inline static
#elif defined(__clang__)
    #define FORCE_INLINE __attribute__((always_inline)) inline static
#elif defined(_MSC_VER)
    #define FORCE_INLINE __forceinline
#else
    #define FORCE_INLINE inline
#endif

extern volatile ml_log_level_t current_level;
void ml_log_vwrite(ml_log_level_t level, const wchar_t *component,
                   const wchar_t *format, va_list args);
FORCE_INLINE void ml_log_write(ml_log_level_t level, const wchar_t *component,
                  const wchar_t *format, ...) {
    if (level < current_level) return;
    va_list args;
    va_start(args, format);
    ml_log_vwrite(level, component, format, args);
    va_end(args);
}

#define ML_LOG(level, component, ...) ml_log_write((level), (component), __VA_ARGS__)
#define ML_LOG_TRACE(component, ...) ml_log_write(ML_LOG_LEVEL_TRACE, (component), __VA_ARGS__)
#define ML_LOG_DEBUG(component, ...) ml_log_write(ML_LOG_LEVEL_DEBUG, (component), __VA_ARGS__)
#define ML_LOG_INFO(component, ...) ml_log_write(ML_LOG_LEVEL_INFO, (component), __VA_ARGS__)
#define ML_LOG_WARN(component, ...) ml_log_write(ML_LOG_LEVEL_WARN, (component), __VA_ARGS__)
#define ML_LOG_ERROR(component, ...) ml_log_write(ML_LOG_LEVEL_ERROR, (component), __VA_ARGS__)

#ifdef __cplusplus
}
#endif
