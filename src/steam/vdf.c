/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

/*
 * Valve text KeyValues (VDF) reader that accepts, rejects and interprets input
 * like the Steam client reading libraryfolders.vdf and appmanifest_*.acf. Those
 * readers call KeyValues::LoadFromBuffer with escape sequences enabled; the
 * rules below follow that implementation in steamclient64.dll (ReadToken,
 * RecursiveLoadFromBuffer, the conditional expression evaluator, and the
 * CUtlBuffer text helpers):
 *
 * - A UTF-8 BOM is skipped. Whitespace is what isspace() accepts in the "C"
 *   locale, and "//" starts a comment that runs to the end of the line.
 * - Quoted strings decode \n \t \v \b \r \f \a \\ \? \' \"; a backslash before
 *   any other character becomes a NUL, which ends the string. A string longer
 *   than the token buffer is truncated; an unquoted token that long is an error.
 * - {, } and = are tokens of their own. Unquoted tokens end at whitespace, a
 *   quote, one of those tokens, or a NUL.
 * - Every top-level entry must be "key" { ... }. The caller receives the first
 *   one; later entries only have to parse. A top-level #include and the file
 *   name after it are skipped, and #include without a file name is an error.
 * - An unquoted token containing [ and then ] is a conditional. Before a value
 *   or block it decides whether the key is kept; after a value it decides that
 *   too, and a kept key then replaces the first earlier key of the same name.
 * - A value made of " +,-.0123456789" that strtol consumes completely, does not
 *   overflow, and starts with a nonzero character (or is exactly "0") is an
 *   integer, which GetString formats back in decimal.
 * - Key lookup ignores ASCII case and returns the first match.
 * - Any error Steam reports, including an unterminated block or text after the
 *   last entry, rejects the whole input.
 *
 * Where Steam's behavior is undefined, this reader stays deterministic: nesting
 * beyond VDF_MAX_DEPTH (Steam overflows its stack) rejects the input, and a
 * malformed conditional reads NULs after the end of its token instead of stale
 * buffer contents. Conditional symbols registered at run time by the Steam
 * client are unknown here and evaluate to false.
 */

#include "vdf.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Steam client's KeyValues token buffer. */
#define VDF_TOKEN_SIZE 0x40000
#define VDF_MAX_DEPTH 4096
/* CExpressionEvaluator's identifier buffer. */
#define VDF_IDENTIFIER_SIZE 128

typedef struct vdf_parser {
    const unsigned char *data;
    int size;
    int get;
    /* CUtlBuffer's GET_OVERFLOW: a read went past the end. */
    bool overflow;
    /* Steam reported an error, or memory ran out. */
    bool error;
    char *token;
} vdf_parser_t;

static bool is_space(int c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static bool ascii_equal(const char *left, const char *right) {
    for (;; left++, right++) {
        char l = *left >= 'a' && *left <= 'z' ? (char)(*left - 0x20) : *left;
        char r = *right >= 'a' && *right <= 'z' ? (char)(*right - 0x20) : *right;
        if (l != r) return false;
        if (l == '\0') return true;
    }
}

static char *copy_string(const char *s) {
    const size_t length = strlen(s);
    char *copy = malloc(length + 1);
    if (copy != NULL) memcpy(copy, s, length + 1);
    return copy;
}

/* --- CUtlBuffer text helpers ------------------------------------------- */

static void eat_white_space(vdf_parser_t *p) {
    while (!p->overflow) {
        if (p->get >= p->size) {
            p->overflow = true;
            return;
        }
        if (!is_space(p->data[p->get])) return;
        p->get++;
    }
}

static int get_char(vdf_parser_t *p) {
    if (p->overflow) return 0;
    if (p->get >= p->size) {
        p->overflow = true;
        return 0;
    }
    return p->data[p->get++];
}

static bool eat_cpp_comment(vdf_parser_t *p) {
    if (p->overflow || p->size - p->get < 2 ||
        p->data[p->get] != '/' || p->data[p->get + 1] != '/') return false;
    p->get += 2;
    while (get_char(p) != '\n' && !p->overflow) {}
    return true;
}

static char escaped_char(int c) {
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'v': return '\v';
        case 'b': return '\b';
        case 'r': return '\r';
        case 'f': return '\f';
        case 'a': return '\a';
        case '\\': return '\\';
        case '?': return '?';
        case '\'': return '\'';
        case '"': return '"';
        default: return '\0';
    }
}

