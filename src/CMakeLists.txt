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

    target_link_libraries(mercury hamlib)
    target_compile_definitions(mercury PRIVATE "HAVE_HAMLIB")
endif()

find_package(Threads REQUIRED)

target_link_libraries(mercury
    Threads::Threads
    m
    ffaudio
    freedv
    raptorq
    iniparser
    hidapi
)

# Add compile definitions
target_compile_definitions(mercury PRIVATE _GNU_SOURCE)

# Target sources
target_sources(mercury PRIVATE main.c)

# audioio
target_sources(mercury PRIVATE
    audioio/audio_dev_limits.h
    audioio/audioio.c
    audioio/audioio.h
    # audioio/oss4-probe.c
    audioio/pcm24.h
    audioio/resampler.c
    audioio/resampler.h
    audioio/rtp_wire.h
    audioio/sock_wire.h
    audioio/std.h
)

target_include_directories(mercury PRIVATE audioio)

# common
target_sources(mercury PRIVATE
    common/cfg_utils.c
    common/cfg_utils.h
    common/chan.c
    common/chan.h
    common/crc6.c
    common/crc6.h
    common/defines_modem.h
    common/hermes_log.c
    common/hermes_log.h
    common/mercury_cli.c
    common/mercury_cli.h
    common/mercury_engine.c
    common/mercury_engine.h
    common/mercury_modes.c
    common/mercury_prng.c
    common/mercury_prng.h
    common/mercury_version.h
    common/message_store.c
    common/message_store.h
    common/os_interop.c
    common/os_interop.h
    common/queue.c
    common/queue.h
    common/ring_buffer_posix.c
    common/ring_buffer_posix.h
    common/shm_posix.c
    common/shm_posix.h
    common/virtual_clock.c
    common/virtual_clock.h
    common/watterson.c
    common/watterson.h
)

target_include_directories(mercury PRIVATE common)

# data_interfaces
target_sources(mercury PRIVATE
    data_interfaces/net.c
    data_interfaces/net.h
    data_interfaces/tcp_interfaces.c
    data_interfaces/tcp_interfaces.h
)

target_include_directories(mercury PRIVATE data_interfaces)

# datalink_arq
target_sources(mercury PRIVATE
    datalink_arq/arith.c
    datalink_arq/arq.c
    datalink_arq/arq_channels.c
    datalink_arq/arq_channels.h
    datalink_arq/arq_events.h
    datalink_arq/arq_fsm.c
    datalink_arq/arq_fsm.h
    datalink_arq/arq.h
    datalink_arq/arq_modem.c
    datalink_arq/arq_modem.h
    datalink_arq/arq_protocol.c
    datalink_arq/arq_protocol.h
    datalink_arq/arq_timing.c
    datalink_arq/arq_timing.h
    datalink_arq/arq_tnc.c
    datalink_arq/arq_tnc.h
    datalink_arq/arq_trace.c
    datalink_arq/arq_trace.h
    datalink_arq/carousel.c
    datalink_arq/carousel.h
    datalink_arq/rs_erasure.c
    datalink_arq/rs_erasure.h
)

target_include_directories(mercury PRIVATE datalink_arq)

# datalink_broadcast
target_sources(mercury PRIVATE
    datalink_broadcast/bcast_file.c
    datalink_broadcast/bcast_file.h
    datalink_broadcast/bcast_modes.h
    datalink_broadcast/broadcast.c
    datalink_broadcast/broadcast.h
    datalink_broadcast/kiss.c
    datalink_broadcast/kiss.h
)

target_include_directories(mercury PRIVATE datalink_broadcast)

# gui_interface
target_sources(mercury PRIVATE
    gui_interface/ui_communication.c
    gui_interface/ui_communication.h
    gui_interface/ui_devices.c
    gui_interface/ui_history.c
    gui_interface/ui_history.h
    gui_interface/ui_status.c
    gui_interface/ui_status.h
    gui_interface/websocket/mercury_websocket.c
    gui_interface/websocket/mercury_websocket.h
    gui_interface/websocket/web_assets.h
    gui_interface/websocket/ws_crypto.c
    gui_interface/websocket/ws_crypto.h
    gui_interface/websocket/ws_frame.c
    gui_interface/websocket/ws_frame.h
    gui_interface/websocket/ws_json.c
    gui_interface/websocket/ws_json.h
    gui_interface/websocket/ws_server.c
    gui_interface/websocket/ws_server.h
    gui_interface/websocket/ws_tls.c
    gui_interface/websocket/ws_tls.h
)

target_include_directories(mercury PRIVATE gui_interface)

# modem
target_sources(mercury PRIVATE
    modem/channel_busy.c
    modem/channel_busy.h
    modem/framer.c
    modem/framer.h
    modem/modem_backend.h
    modem/modem.c
    modem/modem_freedv.c
    modem/modem_freedv.h
    modem/modem.h
    modem/modem_mfsk.c
    modem/modem_mfsk.h
    modem/monitor_frame.c
    modem/monitor_frame.h
    modem/tx_pacing.h
    modem/mfsk/mfsk.c
    modem/mfsk/mfsk.h
    modem/mfsk/mfsk_ldpc_1_16.c
    modem/mfsk/mfsk_ldpc_2_16.c
    modem/mfsk/mfsk_ldpc_3_16.c
    modem/mfsk/mfsk_ldpc_5_16.c
    modem/mfsk/mfsk_ldpc_8_16.c
    modem/mfsk/mfsk_ldpc.c
    modem/mfsk/mfsk_ldpc.h
    modem/mfsk/mfsk_ofdm.c
    modem/mfsk/mfsk_ofdm.h
    modem/mfsk/mfsk_sync.c
    modem/mfsk/mfsk_sync.h
)

target_include_directories(mercury PRIVATE modem)

# radio_io
target_sources(mercury PRIVATE
    radio_io/cm108_ptt.c
    radio_io/cm108_ptt.h
    radio_io/radio_cmds.h
    radio_io/radio_io.c
    radio_io/radio_io.h
    radio_io/radio_port.h
    radio_io/rigctl_parse.c
    radio_io/rigctl_parse.h
    radio_io/sbitx_io.c
    radio_io/sbitx_io.h
    radio_io/serial_ptt.c
    radio_io/serial_ptt.h
    radio_io/shm_utils.c
    radio_io/shm_utils.h
)

target_include_directories(mercury PRIVATE radio_io)

target_include_directories(mercury PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# GUI building
# if(BUILD_GUI)

# endif()