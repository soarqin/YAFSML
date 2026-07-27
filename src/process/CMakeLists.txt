add_project(process STATIC
    fd4_step.c fd4_step.h
    fd4_step_static.c fd4_step_static.h
    dlrf.c dlrf.h
    image.c image.h
    pe.c pe.h
    rip_scan.c rip_scan.h
    rtti.c rtti.h
    scanner.c scanner.h
    singleton.c singleton.h
    undname.c undname.h
    util.c util.h
    FOLDER libs
)

target_link_libraries(process PRIVATE klib)
target_include_directories(process PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
