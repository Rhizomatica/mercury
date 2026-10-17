add_library(hidapi STATIC
    src/hidapi_cfgmgr32.h
    src/hidapi_descriptor_reconstruct.c
    src/hidapi_descriptor_reconstruct.h
    src/hidapi_hidclass.h
    src/hidapi_hidpi.h
    src/hidapi_hidsdi.h
    src/hidapi_winapi.h
    src/hid.c
)

target_include_directories(hidapi PUBLIC "include")