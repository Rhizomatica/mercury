cmake_minimum_required(VERSION 3.31)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

project(mercury
    VERSION "1.9.19"
    LANGUAGES C
)

# Options
option(ENABLE_HAMLIB "Build with hamlib support for rig steering" ON)
option(BUILD_GUI "Build Fyne-UI based GUI written in Golang" ON)
option(ENABLE_TESTS "Enable testing" ON)
option(PACK_DEPS "Install required libraries to the installation path (use for Windows and MacOS)" OFF)

if(PACK_DEPS AND LINUX)
    message(WARNING "Use PACK_DEPS on Linux only for building AppImage!")
endif()

# Subdirectories
add_subdirectory(third_party)
add_subdirectory(src)

if(ENABLE_TESTS)
    add_subdirectory(tests)
endif()

# Misc installation rules
include(GNUInstallDirs)

if(LINUX)
    install(FILES packaging/mercury.1
        DESTINATION "${CMAKE_INSTALL_MANDIR}/1"
    )

    install(FILES packaging/mercury.ini.example
        DESTINATION "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}"
    )
endif()

# CPack logic
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Digital Radio OFDM protocol for HF broadcast and peer-to-peer ARQ connections with compatible TCP interfaces.")
set(CPACK_PACKAGE_DESCRIPTION ${CPACK_PACKAGE_DESCRIPTION_SUMMARY})
set(CPACK_PACKAGE_CONTACT "Rhizomatica")
set(CPACK_PACKAGE_VENDOR "Rhizomatica")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CMAKE_PROJECT_HOMEPAGE_URL "https://github.com/Rhizomatica/mercury")

if(LINUX)
    set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/docs/app/icon-512.svg")

    # Deb
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION "hamradio")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

    # RPM
    set(CPACK_RPM_PACKAGE_LICENSE ${CPACK_PACKAGE_LICENSE})
    set(CPACK_RPM_PACKAGE_DESCRIPTION ${CPACK_PACKAGE_DESCRIPTION_SUMMARY})
    set(CPACK_RPM_PACKAGE_URL ${CMAKE_PROJECT_HOMEPAGE_URL})
    set(CPACK_RPM_PACKAGE_AUTOREQ ON)
elseif(WIN32)
    set(CPACK_NSIS_DISPLAY_NAME ${PROJECT_NAME})
    set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/mercury.ico")
    set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/mercury.ico")
    set(CPACK_NSIS_URL_INFO_ABOUT ${CMAKE_PROJECT_HOMEPAGE_URL})
elseif(APPLE)
    message(STATUS "CPack configuration is not yet ready for MacOS")
endif()

include(CPack)