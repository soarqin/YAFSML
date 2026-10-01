/* Codes from: [libofdf](https://github.com/Jan200101/libofdf), licensed under MIT */

#include <stdlib.h>
#include <stdio.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "vdf.h"

#define CHAR_SPACE ' '
#define CHAR_TAB '\t'
#define CHAR_NEWLINE '\n'
#define CHAR_CARRIAGE_RETURN '\r'
#define CHAR_DOUBLE_QUOTE '"'
#define CHAR_OPEN_CURLY_BRACKET '{'
#define CHAR_CLOSED_CURLY_BRACKET '}'
#define CHAR_OPEN_ANGLED_BRACKET '['
#define CHAR_CLOSED_ANGLED_BRACKET ']'
#define CHAR_FRONTSLASH '/'
#define CHAR_BACKSLASH '\\'

#define FMT_UNKNOWN_CHAR "Encountered Unknown Character %c (%li)\n"

static char *local_strndup_escape(const char *s, const size_t n) {
    if (!s)
        return NULL;

    char *retval = LocalAlloc(0, n + 1);
    if (retval == NULL)
        return NULL;
    strncpy(retval, s, n);
    retval[n] = '\0';

    char *head = retval;
    const char *tail = retval + n;

    while (*head) {
        if (*head == CHAR_BACKSLASH) {
            switch (head[1]) {
                case 'n':
                    memmove(head, head + 1, (size_t)(tail - head));
                    *head = CHAR_NEWLINE;
                    break;

                case 't':
                    memmove(head, head + 1, (size_t)(tail - head));
                    *head = CHAR_TAB;
                    break;

                case CHAR_BACKSLASH:
                case CHAR_DOUBLE_QUOTE:
                    memmove(head, head + 1, (size_t)(tail - head));
                    break;
            }
        }
        ++head;
    }

    return retval;
}

struct vdf_object *vdf_parse_buffer(const char *buffer, const size_t size) {
    if (!buffer)
        return NULL;

    struct vdf_object *root_object = LocalAlloc(0, sizeof(struct vdf_object));
    if (root_object == NULL)
        return NULL;
    root_object->key = NULL;
    root_object->parent = NULL;
    root_object->type = VDF_TYPE_NONE;
    root_object->conditional = NULL;

    struct vdf_object *o = root_object;

    const char *head = buffer;
    const char *tail = head;

    const char *end = buffer + size;

    const char *buf = NULL;

    while (end > tail) {
        switch (*tail) {
            case CHAR_DOUBLE_QUOTE:
                if (tail > buffer && *(tail - 1) == CHAR_BACKSLASH)
                    break;

                if (!buf) {
                    buf = tail + 1;
                } else if (o->key) {
                    const size_t len = tail - buf;
                    size_t digits = 0;
                    size_t chars = 0;

                    for (size_t i = 0; i < len; ++i) {
                        if (isdigit((unsigned char)buf[i]))
                            digits++;

                        if (isalpha((unsigned char)buf[i]))
                            chars++;
                    }

                    if (len && digits == len) {
                        o->type = VDF_TYPE_INT;
                    } else {
                        o->type = VDF_TYPE_STRING;
                    }

                    switch (o->type) {
                        case VDF_TYPE_INT:
                            o->data.data_int = strtoll(buf, NULL, 10);
                            break;

                        case VDF_TYPE_STRING:
                            o->data.data_string.len = len;
                            o->data.data_string.str = local_strndup_escape(buf, len);
                            break;

                        default:
                            assert(0);
                            break;
                    }

                    buf = NULL;

                    if (o->parent && o->parent->type == VDF_TYPE_ARRAY) {
                        struct vdf_object **grown;
                        struct vdf_object *child;
                        size_t new_len;
                        o = o->parent;
                        assert(o->type == VDF_TYPE_ARRAY);

                        new_len = o->data.data_array.len + 1;
                        grown = LocalReAlloc(o->data.data_array.data_value, (sizeof(void *)) * (new_len + 1), LMEM_MOVEABLE);
                        child = grown == NULL ? NULL : LocalAlloc(0, sizeof(struct vdf_object));
                        if (grown != NULL) o->data.data_array.data_value = grown;
                        if (child == NULL) {
                            vdf_free_object(root_object);
                            return NULL;
                        }
                        o->data.data_array.len = new_len;
                        grown[new_len] = child;
                        child->parent = o;

                        o = child;
                        o->key = NULL;
                        o->type = VDF_TYPE_NONE;
                        o->conditional = NULL;
                    }
                } else {
                    const size_t len = tail - buf;
                    o->key = local_strndup_escape(buf, len);
                    buf = NULL;
                }
                break;

            case CHAR_OPEN_CURLY_BRACKET:
                assert(!buf);
                assert(o->type == VDF_TYPE_NONE);

                {
                    /* Allocate both the slot array and the first child before
                     * mutating `o` into an array, so a failed allocation leaves
                     * `o` as VDF_TYPE_NONE and vdf_free_object stays safe. */
                    struct vdf_object **arr = LocalAlloc(0, sizeof(void *) * 1);
                    struct vdf_object *child = arr == NULL ? NULL : LocalAlloc(0, sizeof(struct vdf_object));
                    if (child == NULL) {
                        LocalFree(arr);
                        vdf_free_object(root_object);
                        return NULL;
                    }
                    if (o->parent && o->parent->type == VDF_TYPE_ARRAY)
                        o->parent->data.data_array.len++;

                    o->type = VDF_TYPE_ARRAY;
                    o->data.data_array.len = 0;
                    o->data.data_array.data_value = arr;
                    arr[0] = child;
                    child->parent = o;

                    o = child;
                }
                o->key = NULL;
                o->type = VDF_TYPE_NONE;
                o->conditional = NULL;
                break;

            case CHAR_CLOSED_CURLY_BRACKET:
                assert(!buf);

                o = o->parent;
                if (o == NULL) {
                    /* Unbalanced '}' in malformed input; assert() is a no-op in
                     * release builds, so bail out instead of dereferencing NULL. */
                    vdf_free_object(root_object);
                    return NULL;
                }
                if (o->parent) {
                    o = o->parent;
                    assert(o->type == VDF_TYPE_ARRAY);

                    o->data.data_array.data_value = LocalReAlloc(o->data.data_array.data_value, (sizeof(void *)) * (o->data.data_array.len + 1), LMEM_MOVEABLE);
                    o->data.data_array.data_value[o->data.data_array.len] = LocalAlloc(0, sizeof(struct vdf_object)),
                        o->data.data_array.data_value[o->data.data_array.len]->parent = o;

                    o = o->data.data_array.data_value[o->data.data_array.len];
                    o->key = NULL;
                    o->type = VDF_TYPE_NONE;
                    o->conditional = NULL;
                }

                break;

            case CHAR_FRONTSLASH:
                if (!buf)
                    while (*tail != '\0' && *tail != CHAR_NEWLINE)
                        ++tail;

                break;

            case CHAR_OPEN_ANGLED_BRACKET:
                if (!buf) {
                    struct vdf_object *prev;
                    if (o->parent == NULL || o->parent->type != VDF_TYPE_ARRAY ||
                        o->parent->data.data_array.len == 0) {
                        /* Conditional with no preceding entry in malformed input. */
                        vdf_free_object(root_object);
                        return NULL;
                    }
                    prev = o->parent->data.data_array.data_value[o->parent->data.data_array.len - 1];
                    assert(!prev->conditional);

                    buf = tail + 1;

                    while (*tail != '\0' && *tail != CHAR_CLOSED_ANGLED_BRACKET)
                        ++tail;

                    prev->conditional = local_strndup_escape(buf, tail - buf);

                    buf = NULL;
                }

                break;

            default:
                if (!buf) {
                    // we found something we are probably not suppose to
                    // the easiest way out is to just terminate
                    vdf_free_object(root_object);
                    return NULL;
                }
                break;

            case CHAR_NEWLINE:
            case CHAR_CARRIAGE_RETURN:
            case CHAR_SPACE:
            case CHAR_TAB:
                break;
        }
        ++tail;
    }
    return root_object;
}

