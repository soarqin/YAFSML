set(YAFSML_MANAGER_VERSION "0.1.0")

project(yafsml_manager LANGUAGES C VERSION "${YAFSML_MANAGER_VERSION}")

generate_version_info(manager_res_files
    RC_FOLDER "../modloader"
    NAME "YAFSML Manager"
    VERSION_MAJOR ${PROJECT_VERSION_MAJOR}
    VERSION_MINOR ${PROJECT_VERSION_MINOR}
    VERSION_PATCH ${PROJECT_VERSION_PATCH}
    VERSION_REVISION ${PROJECT_VERSION_TWEAK}
    ORIGINAL_FILENAME "YAFSML.Manager.exe"
    COMPANY_NAME "Soar Qin"
    FILE_DESCRIPTION "YAFSML configuration manager")

add_project(yafsml_manager WIN32
    manager.c
    config_model.c
    updater.c
    locale.c
    manager_manifest.rc
    ${manager_res_files}
    INLINE_TARGET OUTPUT_NAME "YAFSML.Manager" FOLDER app)

target_include_directories(yafsml_manager PRIVATE . .. ${CMAKE_SOURCE_DIR}/src/common)
target_compile_definitions(yafsml_manager PRIVATE
    YAFSML_MANAGER_VERSION_TEXT=L"${YAFSML_MANAGER_VERSION}"
    YAFSML_LOADER_VERSION_TEXT=L"${YAFSML_VERSION}")
if (MSVC)
    # Embed the explicit Common Controls v6 manifest resource without also
    # asking the linker to synthesize its default manifest.
    target_link_options(yafsml_manager PRIVATE "/MANIFEST:NO")
endif ()
target_link_libraries(yafsml_manager inih miniz comctl32 ole32 oleaut32 shell32 shlwapi winhttp bcrypt version)
