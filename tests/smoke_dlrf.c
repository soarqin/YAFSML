#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "test_common.h"
#include "process/dlrf.h"

typedef struct fake_runtime_class_s {
    void *vtable;
} fake_runtime_class_t;

typedef struct fake_vector_s {
    void *allocator;
    void *first;
    void *last;
    void *end;
} fake_vector_t;

typedef struct fake_vector_2012_s {
    void *first;
    void *last;
    void *end;
    void *allocator;
} fake_vector_2012_t;

typedef struct fake_invoker_s {
    void *vtable;
    void *address;
} fake_invoker_t;

typedef struct fake_resolver_s {
    void *runtime_class;
    const char *name;
    const wchar_t *name_wide;
    fake_vector_t invokers;
    fake_vector_t parameter_infos;
    void *loose_parameter_info_start;
} fake_resolver_t;

typedef struct fake_method_s {
    fake_resolver_t *resolver;
    const char *name;
    const wchar_t *name_wide;
    size_t name_len;
} fake_method_t;

typedef struct fake_full_class_s {
    void *vtable;
    void *parent;
    void *runtime_ctor;
    fake_vector_t methods;
    const char *name;
    const wchar_t *name_wide;
} fake_full_class_t;

/* Names and vectors that end at, or run into, an inaccessible page. */
static int test_readability_boundaries(void) {
    static const char name[] = "CSAutoControlAPI";
    SYSTEM_INFO system;
    uint8_t *pages;
    size_t page;
    DWORD old_protect;
    fake_full_class_t runtime_class = { 0 };
    dlrf_runtime_class_entry_t entry = { NULL, &runtime_class };
    fake_method_t method = { 0 };
    fake_resolver_t resolver = { 0 };
    void *invokers[1];
    dlrf_method_info_t method_info;

    GetSystemInfo(&system);
    page = system.dwPageSize;
    pages = VirtualAlloc(NULL, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    EXPECT_NOT_NULL(pages);
    EXPECT_TRUE(VirtualProtect(pages + page, page, PAGE_NOACCESS, &old_protect));

    /* The terminator is the last readable byte. */
    runtime_class.name = (const char *)pages + page - sizeof(name);
    memcpy(pages + page - sizeof(name), name, sizeof(name));
    EXPECT_EQ(dlrf_find_runtime_class(&entry, 1, name), &runtime_class);
    EXPECT_NULL(dlrf_find_runtime_class(&entry, 1, "CSAutoControlAP"));
    EXPECT_NULL(dlrf_find_runtime_class(&entry, 1, "CSAutoControlAPI2"));

    /* The terminator would sit in the inaccessible page. */
    runtime_class.name = (const char *)pages + page - (sizeof(name) - 1);
    memcpy(pages + page - (sizeof(name) - 1), name, sizeof(name) - 1);
    EXPECT_NULL(dlrf_find_runtime_class(&entry, 1, name));

    /* A method vector whose storage is inaccessible. */
    runtime_class.methods = (fake_vector_t){ NULL, pages + page, pages + page + sizeof(method),
                                             pages + page + sizeof(method) };
    EXPECT_TRUE(!dlrf_find_method(&runtime_class, DLRF_VECTOR_ABI_MSVC2015, "SetGameProperty", &method_info));

    /* A readable method whose only invoker is inaccessible. */
    method.name = "SetGameProperty";
    method.resolver = &resolver;
    invokers[0] = pages + page;
    resolver.invokers = (fake_vector_t){ NULL, invokers, invokers + 1, invokers + 1 };
    runtime_class.methods = (fake_vector_t){ NULL, &method, &method + 1, &method + 1 };
    EXPECT_TRUE(!dlrf_find_method(&runtime_class, DLRF_VECTOR_ABI_MSVC2015, "SetGameProperty", &method_info));

    EXPECT_TRUE(VirtualFree(pages, 0, MEM_RELEASE));
    return 0;
}

int main(void) {
    struct {
        unsigned char type_ids[3];
        fake_runtime_class_t classes[3];
        dlrf_runtime_class_entry_t entries[4];
    } data = { 0 };
    void *vtable = &data;
    dlrf_range_t data_range = { (const unsigned char *)&data, sizeof(data) };
    dlrf_range_t rdata = { (const unsigned char *)&vtable, sizeof(vtable) };
    const dlrf_runtime_class_entry_t *registry = NULL;
    fake_full_class_t control_class = { 0 };
    fake_method_t methods[1] = { 0 };
    fake_resolver_t resolver = { 0 };
    fake_invoker_t invoker = { 0 };
    void *invokers[1] = { &invoker };
    dlrf_method_info_t method_info;

    data.classes[0].vtable = &vtable;
    data.classes[1].vtable = &vtable;
    data.classes[2].vtable = &vtable;
    data.entries[0] = (dlrf_runtime_class_entry_t){ &data.type_ids[0], &data.classes[0] };
    data.entries[1] = (dlrf_runtime_class_entry_t){ &data.type_ids[1], &data.classes[1] };
    data.entries[2] = (dlrf_runtime_class_entry_t){ &data.type_ids[2], &data.classes[2] };

    EXPECT_EQ(dlrf_find_registry(data.entries, 4, data_range, rdata, &registry), 3);
    EXPECT_EQ(registry, data.entries);

    control_class.name = "CSAutoControlAPI";
    methods[0].name = "SetGameProperty";
    methods[0].name_len = strlen(methods[0].name);
    resolver.name = methods[0].name;
    resolver.invokers = (fake_vector_t){ NULL, invokers, invokers + 1, invokers + 1 };
    methods[0].resolver = &resolver;
    control_class.methods = (fake_vector_t){ NULL, methods, methods + 1, methods + 1 };
    invoker.address = (void *)(uintptr_t)0x12345678;
    {
        dlrf_runtime_class_entry_t control_entry = { NULL, &control_class };
        EXPECT_EQ(dlrf_find_runtime_class(&control_entry, 1, "CSAutoControlAPI"), &control_class);
    }
    EXPECT_TRUE(dlrf_find_method(&control_class, DLRF_VECTOR_ABI_MSVC2015, "SetGameProperty", &method_info));
    EXPECT_EQ(method_info.resolver, &resolver);
    EXPECT_EQ(method_info.invoker, &invoker);
    EXPECT_EQ(method_info.address, (void *)(uintptr_t)0x12345678);
    EXPECT_EQ(method_info.invoker_count, 1);
    control_class.methods.last = (void *)((uintptr_t)methods + 1);
    EXPECT_TRUE(!dlrf_find_method(&control_class, DLRF_VECTOR_ABI_MSVC2015, "SetGameProperty", &method_info));
    {
        fake_full_class_t legacy_class = { 0 };
        fake_vector_2012_t legacy_methods = { methods, methods + 1, methods + 1, NULL };
        fake_vector_2012_t legacy_invokers = { invokers, invokers + 1, invokers + 1, NULL };
        memcpy(&legacy_class.methods, &legacy_methods, sizeof(legacy_methods));
        memcpy(&resolver.invokers, &legacy_invokers, sizeof(legacy_invokers));
        legacy_class.name = "LegacyControlAPI";
        EXPECT_TRUE(dlrf_find_method(&legacy_class, DLRF_VECTOR_ABI_MSVC2012, "SetGameProperty", &method_info));
        EXPECT_EQ(method_info.address, (void *)(uintptr_t)0x12345678);
    }
    EXPECT_EQ(test_readability_boundaries(), 0);
    printf("smoke_dlrf: all tests passed\n");
    return 0;
}