struct vdf_object *vdf_parse_file(const wchar_t *path) {
    struct vdf_object *o = NULL;
    FILE *fd;
    long file_size;

    if (!path)
        return NULL;

    /* Binary mode: text mode performs CRLF translation, so fread would return
     * fewer bytes than ftell reported, leaving the tail uninitialized. */
    fd = _wfopen(path, L"rb");
    if (!fd)
        return NULL;

    if (fseek(fd, 0L, SEEK_END) != 0) {
        fclose(fd);
        return NULL;
    }
    file_size = ftell(fd);
    rewind(fd);

    if (file_size > 0) {
        char *buffer = LocalAlloc(0, (size_t)file_size + 1);
        if (buffer != NULL) {
            /* Parse only the bytes actually read and NUL-terminate so the
             * comment/conditional scanners in vdf_parse_buffer cannot run past
             * the end of the buffer. */
            size_t read = fread(buffer, 1, (size_t)file_size, fd);
            buffer[read] = '\0';
            o = vdf_parse_buffer(buffer, read);
            LocalFree(buffer);
        }
    }

    fclose(fd);
    return o;
}


size_t vdf_object_get_array_length(const struct vdf_object *o) {
    assert(o);
    assert(o->type == VDF_TYPE_ARRAY);

    return o->data.data_array.len;
}

struct vdf_object *vdf_object_index_array(const struct vdf_object *o, const size_t index) {
    assert(o);
    assert(o->type == VDF_TYPE_ARRAY);
    assert(o->data.data_array.len > index);

    return o->data.data_array.data_value[index];
}

struct vdf_object *vdf_object_index_array_str(const struct vdf_object *o, const char *str) {
    if (!o || !str || o->type != VDF_TYPE_ARRAY)
        return NULL;

    for (size_t i = 0; i < o->data.data_array.len; ++i) {
        struct vdf_object *k = o->data.data_array.data_value[i];
        if (k == NULL || k->key == NULL)
            continue;
        if (!strcmp(k->key, str))
            return k;
    }
    return NULL;
}

const char *vdf_object_get_string(const struct vdf_object *o) {
    assert(o->type == VDF_TYPE_STRING);

    return o->data.data_string.str;
}

int64_t vdf_object_get_int(const struct vdf_object *o) {
    assert(o->type == VDF_TYPE_INT);

    return o->data.data_int;
}

void vdf_free_object(struct vdf_object *o) {
    if (!o)
        return;

    switch (o->type) {
        case VDF_TYPE_ARRAY:
            for (size_t i = 0; i <= o->data.data_array.len; ++i) {
                vdf_free_object(o->data.data_array.data_value[i]);
            }
            LocalFree(o->data.data_array.data_value);
            break;


        case VDF_TYPE_STRING:
            if (o->data.data_string.str)
                LocalFree(o->data.data_string.str);
            break;

        default:
        case VDF_TYPE_NONE:
            break;

    }

    if (o->key)
        LocalFree(o->key);

    if (o->conditional)
        LocalFree(o->conditional);
    LocalFree(o);
}
