#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "test_common.h"
#include "steam/vdf.h"

/* The parser is compiled into this test with tracking allocators: every
 * block must be freed exactly once, reading a slot past the end of an entry
 * array yields `poison_entry`, and allocations can be failed on demand. */
#define TRACK_CAPACITY 4096
#define SLACK_BYTES (4 * sizeof(void *))
#define NO_FAILURE SIZE_MAX

typedef struct tracked_block {
    void *pointer;
    size_t size;
} tracked_block_t;

static tracked_block_t tracked[TRACK_CAPACITY];
static size_t tracked_count;
static size_t allocation_attempts;
static size_t fail_attempt = NO_FAILURE;
static unsigned invalid_frees;
static struct vdf_object poison_entry;

static void fill_slack(unsigned char *block, size_t size) {
    struct vdf_object *poison = &poison_entry;
    if (size % sizeof(void *) == 0) {
        for (size_t offset = 0; offset < SLACK_BYTES; offset += sizeof(void *))
            memcpy(block + size + offset, &poison, sizeof(poison));
    } else {
        memset(block + size, 0xfd, SLACK_BYTES);
    }
}

static size_t find_tracked(const void *pointer) {
    for (size_t i = 0; i < tracked_count; i++)
        if (tracked[i].pointer == pointer) return i;
    return SIZE_MAX;
}

static void *test_malloc(size_t size) {
    unsigned char *block;
    if (allocation_attempts++ == fail_attempt || tracked_count == TRACK_CAPACITY) return NULL;
    block = HeapAlloc(GetProcessHeap(), 0, size + SLACK_BYTES);
    if (block == NULL) return NULL;
    fill_slack(block, size);
    tracked[tracked_count].pointer = block;
    tracked[tracked_count].size = size;
    tracked_count++;
    return block;
}

static void *test_calloc(size_t count, size_t size) {
    void *block = count != 0 && size > SIZE_MAX / count ? NULL : test_malloc(count * size);
    if (block != NULL) memset(block, 0, count * size);
    return block;
}

static void *test_realloc(void *memory, size_t size) {
    size_t index;
    unsigned char *block;
    if (memory == NULL) return test_malloc(size);
    index = find_tracked(memory);
    if (index == SIZE_MAX) {
        invalid_frees++;
        return NULL;
    }
    if (allocation_attempts++ == fail_attempt) return NULL;
    block = HeapReAlloc(GetProcessHeap(), 0, memory, size + SLACK_BYTES);
    if (block == NULL) return NULL;
    fill_slack(block, size);
    tracked[index].pointer = block;
    tracked[index].size = size;
    return block;
}

static void test_free(void *memory) {
    size_t index;
    if (memory == NULL) return;
    index = find_tracked(memory);
    if (index == SIZE_MAX) {
        invalid_frees++;
        return;
    }
    HeapFree(GetProcessHeap(), 0, memory);
    tracked[index] = tracked[--tracked_count];
}

/* <stdlib.h> is already included, so these only rename the parser's calls. */
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#include "../src/steam/vdf.c"
#undef free
#undef realloc
#undef calloc
#undef malloc

static struct vdf_object *parse(const char *text) {
    return vdf_parse_buffer(text, strlen(text));
}

/* Frees the tree and verifies that nothing leaked or was freed twice. */
static int free_clean(struct vdf_object *object) {
    vdf_free_object(object);
    EXPECT_EQ(tracked_count, 0);
    EXPECT_EQ(invalid_frees, 0);
    return 0;
}

static int expect_rejected(const char *text) {
    EXPECT_NULL(parse(text));
    return free_clean(NULL);
}

static const char *string_value(const struct vdf_object *array, const char *key) {
    const struct vdf_object *entry = vdf_object_index_array_str(array, key);
    return entry != NULL && entry->type == VDF_TYPE_STRING ? vdf_object_get_string(entry) : NULL;
}

static int expect_string(const struct vdf_object *array, const char *key, const char *expected) {
    const char *value = string_value(array, key);
    EXPECT_NOT_NULL(value);
    EXPECT_STREQ_A(value, expected);
    return 0;
}

