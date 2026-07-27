add_project(dearxan STATIC
    dearxan.c dearxan.h
    image.c image.h
    encryption.c encryption.h
    analysis.c analysis.h
    vm.c vm.h
    patch.c patch.h
    entry_point.c entry_point.h
    steamstub.c steamstub.h
    disabler.c
    FOLDER libs
)

target_include_directories(dearxan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dearxan PRIVATE Zydis)
