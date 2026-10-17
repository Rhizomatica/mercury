add_library(ffaudio STATIC)

if(LINUX)
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(libpulse REQUIRED IMPORTED_TARGET libpulse)
    pkg_check_modules(alsa REQUIRED IMPORTED_TARGET alsa)

    target_link_libraries(ffaudio PRIVATE PkgConfig::libpulse PkgConfig::alsa)
    target_sources(ffaudio PRIVATE
        ffaudio/pulse.c
        ffaudio/alsa.c
    )
elseif(WIN32)
    target_sources(ffaudio PRIVATE
        ffaudio/wasapi.c
        ffaudio/dsound.c
    )
elseif(APPLE)
    target_sources(ffaudio PRIVATE
        ffaudio/coreaudio.c
    )
elseif(BSD)
    target_sources(ffaudio PRIVATE
        ffaudio/oss.c
    )
endif()

target_include_directories(ffaudio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")