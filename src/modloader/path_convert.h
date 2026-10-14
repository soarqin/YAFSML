#pragma once

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

wchar_t *ml_path_from_ansi(const char *path);

#ifdef __cplusplus
}
#endif
