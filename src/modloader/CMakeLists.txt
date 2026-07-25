enable_language(ASM_NASM)
project(modloader LANGUAGES C ASM_NASM VERSION "${YAFSML_VERSION}")

set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)

option(ML_STRIP_MODENGINE_CONFIG_SUPPORT "Strip ModEngine config support" OFF)

# dll target
generate_version_info(dll_res_files
    NAME "YAFSML"
    VERSION_MAJOR ${PROJECT_VERSION_MAJOR}
    VERSION_MINOR ${PROJECT_VERSION_MINOR}
    VERSION_PATCH ${PROJECT_VERSION_PATCH}
    VERSION_REVISION ${PROJECT_VERSION_TWEAK}
    ORIGINAL_FILENAME "YAFSML.dll"
    COMPANY_NAME "Soar Qin"
    FILE_DESCRIPTION "Yet Another FromSoftware Mod Loader")

add_project(modloader_dll SHARED
    modloader.c
    ${CMAKE_SOURCE_DIR}/src/common/allocator.c
    ${CMAKE_SOURCE_DIR}/src/common/log.h
    config.c config.h
    extdll.c extdll.h
    gamehook.c gamehook.h
    hook.c hook.h
    hook_batch.c hook_batch.h
    lifecycle.c lifecycle.h
    dl_allocator.c dl_allocator.h
    mimalloc_allocator.c mimalloc_allocator.h
    mod.c mod.h
    path_convert.c path_convert.h
    vfs.c vfs.h
    patches/common.c patches/common.h
    patches/allocator.c patches/allocator.h
    patches/logo.c patches/logo.h
    patches/properties.c patches/properties.h patches/properties_hook.asm
    patches/regulation.c patches/regulation.h
    patches/runtime_ready.c patches/runtime_ready.h
    patches/save_mapping.c patches/save_mapping.h
    patches/win32_hooks.c patches/win32_hooks.h
    patches/window_flash.c patches/window_flash.h
    patches/wwise_path.c patches/wwise_path.h
    patches/dl_device.c patches/dl_device.h
    patches/asset_sig.c patches/asset_sig.h
    patches/asset_hooks.c patches/asset_hooks.h
    patches/eldenring.c patches/eldenring.h
    patches/nightreign.c patches/nightreign.h
    patches/sekiro.c patches/sekiro.h
    patches/darksouls3.c patches/darksouls3.h

    proxy/winhttp.c proxy/winhttp.h proxy/winhttp.def
    proxy/dinput8.c proxy/dinput8.h proxy/dinput8.def
    proxy/dxgi.c proxy/dxgi.h proxy/dxgi.def
    proxy/proxy_stubs.asm
    proxy/common.c proxy/common.h

    ${dll_res_files}

    LANGUAGES C ASM_NASM

    INLINE_TARGET OUTPUT_NAME "YAFSML" NO_PREFIX FOLDER app
)

target_include_directories(modloader_dll PRIVATE ..)
target_include_directories(modloader_dll PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
add_library(modloader_log OBJECT ${CMAKE_SOURCE_DIR}/src/common/log.c)
target_include_directories(modloader_log PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
target_sources(modloader_dll PRIVATE $<TARGET_OBJECTS:modloader_log>)
target_compile_definitions(modloader_dll PRIVATE ML_USE_MIMALLOC)
if (ML_STRIP_MODENGINE_CONFIG_SUPPORT)
    target_compile_definitions(modloader_dll PRIVATE ML_STRIP_MODENGINE_CONFIG)
else ()
    target_link_libraries(modloader_dll toml-c)
endif ()
target_link_libraries(modloader_dll game_registry process steam minhook inih klib mimalloc-static miniz xxhash gdi32 shlwapi version)
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/common/log.c PROPERTIES COMPILE_DEFINITIONS "ML_LOG_IMPLEMENTATION")
if (NOT MSVC)
    target_link_options(modloader_dll PRIVATE -municode)
endif ()
