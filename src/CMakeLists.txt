add_executable(mercury)

# Search for libraries
if(LINUX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(hidapi-libusb REQUIRED IMPORTED_TARGET hidapi-libusb)
    add_library(hidapi ALIAS PkgConfig::hidapi-libusb)
endif()
# For MacOS and Windows the libraries are already configured

if(ENABLE_HAMLIB)
    if(LINUX)
        pkg_check_modules(hamlib REQUIRED IMPORTED_TARGET hamlib)
        add_library(hamlib ALIAS PkgConfig::hamlib)
    endif()

    # For MacOS and Windows the libraries are already configured

    target_link_libraries(mercury hamlib)
    target_compile_definitions(mercury "HAVE_HAMLIB")
endif()

target_link_libraries(mercury
    ffaudio
    freedv
    iniparser
    hidapi
)

# GUI building
# if(BUILD_GUI)

# endif()