static int test_well_formed(void) {
    struct vdf_object *root = parse(
        "\"libraryfolders\"\n{\n"
        "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Steam\"\n"
        "\t\t\"apps\"\n\t\t{\n\t\t\t\"374320\"\t\t\"1\"\n\t\t\t\"1245620\"\t\t\"2\"\n\t\t}\n\t}\n"
        "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:/Games\"\n\t}\n"
        "\t\"contentstatsid\"\t\t\"-123\"\n}\n");
    const struct vdf_object *library;
    const struct vdf_object *apps;
    EXPECT_NOT_NULL(root);
    EXPECT_STREQ_A(root->key, "libraryfolders");
    EXPECT_EQ(root->type, VDF_TYPE_ARRAY);
    EXPECT_EQ(vdf_object_get_array_length(root), 3);
    library = vdf_object_index_array(root, 0);
    EXPECT_STREQ_A(library->key, "0");
    EXPECT_EQ(library->type, VDF_TYPE_ARRAY);
    EXPECT_EQ(vdf_object_get_array_length(library), 2);
    EXPECT_EQ(expect_string(library, "path", "C:\\Steam"), 0);
    apps = vdf_object_index_array_str(library, "apps");
    EXPECT_NOT_NULL(apps);
    EXPECT_EQ(apps->type, VDF_TYPE_ARRAY);
    EXPECT_EQ(vdf_object_get_array_length(apps), 2);
    EXPECT_EQ(expect_string(apps, "1245620", "2"), 0);
    library = vdf_object_index_array(root, 1);
    EXPECT_STREQ_A(library->key, "1");
    EXPECT_EQ(vdf_object_get_array_length(library), 1);
    EXPECT_EQ(expect_string(library, "path", "D:/Games"), 0);
    EXPECT_EQ(expect_string(root, "contentstatsid", "-123"), 0);
    /* KeyValues::FindKey ignores ASCII case. */
    EXPECT_EQ(expect_string(root, "ContentStatsID", "-123"), 0);
    EXPECT_NULL(vdf_object_index_array(root, 3));
    EXPECT_EQ(free_clean(root), 0);

    /* Deep nesting closes back to the right level after every block. */
    root = parse("\"a\" { \"b\" { \"c\" { \"d\" { \"e\" \"1\" } } \"f\" \"2\" } \"g\" { } \"h\" \"3\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 3);
    library = vdf_object_index_array_str(root, "b");
    EXPECT_NOT_NULL(library);
    EXPECT_EQ(vdf_object_get_array_length(library), 2);
    EXPECT_EQ(expect_string(library, "f", "2"), 0);
    library = vdf_object_index_array_str(library, "c");
    EXPECT_NOT_NULL(library);
    library = vdf_object_index_array_str(library, "d");
    EXPECT_NOT_NULL(library);
    EXPECT_EQ(expect_string(library, "e", "1"), 0);
    library = vdf_object_index_array_str(root, "g");
    EXPECT_NOT_NULL(library);
    EXPECT_EQ(library->type, VDF_TYPE_ARRAY);
    EXPECT_EQ(vdf_object_get_array_length(library), 0);
    EXPECT_NULL(vdf_object_get_string(library));
    EXPECT_EQ(expect_string(root, "h", "3"), 0);
    EXPECT_EQ(free_clean(root), 0);

    /* Duplicate keys are kept in order and the first one is found. */
    root = parse("\"r\" { \"k\" \"1\" \"K\" \"2\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 2);
    EXPECT_EQ(expect_string(root, "k", "1"), 0);
    return free_clean(root);
}

static int test_escapes(void) {
    /* An escaped backslash right before the closing quote ends the string. */
    struct vdf_object *root = parse("\"root\" { \"path\" \"D:\\\\\" \"next\" \"x\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 2);
    EXPECT_EQ(expect_string(root, "path", "D:\\"), 0);
    EXPECT_EQ(expect_string(root, "next", "x"), 0);
    EXPECT_EQ(free_clean(root), 0);

    root = parse("\"root\" { \"quote\" \"a\\\"b\" \"mixed\" \"x\\\\\\\"y\" \"lines\" \"1\\n2\\t3\" "
                 "\"key \\\"q\\\"\" \"\\\\\" \"rare\" \"\\v\\b\\r\\f\\a\\?\\'\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 5);
    EXPECT_EQ(expect_string(root, "quote", "a\"b"), 0);
    EXPECT_EQ(expect_string(root, "mixed", "x\\\"y"), 0);
    EXPECT_EQ(expect_string(root, "lines", "1\n2\t3"), 0);
    EXPECT_EQ(expect_string(root, "key \"q\"", "\\"), 0);
    EXPECT_EQ(expect_string(root, "rare", "\v\b\r\f\a?'"), 0);
    EXPECT_EQ(free_clean(root), 0);

    /* An unknown escape becomes a NUL, which ends the string there. */
    root = parse("\"root\" { \"dir\" \"C:\\Program Files\" \"next\" \"x\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 2);
    EXPECT_EQ(expect_string(root, "dir", "C:"), 0);
    EXPECT_EQ(expect_string(root, "next", "x"), 0);
    EXPECT_EQ(free_clean(root), 0);

    /* Outside a string a backslash is an ordinary unquoted token. */
    root = parse("\"root\" { \"a\" \\x }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(expect_string(root, "a", "\\x"), 0);
    EXPECT_EQ(free_clean(root), 0);
    /* An unterminated string reaches the end of the input. */
    EXPECT_EQ(expect_rejected("\"root\" { \"a\" \\\"b\\\" }"), 0);
    return expect_rejected("\"root\" { \"a\" \"b\\\\");
}

static int test_structure_in_strings(void) {
    struct vdf_object *root = parse(
        "\"root\" { \"name\" \"{weird}\" \"close\" \"}\" \"{open\" \"{\" "
        "\"cond\" \"[$X360]\" \"slash\" \"//not a comment\" \"eq\" \"=\" \"}\" \"key\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 7);
    EXPECT_EQ(expect_string(root, "}", "key"), 0);
    EXPECT_EQ(expect_string(root, "name", "{weird}"), 0);
    EXPECT_EQ(expect_string(root, "close", "}"), 0);
    EXPECT_EQ(expect_string(root, "{open", "{"), 0);
    /* A quoted conditional is only a value. */
    EXPECT_EQ(expect_string(root, "cond", "[$X360]"), 0);
    EXPECT_EQ(expect_string(root, "slash", "//not a comment"), 0);
    EXPECT_EQ(expect_string(root, "eq", "="), 0);
    return free_clean(root);
}

static int test_unquoted_tokens(void) {
    /* Steam accepts unquoted keys and values; Steam's controller templates use
     * them (`"id" 1`). */
    struct vdf_object *root = parse("controller { \"id\" 1 name ELDEN/RING path a/b //x\n }");
    EXPECT_NOT_NULL(root);
    EXPECT_STREQ_A(root->key, "controller");
    EXPECT_EQ(vdf_object_get_array_length(root), 3);
    EXPECT_EQ(expect_string(root, "id", "1"), 0);
    EXPECT_EQ(expect_string(root, "name", "ELDEN/RING"), 0);
    EXPECT_EQ(expect_string(root, "path", "a/b"), 0);
    EXPECT_EQ(free_clean(root), 0);

    /* Quotes and braces end an unquoted token. */
    root = parse("r{a\"b\"c{d e}}");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 2);
    EXPECT_EQ(expect_string(root, "a", "b"), 0);
    EXPECT_EQ(expect_string(vdf_object_index_array_str(root, "c"), "d", "e"), 0);
    EXPECT_EQ(free_clean(root), 0);

    /* "key" = "value" is accepted. */
    root = parse("\"root\" { \"a\" = \"1\" \"b\" = { \"c\" \"2\" } }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 2);
    EXPECT_EQ(expect_string(root, "a", "1"), 0);
    EXPECT_EQ(expect_string(vdf_object_index_array_str(root, "b"), "c", "2"), 0);
    return free_clean(root);
}

static int test_rejected_structure(void) {
    /* Truncation anywhere inside a block. */
    EXPECT_EQ(expect_rejected("\"root\" { \"done\" \"1\" \"a\" { \"b\" { \"c\" \"d\""), 0);
    EXPECT_EQ(expect_rejected("\"root\" { \"a\" { \"b\" { \"c\" { \"d\" { "), 0);
    EXPECT_EQ(expect_rejected("\"root\" { \"a\""), 0);
    EXPECT_EQ(expect_rejected("\"root\""), 0);
    /* A key whose value is a closing brace. */
    EXPECT_EQ(expect_rejected("\"root\" { \"a\" { \"b\" { \"c\" \"d\" } ! } }"), 0);
    EXPECT_EQ(expect_rejected("\"root\" { \"a\" { \"b\" { [$X] } } }"), 0);
    EXPECT_EQ(expect_rejected("\"root\" { [$WIN32] }"), 0);
    /* Empty key names inside a block. */
    EXPECT_EQ(expect_rejected("\"root\" { \"\" \"x\" }"), 0);
    {
        static const char nul_in_block[] = "\"root\" { \"a\" \"1\" \0 }";
        EXPECT_NULL(vdf_parse_buffer(nul_in_block, sizeof(nul_in_block) - 1));
        EXPECT_EQ(free_clean(NULL), 0);
    }
    /* A top-level key must open a block. */
    EXPECT_EQ(expect_rejected("}"), 0);
    EXPECT_EQ(expect_rejected("\"key\" \"value\""), 0);
    return expect_rejected("[$WIN32] \"root\" { }");
}

static int test_top_level_entries(void) {
    /* Install scripts append a "kvsignatures" entry: Steam parses every entry
     * and hands its caller the first one. */
    struct vdf_object *root = parse(
        "\"InstallScript\" { \"Registry\" { \"k\" \"v\" } } "
        "\"kvsignatures\" { \"InstallScript\" \"abc\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_STREQ_A(root->key, "InstallScript");
    EXPECT_EQ(vdf_object_get_array_length(root), 1);
    EXPECT_NOT_NULL(vdf_object_index_array_str(root, "Registry"));
    EXPECT_NULL(vdf_object_index_array_str(root, "kvsignatures"));
    EXPECT_EQ(free_clean(root), 0);

    /* Any later entry that does not parse rejects the whole input. */
    EXPECT_EQ(expect_rejected("\"root\" { \"a\" { } } } ! \\ { \"x\" \"y\""), 0);
    EXPECT_EQ(expect_rejected("\"root\" { } trailing"), 0);
    EXPECT_EQ(expect_rejected("\"a\" { } \"b\" { \"x\" }"), 0);
    EXPECT_EQ(expect_rejected("\"key\" \"value\" \"other\" { }"), 0);

    /* Comments and whitespace may follow; a NUL ends the input. */
    root = parse("\"root\" { \"a\" \"1\" }\r\n// done\r\n");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(free_clean(root), 0);
    {
        static const char nul_after[] = "\"root\" { \"a\" \"1\" }\0garbage {";
        root = vdf_parse_buffer(nul_after, sizeof(nul_after) - 1);
    }
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(expect_string(root, "a", "1"), 0);
    EXPECT_EQ(free_clean(root), 0);

    /* Input without any entry parses to an object without a key. */
    root = parse(" \r\n\t// nothing\n");
    EXPECT_NOT_NULL(root);
    EXPECT_NULL(root->key);
    EXPECT_EQ(vdf_object_get_array_length(root), 0);
    EXPECT_EQ(free_clean(root), 0);
    /* An empty quoted name is still a top-level key. */
    root = parse("\"\" { \"a\" \"1\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_STREQ_A(root->key, "");
    EXPECT_EQ(free_clean(root), 0);

    /* #include is ignored, but must name a file. */
    root = parse("#include \"other.vdf\" #INCLUDE x \"root\" { \"a\" \"1\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_STREQ_A(root->key, "root");
    EXPECT_EQ(free_clean(root), 0);
    EXPECT_EQ(expect_rejected("\"root\" { } #include"), 0);

    /* A UTF-8 BOM is skipped. */
    root = parse("\xEF\xBB\xBF\"root\" { \"a\" \"1\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_STREQ_A(root->key, "root");
    return free_clean(root);
}

static int test_conditionals(void) {
    struct vdf_object *root = parse(
        "\"root\" { \"a\" \"1\" [$WIN32] \"b\" \"2\" [$X360] \"c\" { \"d\" \"3\" } "
        "\"e\" [$X360] { } \"f\" [!$X360] \"4\" \"g\" \"5\" [$win64] }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 4);
    EXPECT_EQ(expect_string(root, "a", "1"), 0);
    EXPECT_NULL(vdf_object_index_array_str(root, "b"));
    EXPECT_NOT_NULL(vdf_object_index_array_str(root, "c"));
    EXPECT_NULL(vdf_object_index_array_str(root, "e"));
    EXPECT_EQ(expect_string(root, "f", "4"), 0);
    EXPECT_EQ(expect_string(root, "g", "5"), 0);
    EXPECT_EQ(free_clean(root), 0);

    /* Expressions: && and || from left to right, ! of the next factor,
     * parentheses, constants, and $0 and $1. The second character of an
     * operator only needs the bits of the first. */
    root = parse(
        "\"r\" { \"t1\" \"x\" [$WIN32||$OSX] \"f1\" \"x\" [$WIN32&&$OSX] \"f2\" \"x\" [!$WIN32] "
        "\"t2\" \"x\" [($OSX||$WINDOWS)&&!$X360] \"t3\" \"x\" [$1] \"f3\" \"x\" [$0] "
        "\"t4\" \"x\" [7] \"f4\" \"x\" [00] \"f5\" \"x\" [$UNKNOWN] \"t5\" \"x\" [$WIN32|$OSX] "
        "\"f6\" \"x\" [$WIN32!$WIN32] \"t6\" \"x\" [$VISTA&&$WIN7] \"f7\" \"x\" [$LINUX||$POSIX] "
        "\"f8\" \"x\" [$WIN32&o$X360] \"t7\" \"x\" [$X360|~$WIN32] }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 7);
    for (size_t i = 0; i < vdf_object_get_array_length(root); i++)
        EXPECT_TRUE(vdf_object_index_array(root, i)->key[0] == 't');
    EXPECT_EQ(free_clean(root), 0);

    /* A kept key with a trailing conditional replaces the first earlier key of
     * the same name; a key before = can do the same. */
    root = parse("\"r\" { \"k\" \"old\" \"k\" \"older\" \"K\" \"new\" [$WIN32] "
                 "\"j\" \"1\" \"j\" = [$WIN32] \"2\" \"n\" = [$X360] \"3\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 3);
    EXPECT_EQ(expect_string(root, "k", "older"), 0);
    EXPECT_STREQ_A(vdf_object_index_array(root, 1)->key, "K");
    EXPECT_EQ(expect_string(root, "j", "2"), 0);
    EXPECT_NULL(vdf_object_index_array_str(root, "n"));
    EXPECT_EQ(free_clean(root), 0);

    /* A rejected top-level entry is cleared and replaced by the next one. */
    root = parse("\"skip\" [$X360] { \"x\" \"1\" } \"keep\" [$WINDOWS] { \"y\" \"2\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_STREQ_A(root->key, "keep");
    EXPECT_EQ(vdf_object_get_array_length(root), 1);
    EXPECT_EQ(expect_string(root, "y", "2"), 0);
    EXPECT_EQ(free_clean(root), 0);

    /* Two conditionals in a row put one between the key and its value. */
    return expect_rejected("\"root\" { \"k\" [$WIN32] [$WIN32] }");
}

static int test_integers(void) {
    struct vdf_object *root = parse(
        "\"AppState\" { \"appid\" \"1245620\" \"installdir\" \"12345\" \"empty\" \"\" "
        "\"size\" \"18446744073709551615\" \"space\" \" 12\" \"zeros\" \"007\" \"neg0\" \"-0\" "
        "\"plus\" \"+5\" \"big\" \"2147483648\" \"min\" \"-2147483648\" \"frac\" \"1.5\" "
        "\"zero\" \"0\" \"zz\" \"00\" \"comma\" \"1,2\" \"trail\" \"12 \" \"tab\" \"\t1\" }");
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 16);
    for (size_t i = 0; i < vdf_object_get_array_length(root); i++)
        EXPECT_EQ(vdf_object_index_array(root, i)->type, VDF_TYPE_STRING);
    EXPECT_EQ(expect_string(root, "appid", "1245620"), 0);
    EXPECT_EQ(expect_string(root, "installdir", "12345"), 0);
    EXPECT_EQ(expect_string(root, "empty", ""), 0);
    EXPECT_EQ(expect_string(root, "size", "18446744073709551615"), 0);
    /* GetString formats integers again. */
    EXPECT_EQ(expect_string(root, "space", "12"), 0);
    EXPECT_EQ(expect_string(root, "zeros", "007"), 0);
    EXPECT_EQ(expect_string(root, "neg0", "0"), 0);
    EXPECT_EQ(expect_string(root, "plus", "5"), 0);
    EXPECT_EQ(expect_string(root, "big", "2147483648"), 0);
    EXPECT_EQ(expect_string(root, "min", "-2147483648"), 0);
    EXPECT_EQ(expect_string(root, "frac", "1.5"), 0);
    EXPECT_EQ(expect_string(root, "zero", "0"), 0);
    EXPECT_EQ(expect_string(root, "zz", "00"), 0);
    EXPECT_EQ(expect_string(root, "comma", "1,2"), 0);
    EXPECT_EQ(expect_string(root, "trail", "12 "), 0);
    EXPECT_EQ(expect_string(root, "tab", "\t1"), 0);
    return free_clean(root);
}

static int test_token_limits(void) {
    const size_t limit = VDF_TOKEN_SIZE - 1;
    const size_t text_size = 2 * VDF_TOKEN_SIZE + 64;
    char *text = malloc(text_size);
    struct vdf_object *root;
    size_t length;
    EXPECT_NOT_NULL(text);
    /* A quoted value is cut at the token buffer, an unquoted one is an error. */
    length = (size_t)snprintf(text, text_size, "\"r\" { \"long\" \"");
    memset(text + length, 'q', limit + 10);
    length += limit + 10;
    length += (size_t)snprintf(text + length, text_size - length, "\" }");
    root = vdf_parse_buffer(text, length);
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(strlen(string_value(root, "long")), limit);
    EXPECT_EQ(free_clean(root), 0);

    length = (size_t)snprintf(text, text_size, "\"r\" { \"long\" ");
    memset(text + length, 'u', limit);
    length += limit;
    length += (size_t)snprintf(text + length, text_size - length, " }");
    root = vdf_parse_buffer(text, length);
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(strlen(string_value(root, "long")), limit);
    EXPECT_EQ(free_clean(root), 0);
    text[length - 2] = 'u';
    EXPECT_NULL(vdf_parse_buffer(text, length));
    free(text);
    return free_clean(NULL);
}

static int test_crlf_and_files(void) {
    const char *text =
        "// Steam library index\r\n"
        "\"libraryfolders\"\r\n{\r\n"
        "\t\"0\"\t// first library\r\n\t{\r\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\r\n\t}\r\n"
        "}\r\n// trailing comment without a newline";
    wchar_t temp_dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    FILE *file;
    struct vdf_object *root = parse(text);
    const struct vdf_object *library;
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(vdf_object_get_array_length(root), 1);
    library = vdf_object_index_array_str(root, "0");
    EXPECT_NOT_NULL(library);
    EXPECT_EQ(expect_string(library, "path", "C:\\Program Files (x86)\\Steam"), 0);
    EXPECT_EQ(free_clean(root), 0);

    /* Files are read in binary mode, so CRLF reaches the parser unchanged. */
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp_dir) != 0);
    EXPECT_TRUE(GetTempFileNameW(temp_dir, L"vdf", 0, path) != 0);
    /* Steam does not parse an empty file. */
    EXPECT_NULL(vdf_parse_file(path));
    file = _wfopen(path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_EQ(fwrite(text, 1, strlen(text), file), strlen(text));
    EXPECT_EQ(fclose(file), 0);
    root = vdf_parse_file(path);
    EXPECT_TRUE(DeleteFileW(path));
    EXPECT_NOT_NULL(root);
    library = vdf_object_index_array_str(root, "0");
    EXPECT_NOT_NULL(library);
    EXPECT_EQ(expect_string(library, "path", "C:\\Program Files (x86)\\Steam"), 0);
    EXPECT_EQ(free_clean(root), 0);
    EXPECT_NULL(vdf_parse_file(path));
    return free_clean(NULL);
}

static int test_allocation_failures(void) {
    const char *text = "\"root\" { \"a\" \"1\" [$WIN32] \"b\" { \"c\" { \"d\" \"2\" } \"e\" \"3\" [$X360] } "
                       "\"f\" \"4\" \"f\" \"5\" [$WIN32] } \"second\" { \"x\" \"y\" }";
    struct vdf_object *root;
    size_t attempts;
    allocation_attempts = 0;
    root = parse(text);
    attempts = allocation_attempts;
    EXPECT_NOT_NULL(root);
    EXPECT_EQ(free_clean(root), 0);
    EXPECT_TRUE(attempts > 10);
    /* Every allocation failure rejects the input and frees what was built. */
    for (size_t i = 0; i < attempts; i++) {
        allocation_attempts = 0;
        fail_attempt = i;
        root = parse(text);
        fail_attempt = NO_FAILURE;
        if (root != NULL) {
            fprintf(stderr, "allocation %zu did not fail the parse\n", i);
            vdf_free_object(root);
            return 1;
        }
        EXPECT_EQ(free_clean(NULL), 0);
    }
    return 0;
}

int main(void) {
    EXPECT_EQ(test_well_formed(), 0);
    EXPECT_EQ(test_escapes(), 0);
    EXPECT_EQ(test_structure_in_strings(), 0);
    EXPECT_EQ(test_unquoted_tokens(), 0);
    EXPECT_EQ(test_rejected_structure(), 0);
    EXPECT_EQ(test_top_level_entries(), 0);
    EXPECT_EQ(test_conditionals(), 0);
    EXPECT_EQ(test_integers(), 0);
    EXPECT_EQ(test_token_limits(), 0);
    EXPECT_EQ(test_crlf_and_files(), 0);
    EXPECT_EQ(test_allocation_failures(), 0);
    printf("smoke_vdf: all tests passed\n");
    return 0;
}
