cmake_minimum_required(VERSION 3.31)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

project(mercury
    VERSION "1.9.19"
    LANGUAGES C
)

# Options
option(BUILD_GUI "Build Fyne-UI based GUI written in Golang" ON)
option(ENABLE_TESTS "Enable testing" ON)

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