static char get_delimited_char(vdf_parser_t *p) {
    const int c = get_char(p);
    char converted;
    if (c != '\\') return (char)c;
    if (p->get >= p->size) return '\0';
    /* An unknown escape becomes a NUL; the next character is read normally. */
    converted = escaped_char(p->data[p->get]);
    if (converted != '\0') p->get++;
    return converted;
}

/* GetDelimitedString after the opening quote has been peeked. */
static void read_quoted(vdf_parser_t *p) {
    int length = 0;
    p->get++;
    while (!p->overflow) {
        char c;
        if (p->get < p->size && p->data[p->get] == '"') {
            p->get++;
            break;
        }
        c = get_delimited_char(p);
        if (length < VDF_TOKEN_SIZE) p->token[length++] = c;
    }
    if (length >= VDF_TOKEN_SIZE) length = VDF_TOKEN_SIZE - 1;
    p->token[length] = '\0';
}

/* KeyValues::ReadToken with escape sequences enabled. */
static const char *read_token(vdf_parser_t *p, bool *quoted, bool *conditional) {
    bool bracket = false;
    bool reported = false;
    int length = 0;
    *quoted = false;
    *conditional = false;
    if (p->overflow) return NULL;
    eat_white_space(p);
    if (p->overflow) return NULL;
    while (eat_cpp_comment(p)) {
        eat_white_space(p);
        if (p->overflow) return NULL;
    }
    if (p->get >= p->size) return NULL;
    switch (p->data[p->get]) {
        case '"':
            *quoted = true;
            read_quoted(p);
            return p->token;
        case '{':
        case '}':
        case '=':
            p->token[0] = (char)p->data[p->get++];
            p->token[1] = '\0';
            return p->token;
        default:
            break;
    }
    while (p->get < p->size) {
        const unsigned char c = p->data[p->get];
        if (c == '\0' || c == '"' || c == '=' || c == '{' || c == '}') break;
        if (c == '[') bracket = true;
        else if (c == ']' && bracket) *conditional = true;
        if (is_space(c)) break;
        if (length < VDF_TOKEN_SIZE - 1) {
            p->token[length++] = (char)c;
        } else if (!reported) {
            reported = true;
            p->error = true;
        }
        p->get++;
    }
    p->token[length] = '\0';
    return p->token;
}

/* --- Conditionals (CExpressionEvaluator) -------------------------------- */

typedef struct vdf_expression {
    const char *text;
    size_t length;
    size_t position;
    char current;
    int depth;
} vdf_expression_t;

static char expression_at(const vdf_expression_t *e, size_t position) {
    return position < e->length ? e->text[position] : '\0';
}

static void next_token(vdf_expression_t *e) {
    while (expression_at(e, e->position) == ' ') e->position++;
    e->current = expression_at(e, e->position++);
}

/* The Steam client's symbol callback on 64-bit Windows Vista or later. */
static bool evaluate_symbol(const char *identifier) {
    static const char *const true_symbols[] = { "WINDOWS", "WIN32", "WIN64", "VISTA", "WIN7" };
    const char *name = identifier[0] == '$' ? identifier + 1 : identifier;
    if (ascii_equal(identifier, "$0")) return false;
    if (ascii_equal(identifier, "$1")) return true;
    for (size_t i = 0; i < sizeof(true_symbols) / sizeof(true_symbols[0]); i++) {
        if (ascii_equal(name, true_symbols[i])) return true;
    }
    return false;
}

static bool is_ascii_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool make_expression(vdf_expression_t *e);

/* Each make_* function returns the value of the subtree it builds; a missing
 * subtree is false, as in CExpressionEvaluator::SimplifyNode. */
