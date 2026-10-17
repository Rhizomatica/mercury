add_library(iniparser STATIC
    dictionary.c
    iniparser.c
)

target_include_directories(iniparser PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")