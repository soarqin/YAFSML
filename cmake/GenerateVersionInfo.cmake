include (CMakeParseArguments)

# generate_version_info() function
#
# This function uses versioninfo.h.in template file and versioninfo.rc.in file
# to generate WIN32 resource with version information and general resource strings.
#
# Usage:
#   generate_version_info(
#     SomeOutputResourceVariable
#     NAME MyGreatProject
#     VERSION_MAJOR 2
#     VERSION_MINOR 3
#     VERSION_PATCH ${BUILD_COUNTER}
#     VERSION_REVISION ${BUILD_REVISION}
#   )
# where BUILD_COUNTER and BUILD_REVISION could be values from your CI server.
#
# You can use generated resource for your executable targets:
#   add_executable(target-name ${target-files} ${SomeOutputResourceVariable})
#
# You can specify resource strings in arguments:
#   RC_FOLDER          - folder with versioninfo.h.in and versioninfo.rc.in files (default for current folder)
#   NAME               - name of executable (no defaults, ex: Microsoft Word)
#   BUNDLE             - bundle (${NAME} is default, ex: Microsoft Office)
#   VERSION_MAJOR      - 1 is default
#   VERSION_MINOR      - 0 is default
#   VERSION_PATCH      - 0 is default
#   VERSION_REVISION   - 0 is default
#   COMPANY_NAME       - your company name (no defaults)
#   COMPANY_COPYRIGHT  - ${COMPANY_NAME} (C) Copyright ${CURRENT_YEAR} is default
#   COMMENTS           - ${NAME} v${VERSION_MAJOR}.${VERSION_MINOR} is default
#   ORIGINAL_FILENAME  - ${NAME} is default
#   INTERNAL_NAME      - ${NAME} is default
#   FILE_DESCRIPTION   - ${NAME} is default
#   FILE_TYPE          - VFT_APP is default (VFT_DLL for libraries)
function(generate_version_info outfiles)
    set (options)
    set (oneValueArgs
        RC_FOLDER
        NAME
        BUNDLE
        VERSION_MAJOR
        VERSION_MINOR
        VERSION_PATCH
        VERSION_REVISION
        COMPANY_NAME
        COMPANY_COPYRIGHT
        COMMENTS
        ORIGINAL_FILENAME
        INTERNAL_NAME
        FILE_DESCRIPTION
        FILE_TYPE)
    set (multiValueArgs)
    cmake_parse_arguments(PRODUCT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT PRODUCT_BUNDLE OR "${PRODUCT_BUNDLE}" STREQUAL "")
        set(PRODUCT_BUNDLE "${PRODUCT_NAME}")
    endif()

    if (NOT PRODUCT_VERSION_MAJOR EQUAL 0 AND (NOT PRODUCT_VERSION_MAJOR OR "${PRODUCT_VERSION_MAJOR}" STREQUAL ""))
        set(PRODUCT_VERSION_MAJOR 1)
    endif()
    if (NOT PRODUCT_VERSION_MINOR EQUAL 0 AND (NOT PRODUCT_VERSION_MINOR OR "${PRODUCT_VERSION_MINOR}" STREQUAL ""))
        set(PRODUCT_VERSION_MINOR 0)
    endif()
    if (NOT PRODUCT_VERSION_PATCH EQUAL 0 AND (NOT PRODUCT_VERSION_PATCH OR "${PRODUCT_VERSION_PATCH}" STREQUAL ""))
        set(PRODUCT_VERSION_PATCH 0)
    endif()
    if (NOT PRODUCT_VERSION_REVISION EQUAL 0 AND (NOT PRODUCT_VERSION_REVISION OR "${PRODUCT_VERSION_REVISION}" STREQUAL ""))
        set(PRODUCT_VERSION_REVISION 0)
    endif()

    if (NOT PRODUCT_COMPANY_COPYRIGHT OR "${PRODUCT_COMPANY_COPYRIGHT}" STREQUAL "")
        string(TIMESTAMP PRODUCT_CURRENT_YEAR "%Y")
        set(PRODUCT_COMPANY_COPYRIGHT "${PRODUCT_COMPANY_NAME} (c) Copyright ${PRODUCT_CURRENT_YEAR}")
    endif()
    if (NOT PRODUCT_COMMENTS OR "${PRODUCT_COMMENTS}" STREQUAL "")
        set(PRODUCT_COMMENTS "${PRODUCT_NAME} v${PRODUCT_VERSION_MAJOR}.${PRODUCT_VERSION_MINOR}")
    endif()
    if (NOT PRODUCT_ORIGINAL_FILENAME OR "${PRODUCT_ORIGINAL_FILENAME}" STREQUAL "")
        set(PRODUCT_ORIGINAL_FILENAME "${PRODUCT_NAME}")
    endif()
    if (NOT PRODUCT_INTERNAL_NAME OR "${PRODUCT_INTERNAL_NAME}" STREQUAL "")
        set(PRODUCT_INTERNAL_NAME "${PRODUCT_NAME}")
    endif()
    if (NOT PRODUCT_FILE_DESCRIPTION OR "${PRODUCT_FILE_DESCRIPTION}" STREQUAL "")
        set(PRODUCT_FILE_DESCRIPTION "${PRODUCT_NAME}")
    endif()
    if (NOT PRODUCT_FILE_TYPE OR "${PRODUCT_FILE_TYPE}" STREQUAL "")
        set(PRODUCT_FILE_TYPE VFT_APP)
    endif()

    set(_VersionInfoFile ${CMAKE_CURRENT_BINARY_DIR}/versioninfo.h)
    set(_VersionResourceFile ${CMAKE_CURRENT_BINARY_DIR}/versioninfo.rc)
    if (NOT PRODUCT_RC_FOLDER OR "${PRODUCT_RC_FOLDER}" STREQUAL "")
        set(PRODUCT_RC_FOLDER "${CMAKE_CURRENT_LIST_DIR}")
    endif ()
    configure_file(
        ${PRODUCT_RC_FOLDER}/versioninfo.h.in
        ${_VersionInfoFile}
        @ONLY)
    configure_file(
        ${PRODUCT_RC_FOLDER}/versioninfo.rc.in
        ${_VersionResourceFile}
        COPYONLY)
    list(APPEND ${outfiles} ${_VersionInfoFile} ${_VersionResourceFile})
    set (${outfiles} ${${outfiles}} PARENT_SCOPE)
endfunction()