static bool make_factor(vdf_expression_t *e) {
    char identifier[VDF_IDENTIFIER_SIZE];
    int length = 1;
    bool value;
    if (e->current == '(') {
        next_token(e);
        value = false;
        if (e->depth < VDF_MAX_DEPTH) {
            e->depth++;
            value = make_expression(e);
            e->depth--;
        }
        /* The closing parenthesis is consumed without being checked. */
        next_token(e);
        return value;
    }
    if (e->current == '$') {
        identifier[0] = '$';
        while ((is_ascii_alnum(expression_at(e, e->position)) || expression_at(e, e->position) == '_') &&
               length < VDF_IDENTIFIER_SIZE) {
            identifier[length++] = expression_at(e, e->position++);
        }
    } else if (e->current >= '0' && e->current <= '9') {
        identifier[0] = e->current;
        while (expression_at(e, e->position) >= '0' && expression_at(e, e->position) <= '9' &&
               length < VDF_IDENTIFIER_SIZE) {
            identifier[length++] = expression_at(e, e->position++);
        }
    } else if (e->current == '!') {
        /* MakeTerm builds the NOT node. */
        return false;
    } else {
        /* A bad token is skipped. */
        next_token(e);
        return false;
    }
    if (length >= VDF_IDENTIFIER_SIZE - 1) {
        next_token(e);
        return false;
    }
    identifier[length] = '\0';
    if (identifier[0] == '$') {
        value = evaluate_symbol(identifier);
    } else {
        /* atoi(...) != 0 for a run of digits. */
        value = strspn(identifier, "0") != (size_t)length;
    }
    next_token(e);
    return value;
}

/* <factor> { ! <factor> }: a NOT node ignores what precedes it. */
static bool make_term(vdf_expression_t *e) {
    bool value = make_factor(e);
    while (e->current == '!') {
        next_token(e);
        value = !make_factor(e);
    }
    return value;
}

/* <term> { <&& or ||> <term> }, left to right without precedence. */
static bool make_expression(vdf_expression_t *e) {
    bool value = make_term(e);
    while (e->current == '|' || e->current == '&') {
        const char op = e->current;
        const char next = expression_at(e, e->position++);
        bool right;
        if ((next & op) != op) break;
        next_token(e);
        right = make_term(e);
        value = op == '&' ? value && right : value || right;
    }
    return value;
}

/* EvaluateConditional on a conditional token, skipping its first character. */
static bool evaluate_conditional(const char *token) {
    vdf_expression_t e;
    if (token[0] == '\0') return false;
    e.text = token + 1;
    e.length = strlen(e.text);
    e.position = 0;
    e.depth = 0;
    next_token(&e);
    return make_expression(&e);
}

/* --- Tree --------------------------------------------------------------- */

static struct vdf_object *new_object(vdf_parser_t *p, const char *key) {
    struct vdf_object *object = calloc(1, sizeof(*object));
    if (object != NULL && key != NULL) {
        object->key = copy_string(key);
        if (object->key == NULL) {
            free(object);
            object = NULL;
        }
    }
    if (object == NULL) p->error = true;
    return object;
}

static void clear_object(struct vdf_object *o) {
    if (o->type == VDF_TYPE_ARRAY) {
        for (size_t i = 0; i < o->data.data_array.len; i++) {
            vdf_free_object(o->data.data_array.data_value[i]);
        }
        free(o->data.data_array.data_value);
    } else if (o->type == VDF_TYPE_STRING) {
        free(o->data.data_string.str);
    }
    o->type = VDF_TYPE_NONE;
    memset(&o->data, 0, sizeof(o->data));
}

static bool set_key(vdf_parser_t *p, struct vdf_object *o, const char *key) {
    char *copy = copy_string(key);
    if (copy == NULL) {
        p->error = true;
        return false;
    }
    free(o->key);
    o->key = copy;
    return true;
}

