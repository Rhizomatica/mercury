INCLUDE(FindPkgConfig)
PKG_CHECK_MODULES(PC_HERMES_OFDM hermes_ofdm)

FIND_PATH(
    HERMES_OFDM_INCLUDE_DIRS
    NAMES hermes_ofdm/api.h
    HINTS $ENV{HERMES_OFDM_DIR}/include
        ${PC_HERMES_OFDM_INCLUDEDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/include
          /usr/local/include
          /usr/include
)

FIND_LIBRARY(
    HERMES_OFDM_LIBRARIES
    NAMES gnuradio-hermes_ofdm
    HINTS $ENV{HERMES_OFDM_DIR}/lib
        ${PC_HERMES_OFDM_LIBDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/lib
          ${CMAKE_INSTALL_PREFIX}/lib64
          /usr/local/lib
          /usr/local/lib64
          /usr/lib
          /usr/lib64
)

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(HERMES_OFDM DEFAULT_MSG HERMES_OFDM_LIBRARIES HERMES_OFDM_INCLUDE_DIRS)
MARK_AS_ADVANCED(HERMES_OFDM_LIBRARIES HERMES_OFDM_INCLUDE_DIRS)

