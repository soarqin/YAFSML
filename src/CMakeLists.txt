include(GenerateVersionInfo)

set(YAFSML_VERSION "0.8.0")

add_subdirectory(process)
add_subdirectory(dearxan)
add_subdirectory(game)
add_subdirectory(steam)
add_subdirectory(modloader)
add_subdirectory(launcher)

add_dependencies(modloader_launcher modloader_dll)

# Distribution target
add_custom_target(dist)
add_custom_command(TARGET dist POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_BINARY_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist/package"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:modloader_launcher>" "$<TARGET_FILE:modloader_dll>" "${CMAKE_CURRENT_SOURCE_DIR}/YAFSML.ini" "${CMAKE_CURRENT_SOURCE_DIR}/YAFSML.zhCN.ini" "${CMAKE_SOURCE_DIR}/README.md" "${CMAKE_SOURCE_DIR}/README.zhCN.md" "${CMAKE_SOURCE_DIR}/CHANGELOG.md" "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${CMAKE_BINARY_DIR}/dist/package/"
    COMMENT "Copying files...")

#add_custom_command(TARGET dist POST_BUILD
#    COMMAND "$<TARGET_FILE:embeddll>" "${CMAKE_BINARY_DIR}/dist/$<TARGET_FILE_NAME:modloader_launcher>" "${CMAKE_BINARY_DIR}/dist/$<TARGET_FILE_NAME:modloader_dll>"
#    COMMENT "Embedding modloader DLL into exe..."
#)

# Compress to a zip and a 7z file
add_custom_command(TARGET dist POST_BUILD
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/dist/package"
    COMMAND ${CMAKE_COMMAND} -E tar "cf" "${CMAKE_BINARY_DIR}/dist/YAFSML-${YAFSML_VERSION}.zip" --format=zip -- "$<TARGET_FILE_NAME:modloader_launcher>" "$<TARGET_FILE_NAME:modloader_dll>" "YAFSML.ini" "YAFSML.zhCN.ini" "README.md" "README.zhCN.md" "CHANGELOG.md" "LICENSE" "THIRD_PARTY_NOTICES.md"
    COMMAND ${CMAKE_COMMAND} -E tar "cf" "${CMAKE_BINARY_DIR}/dist/YAFSML-${YAFSML_VERSION}.7z" --format=7zip -- "$<TARGET_FILE_NAME:modloader_launcher>" "$<TARGET_FILE_NAME:modloader_dll>" "YAFSML.ini" "YAFSML.zhCN.ini" "README.md" "README.zhCN.md" "CHANGELOG.md" "LICENSE" "THIRD_PARTY_NOTICES.md"
    COMMENT "Compressing to archives..."
)

add_dependencies(dist #[[embeddll]] modloader_launcher)