static bool append_child(vdf_parser_t *p, struct vdf_object *array, struct vdf_object *child) {
    const size_t length = array->data.data_array.len;
    struct vdf_object **grown = realloc(array->data.data_array.data_value, (length + 1) * sizeof(*grown));
    if (grown == NULL) {
        p->error = true;
        return false;
    }
    grown[length] = child;
    array->data.data_array.data_value = grown;
    array->data.data_array.len = length + 1;
    return true;
}

/* The first child of the same name gives way to a new key whose trailing
 * conditional is true. */
static void remove_first_named(struct vdf_object *array, const char *key) {
    struct vdf_object **children = array->data.data_array.data_value;
    const size_t length = array->data.data_array.len;
    for (size_t i = 0; i < length; i++) {
        if (ascii_equal(children[i]->key, key)) {
            vdf_free_object(children[i]);
            memmove(children + i, children + i + 1, (length - i - 1) * sizeof(*children));
            array->data.data_array.len = length - 1;
            return;
        }
    }
}

/* KeyValues::SetString, or SetInt followed by GetString's decimal formatting. */
static bool set_value(vdf_parser_t *p, struct vdf_object *o, const char *value) {
    const size_t length = strlen(value);
    char formatted[16];
    const char *stored = value;
    if (length != 0 && strspn(value, " +,-.0123456789") == length) {
        char *end;
        long number;
        bool overflow;
        errno = 0;
        number = strtol(value, &end, 10);
        overflow = (number == LONG_MAX || number == LONG_MIN) && errno == ERANGE;
        if ((end == value + length && value[0] != '0' && !overflow) ||
            (value[0] == '0' && value[1] == '\0')) {
            snprintf(formatted, sizeof(formatted), "%ld", number);
            stored = formatted;
        }
    }
    o->data.data_string.str = copy_string(stored);
    if (o->data.data_string.str == NULL) {
        p->error = true;
        return false;
    }
    o->type = VDF_TYPE_STRING;
    return true;
}

/* --- KeyValues::RecursiveLoadFromBuffer ----------------------------------- */

static void load_block(vdf_parser_t *p, struct vdf_object *object, int depth) {
    bool quoted;
    bool conditional;
    const char *name;
    object->type = VDF_TYPE_ARRAY;
    if (depth > VDF_MAX_DEPTH) {
        p->error = true;
        return;
    }
    for (name = read_token(p, &quoted, &conditional); name != NULL && !p->error;
         name = read_token(p, &quoted, &conditional)) {
        struct vdf_object *child;
        const char *value;
        bool accepted = true;
        if (name[0] == '\0') break; /* "got empty keyname" */
        if (name[0] == '}' && !quoted) return;
        child = new_object(p, name);
        if (child == NULL) return;
        value = read_token(p, &quoted, &conditional);
        if (conditional && value != NULL) {
            accepted = evaluate_conditional(value);
            value = read_token(p, &quoted, &conditional);
        }
        if (value != NULL && value[0] == '=' && !quoted) {
            value = read_token(p, &quoted, &conditional);
            if (conditional) {
                if (value != NULL) {
                    accepted = evaluate_conditional(value);
                    value = read_token(p, &quoted, &conditional);
                }
                if (accepted) remove_first_named(object, child->key);
            }
        }
        /* "got NULL key", "got } in key", or "got conditional between key and value". */
        if (value == NULL || (value[0] == '}' && !quoted) ||
            (!(value[0] == '{' && !quoted) && conditional)) {
            vdf_free_object(child);
            break;
        }
        if (value[0] == '{' && !quoted) {
            load_block(p, child, depth + 1);
        } else if (set_value(p, child, value)) {
            /* Look ahead one token for a trailing conditional. */
            const int saved = p->get;
            bool peek_quoted;
            bool peek_conditional;
            const char *peek = read_token(p, &peek_quoted, &peek_conditional);
            if (peek_conditional) {
                accepted = evaluate_conditional(peek);
                if (accepted) remove_first_named(object, child->key);
            } else {
                p->get = saved;
                p->overflow = false;
            }
        }
        if (p->error || !accepted || !append_child(p, object, child)) {
            vdf_free_object(child);
        }
    }
    /* An empty key, "got EOF instead of keyname", or an error above. */
    p->error = true;
}

