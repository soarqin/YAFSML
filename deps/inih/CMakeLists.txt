add_project(inih STATIC ini.c ini.h LANGUAGES C FOLDER extlibs)
target_include_directories(${PROJECT_NAME} PUBLIC .)
# Disallow multi-line value continuation (configparser-style). For this project
# every value is a single line (dll/mod paths, bool flags). With the default
# behaviour, any indented line after a name=value pair is treated as a
# continuation of the *previous* key and re-dispatched under that key's name,
# which silently swallows the indented entry's own key. This caused extension
# DLLs to intermittently go missing from the [dlls] section.
# Grow the line buffer as needed instead of imposing a fixed byte limit.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    INI_ALLOW_MULTILINE=0 INI_USE_STACK=0 INI_ALLOW_REALLOC=1 INI_MAX_LINE=0)
