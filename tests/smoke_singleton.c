#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "test_common.h"
#include "process/singleton.h"

/* A mapped PE image with .text, .rdata and .data, built in executable memory
 * so that the recorded FD4 get_name can really be called. */
enum {
    IMAGE_SIZE = 0x4000,
    TEXT_RVA = 0x1000,
    RDATA_RVA = 0x2000,
    DATA_RVA = 0x3000,
    SECTION_SIZE = 0x1000,
};

typedef struct image_headers_s {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    IMAGE_SECTION_HEADER sections[3];
} image_headers_t;

static uint8_t *image;
static uint8_t *text;
static uint8_t *rdata;
static uint8_t *data;
static int objects[4];

static void add_section(IMAGE_SECTION_HEADER *section, const char *name, uint32_t rva) {
    memcpy(section->Name, name, strlen(name));
    section->VirtualAddress = rva;
    section->Misc.VirtualSize = SECTION_SIZE;
    section->SizeOfRawData = SECTION_SIZE;
}

static void build_headers(void) {
    image_headers_t *headers = (image_headers_t *)image;

    headers->dos.e_magic = IMAGE_DOS_SIGNATURE;
    headers->dos.e_lfanew = (LONG)offsetof(image_headers_t, nt);
    headers->nt.Signature = IMAGE_NT_SIGNATURE;
    headers->nt.FileHeader.NumberOfSections = 3;
    headers->nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    headers->nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    headers->nt.OptionalHeader.SizeOfImage = IMAGE_SIZE;
    add_section(&headers->sections[0], ".text", TEXT_RVA);
    add_section(&headers->sections[1], ".rdata", RDATA_RVA);
    add_section(&headers->sections[2], ".data", DATA_RVA);
}

static uint8_t *emit(uint8_t *at, const void *bytes, size_t size) {
    memcpy(at, bytes, size);
    return at + size;
}

/* A 7-byte `op [rip+disp32]` instruction whose prefix is the first 3 bytes. */
static uint8_t *emit_rip(uint8_t *at, const uint8_t prefix[3], const void *target) {
    int32_t displacement = (int32_t)((const uint8_t *)target - (at + 7));
    memcpy(at, prefix, 3);
    memcpy(at + 3, &displacement, sizeof(displacement));
    return at + 7;
}

static uint8_t *emit_rel32(uint8_t *at, uint8_t opcode, const void *target) {
    int32_t displacement = (int32_t)((const uint8_t *)target - (at + 5));
    at[0] = opcode;
    memcpy(at + 1, &displacement, sizeof(displacement));
    return at + 5;
}

static const uint8_t MOV_RAX_RIP[] = { 0x48, 0x8b, 0x05 };
static const uint8_t MOV_RBX_RIP[] = { 0x48, 0x8b, 0x1d };
static const uint8_t TEST_RAX[] = { 0x48, 0x85, 0xc0 };
static const uint8_t TEST_RBX[] = { 0x48, 0x85, 0xdb };
static const uint8_t JNE_SHORT[] = { 0x75, 0x20 };
static const uint8_t JNE_NEAR[] = { 0x0f, 0x85, 0x20, 0x00, 0x00, 0x00 };
static const uint8_t LEA_RCX[] = { 0x48, 0x8d, 0x0d };
static const uint8_t LEA_R8[] = { 0x4c, 0x8d, 0x05 };
static const uint8_t LEA_R9[] = { 0x4c, 0x8d, 0x0d };
static const uint8_t MOV_R9_RAX[] = { 0x4c, 0x8b, 0xc8 };
static const uint8_t MOV_EDX[] = { 0xba, 0x08, 0x00, 0x00, 0x00 };
static const uint8_t NOPS[] = { 0x66, 0x90 };
static const uint8_t GET_NAME[] = { 0x48, 0x8b, 0x01, 0xc3 }; /* mov rax, [rcx]; ret */

/* Static instance check guarding a DLRF-style registration that names the
 * singleton directly:
 *
 *     mov  reg, [rip+slot]
 *     test reg, reg
 *     jne  ...
 *     lea  rcx, [rip+...]
 *     lea  r8, [rip+...]
 *     lea  r9, [rip+name]
 *     mov  edx, imm32
 *     call ...
 */
static uint8_t *emit_derived(uint8_t *at, void **slot, const char *name, bool rbx_near_padded) {
    at = emit_rip(at, rbx_near_padded ? MOV_RBX_RIP : MOV_RAX_RIP, slot);
    if (rbx_near_padded) at = emit(at, NOPS, sizeof(NOPS));
    at = emit(at, rbx_near_padded ? TEST_RBX : TEST_RAX, sizeof(TEST_RAX));
    at = rbx_near_padded ? emit(at, JNE_NEAR, sizeof(JNE_NEAR)) : emit(at, JNE_SHORT, sizeof(JNE_SHORT));
    at = emit_rip(at, LEA_RCX, text);
    at = emit_rip(at, LEA_R8, text);
    at = emit_rip(at, LEA_R9, name);
    at = emit(at, MOV_EDX, sizeof(MOV_EDX));
    return emit_rel32(at, 0xe8, text);
}

/* The FD4 form resolves the name through the singleton's reflection:
 *
 *     mov  rax, [rip+slot]
 *     test rax, rax
 *     jne  ...
 *     lea  rcx, [rip+reflection]
 *     call get_name
 *     mov  r9, rax
 *     mov  edx, imm32
 *     lea  r8, [rip+...]
 *     lea  rcx, [rip+...]
 *     call ...
 */