/* --- KeyValues::LoadFromBuffer ------------------------------------------ */

struct vdf_object *vdf_parse_buffer(const char *buffer, const size_t size) {
    vdf_parser_t p = { 0 };
    struct vdf_object *root;
    struct vdf_object *current;
    if (buffer == NULL || size > INT_MAX) return NULL;
    p.data = (const unsigned char *)buffer;
    p.size = (int)size;
    p.token = malloc(VDF_TOKEN_SIZE);
    root = p.token == NULL ? NULL : new_object(&p, NULL);
    if (root == NULL) {
        free(p.token);
        return NULL;
    }
    if (p.size >= 3 && memcmp(p.data, "\xEF\xBB\xBF", 3) == 0) p.get = 3;
    current = root;
    do {
        bool quoted;
        bool conditional;
        bool accepted = true;
        const char *s = read_token(&p, &quoted, &conditional);
        if (p.overflow || s == NULL || (s[0] == '\0' && !quoted)) break;
        if (ascii_equal(s, "#include")) {
            /* Steam ignores includes, but reports one without a file name. */
            s = read_token(&p, &quoted, &conditional);
            if (s == NULL || s[0] == '\0') p.error = true;
            continue;
        }
        /* Later entries are parsed into a scratch object and dropped; a
         * rejected entry is cleared and its object reused for the next one. */
        if (current == NULL) {
            current = new_object(&p, s);
            if (current == NULL) break;
        } else if (!set_key(&p, current, s)) {
            break;
        }
        s = read_token(&p, &quoted, &conditional);
        if (conditional) {
            accepted = evaluate_conditional(s);
            s = read_token(&p, &quoted, &conditional);
        }
        if (s != NULL && s[0] == '{' && !quoted) {
            load_block(&p, current, 1);
        } else {
            p.error = true; /* "LoadFromBuffer: missing {" */
        }
        if (!accepted) {
            clear_object(current);
        } else {
            if (current != root) vdf_free_object(current);
            current = NULL;
        }
    } while (!p.overflow && !p.error);
    if (current != NULL && current != root) vdf_free_object(current);
    free(p.token);
    if (p.error) {
        vdf_free_object(root);
        return NULL;
    }
    return root;
}

/* Like the Steam client, an empty or unreadable file is not parsed. */
struct vdf_object *vdf_parse_file(const wchar_t *path) {
    struct vdf_object *object = NULL;
    LARGE_INTEGER size;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= INT_MAX) {
        char *buffer = malloc((size_t)size.QuadPart);
        DWORD read = 0;
        if (buffer != NULL && ReadFile(file, buffer, (DWORD)size.QuadPart, &read, NULL) && read != 0) {
            object = vdf_parse_buffer(buffer, read);
        }
        free(buffer);
    }
    CloseHandle(file);
    return object;
}

size_t vdf_object_get_array_length(const struct vdf_object *o) {
    return o != NULL && o->type == VDF_TYPE_ARRAY ? o->data.data_array.len : 0;
}

struct vdf_object *vdf_object_index_array(const struct vdf_object *o, const size_t index) {
    return index < vdf_object_get_array_length(o) ? o->data.data_array.data_value[index] : NULL;
}

struct vdf_object *vdf_object_index_array_str(const struct vdf_object *o, const char *str) {
    if (str == NULL) return NULL;
    for (size_t i = 0; i < vdf_object_get_array_length(o); i++) {
        struct vdf_object *child = o->data.data_array.data_value[i];
        if (child->key != NULL && ascii_equal(child->key, str)) return child;
    }
    return NULL;
}

const char *vdf_object_get_string(const struct vdf_object *o) {
    return o != NULL && o->type == VDF_TYPE_STRING ? o->data.data_string.str : NULL;
}

void vdf_free_object(struct vdf_object *o) {
    if (o == NULL) return;
    clear_object(o);
    free(o->key);
    free(o);
}
