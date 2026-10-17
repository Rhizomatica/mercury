add_library(hidapi STATIC
    src/hid.c
)

target_include_directories(hidapi PUBLIC "include")