static uint8_t *emit_fd4(uint8_t *at, void **slot, const void *reflection, const void *get_name, bool padded) {
    at = emit_rip(at, MOV_RAX_RIP, slot);
    at = emit(at, TEST_RAX, sizeof(TEST_RAX));
    at = emit(at, JNE_SHORT, sizeof(JNE_SHORT));
    at = emit_rip(at, LEA_RCX, reflection);
    at = emit_rel32(at, 0xe8, get_name);
    if (padded) *at++ = 0x90;
    at = emit(at, MOV_R9_RAX, sizeof(MOV_R9_RAX));
    at = emit(at, MOV_EDX, sizeof(MOV_EDX));
    at = emit_rip(at, LEA_R8, text);
    at = emit_rip(at, LEA_RCX, text);
    return emit_rel32(at, 0xe8, text);
}

/* Bump allocators for names and reflections in the fake sections. */
static uint8_t *rdata_cursor;
static uint8_t *data_cursor;

static const char *put_string(uint8_t **cursor, const char *value) {
    const char *at = (const char *)*cursor;
    memcpy(*cursor, value, strlen(value) + 1);
    *cursor += 0x40;
    return at;
}

/* An FD4 reflection: get_name above returns the name pointer it starts with. */
static const void *put_reflection(uint8_t **cursor, const char *name) {
    const void *at = *cursor;
    memcpy(*cursor, &name, sizeof(name));
    *cursor += 0x40;
    return at;
}

static void build_text(void) {
    uint8_t *get_name = text + 0xf00;
    uint8_t *at = text + 0x10;

    rdata_cursor = rdata + 0x100;
    data_cursor = data + 0x100;
    memset(text, 0xcc, SECTION_SIZE);
    emit(get_name, GET_NAME, sizeof(GET_NAME));

    at = emit_derived(at, (void **)(data + 0x10), put_string(&rdata_cursor, "DerivedA"), false) + 8;
    at = emit_derived(at, (void **)(data + 0x18), put_string(&rdata_cursor, "DerivedB"), true) + 8;
    /* Rejected: the slot is not pointer aligned, or the name is not in .rdata. */
    at = emit_derived(at, (void **)(data + 0x21), put_string(&rdata_cursor, "DerivedMisaligned"), false) + 8;
    at = emit_derived(at, (void **)(data + 0x28), put_string(&data_cursor, "DerivedNameInData"), false) + 8;

    at = emit_fd4(at, (void **)(data + 0x40),
                  put_reflection(&data_cursor, put_string(&rdata_cursor, "Fd4Good")), get_name, false) + 8;
    at = emit_fd4(at, (void **)(data + 0x48),
                  put_reflection(&data_cursor, put_string(&rdata_cursor, "Fd4Padded")), get_name, true) + 8;
    /* Rejected: the reflection must be a static object in .data. */
    at = emit_fd4(at, (void **)(data + 0x50),
                  put_reflection(&rdata_cursor, put_string(&rdata_cursor, "Fd4ReflectionInRdata")), get_name,
                  false) + 8;
    /* Rejected: get_name must be in .text. */
    emit_fd4(at, (void **)(data + 0x58),
             put_reflection(&data_cursor, put_string(&rdata_cursor, "Fd4GetNameOutsideText")), data + 0x800,
             false);
}

int main(void) {
    void **slot;

    image = VirtualAlloc(NULL, IMAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    EXPECT_NOT_NULL(image);
    text = image + TEXT_RVA;
    rdata = image + RDATA_RVA;
    data = image + DATA_RVA;
    build_headers();
    build_text();
    ((void **)(data + 0x10))[0] = &objects[0];
    ((void **)(data + 0x18))[0] = &objects[1];
    ((void **)(data + 0x28))[0] = &objects[2];

    EXPECT_TRUE(singleton_test_use_image(image));

    /* Derived singletons resolve straight from the registration. */
    EXPECT_EQ(singleton_find_slot("DerivedA"), (void **)(data + 0x10));
    EXPECT_EQ(singleton_find("DerivedA"), (void *)&objects[0]);
    EXPECT_EQ(singleton_find("DerivedB"), (void *)&objects[1]);
    EXPECT_NULL(singleton_find_slot("DerivedMisaligned"));
    EXPECT_NULL(singleton_find_slot("DerivedNameInData"));

    /* FD4 names are only read once some FD4 instance exists. */
    EXPECT_NULL(singleton_find_slot("Fd4Good"));
    ((void **)(data + 0x40))[0] = &objects[3];
    ((void **)(data + 0x50))[0] = &objects[3];
    ((void **)(data + 0x58))[0] = &objects[3];
    slot = singleton_find_slot("Fd4Good");
    EXPECT_EQ(slot, (void **)(data + 0x40));
    EXPECT_EQ(singleton_find("Fd4Good"), (void *)&objects[3]);
    /* The slot is stable: the caller sees later stores through it. */
    ((void **)(data + 0x40))[0] = &objects[2];
    EXPECT_EQ(*slot, (void *)&objects[2]);
    EXPECT_EQ(singleton_find_slot("Fd4Padded"), (void **)(data + 0x48));
    EXPECT_NULL(singleton_find("Fd4Padded"));
    EXPECT_NULL(singleton_find_slot("Fd4ReflectionInRdata"));
    EXPECT_NULL(singleton_find_slot("Fd4GetNameOutsideText"));

    EXPECT_NULL(singleton_find_slot("Missing"));
    EXPECT_NULL(singleton_find_slot(NULL));
    EXPECT_NULL(singleton_find(NULL));

    printf("smoke_singleton: all tests passed\n");
    return 0;
}
