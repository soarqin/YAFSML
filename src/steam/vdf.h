/* Reads Valve's text KeyValues (VDF) files the way the Steam client does; see
 * vdf.c. The interface follows libofdf (https://github.com/Jan200101/libofdf),
 * licensed under MIT. */

#pragma once

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

enum vdf_data_type {
    /* A key that was never given a value or a block. */
    VDF_TYPE_NONE,
    /* A key followed by a { } block. */
    VDF_TYPE_ARRAY,
    /* A key with a value, stored as KeyValues::GetString returns it. */
    VDF_TYPE_STRING
};

struct vdf_object;

union vdf_data {
    struct {
        struct vdf_object **data_value;
        size_t len;
    } data_array;

    struct {
        char *str;
    } data_string;
};

struct vdf_object {
    char *key;
    enum vdf_data_type type;
    union vdf_data data;
};

/* Returns the first top-level entry, or NULL when Steam would reject the
 * input. Input without any entry yields an object without a key. */
struct vdf_object *vdf_parse_buffer(const char *, size_t);
struct vdf_object *vdf_parse_file(const wchar_t *);

size_t vdf_object_get_array_length(const struct vdf_object *);
struct vdf_object *vdf_object_index_array(const struct vdf_object *, size_t);
/* KeyValues::FindKey: the first child whose name matches, ignoring ASCII case. */
struct vdf_object *vdf_object_index_array_str(const struct vdf_object *, const char *);

const char *vdf_object_get_string(const struct vdf_object *);

void vdf_free_object(struct vdf_object *);

#ifdef __cplusplus
}
#endif
