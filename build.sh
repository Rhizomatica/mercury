#!/bin/bash
# Mercury build script (cross-platform: MSYS2/MinGW64, Linux, macOS)
# Usage: ./build.sh [mode] [clean]
#   Modes: release (default), debug, asan, ubsan, o0, o1, o2, o3
#   clean: removes all object files first
#
# Examples:
#   ./build.sh              # Release build (-O3)
#   ./build.sh debug        # Debug build (-O3 -g)
#   ./build.sh asan         # AddressSanitizer build (-O1 -fsanitize=address)
#   ./build.sh o0           # No optimization (stable, for crash investigation)
#   ./build.sh clean        # Clean only
#   ./build.sh debug clean  # Clean then debug build

set -e
cd "$(dirname "$0")"

MODE="${1:-release}"
CLEAN=""

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        clean) CLEAN=1 ;;
        release|debug|asan|ubsan|o0|o1|o2|o3) MODE="$arg" ;;
    esac
done

# Clean function
do_clean() {
    echo "Cleaning..."
    rm -f mercury mercury.exe mercury_*.exe mercury_*
    rm -f source/*.o source/datalink_layer/*.o source/physical_layer/*.o source/common/*.o
    rm -f source/gui/*.o source/gui/widgets/*.o source/gui/dialogs/*.o
    rm -f third_party/imgui/*.o third_party/imgui/backends/*.o
    rm -f source/audioio/*.o source/audioio/*.a source/audioio/ffaudio/ffaudio/*.o
    rm -f source/crypto/*.o source/crypto/mlkem/*.o
    echo "Clean done."
}

if [ "$CLEAN" = "1" ] && [ "$MODE" = "clean" ]; then
    do_clean
    exit 0
fi

# Always clean — build.sh doesn't track header dependencies,
# so stale .o files cause struct layout ABI mismatches.
do_clean

# Set optimization flags based on mode
case "$MODE" in
    release|o3)
        OPT="-O3"
        DBG="-g"
        SUFFIX=""
        EXTRA_CFLAGS=""
        EXTRA_LDFLAGS=""
        ;;
    debug)
        OPT="-O0"
        DBG="-g"
        SUFFIX="_debug"
        EXTRA_CFLAGS=""
        EXTRA_LDFLAGS=""
        ;;
    asan)
        OPT="-O1"
        DBG="-g -fsanitize=address -fno-omit-frame-pointer"
        SUFFIX="_asan"
        EXTRA_CFLAGS=""
        EXTRA_LDFLAGS="-fsanitize=address"
        ;;
    ubsan)
        OPT="-O3"
        DBG="-g -fsanitize=undefined -fsanitize-undefined-trap-on-error -fno-omit-frame-pointer"
        SUFFIX="_ubsan"
        EXTRA_CFLAGS=""
        EXTRA_LDFLAGS=""
        ;;
    o0)
        OPT="-O0"
        DBG=""
        SUFFIX="_o0"
        EXTRA_CFLAGS=""
        EXTRA_LDFLAGS=""
        ;;
    o1)
        OPT="-O1"
        DBG="-g"
        SUFFIX="_o1"
        EXTRA_CFLAGS=""
        EXTRA_LDFLAGS=""
        ;;
    o2)
        OPT="-O2"
        DBG="-g"
        SUFFIX="_o2"
        EXTRA_CFLAGS=""
        EXTRA_LDFLAGS=""
        ;;
    *)
        echo "Unknown mode: $MODE"
        echo "Usage: $0 [release|debug|asan|o0|o1|o2|o3] [clean]"
        exit 1
        ;;
esac

echo "=== Building Mercury ($MODE) ==="

# Compiler settings
CXX=g++
CC=gcc
CXXFLAGS="$OPT $DBG $EXTRA_CFLAGS -Wall -Wextra -Wno-format -Wno-unused -std=c++14 -I./include -I./source/audioio/ffaudio -I./source/compression -I./source/crypto -I./source/crypto/mlkem -pthread -DMERCURY_GUI_ENABLED -I./third_party/imgui -I./third_party/imgui/backends"
CFLAGS="$OPT $DBG $EXTRA_CFLAGS -Wall -Wno-unused -I./source/audioio/ffbase/ -I./source/audioio/ffaudio/ -I./include -I./source/compression -I./source/crypto -I./source/crypto/mlkem -pthread -std=c17"

# Platform-specific flags
if [[ "$OSTYPE" == "msys"* ]] || [[ "$OSTYPE" == "mingw"* ]] || [[ "$OSTYPE" == "cygwin"* ]]; then
    PLATFORM="windows"
    CXXFLAGS="$CXXFLAGS -I./third_party/glfw/include"
    LDFLAGS="-L./third_party/glfw/lib -lglfw3 -lopengl32 -lgdi32 -lole32 -ldsound -ldxguid -lws2_32 -lbcrypt -static-libgcc -static-libstdc++ -static -l:libwinpthread.a $EXTRA_LDFLAGS"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="macos"
    CXXFLAGS="$CXXFLAGS $(pkg-config --cflags glfw3)"
    LDFLAGS="$(pkg-config --libs glfw3) -framework OpenGL -framework CoreFoundation -framework CoreAudio $EXTRA_LDFLAGS"
else
    PLATFORM="linux"
    CXXFLAGS="$CXXFLAGS $(pkg-config --cflags glfw3)"
    LDFLAGS="$(pkg-config --libs glfw3) -lGL -lpulse -lasound -lpthread -lrt $EXTRA_LDFLAGS"
fi

if [ "$PLATFORM" = "windows" ]; then
    OUTPUT="mercury${SUFFIX}.exe"
else
    OUTPUT="mercury${SUFFIX}"
fi

# Source files
CPP_SOURCES="
source/main.cc
source/datalink_layer/arq_commander.cc
source/datalink_layer/arq_common.cc
source/datalink_layer/arq_responder.cc
source/datalink_layer/b2f_handler.cc
source/datalink_layer/datalink_config.cc
source/datalink_layer/fifo_buffer.cc
source/datalink_layer/tcp_socket.cc
source/datalink_layer/timer.cc
source/physical_layer/awgn.cc
source/physical_layer/crc16_modbus_rtu.cc
source/physical_layer/data_container.cc
source/physical_layer/error_rate.cc
source/physical_layer/fir_filter.cc
source/physical_layer/interleaver.cc
source/physical_layer/interpolator.cc
source/physical_layer/ldpc.cc
source/physical_layer/ldpc_decoder_GBF.cc
source/physical_layer/ldpc_decoder_SPA.cc
source/physical_layer/mercury_met_2_16.cc
source/physical_layer/mercury_normal_1_16.cc
source/physical_layer/mercury_normal_14_16.cc
source/physical_layer/mercury_normal_2_16.cc
source/physical_layer/mercury_normal_3_16.cc
source/physical_layer/mercury_normal_4_16.cc
source/physical_layer/mercury_normal_5_16.cc
source/physical_layer/mercury_normal_6_16.cc
source/physical_layer/mercury_normal_8_16.cc
source/physical_layer/mercury_normal_10_16.cc
source/physical_layer/mercury_normal_12_16.cc
source/physical_layer/misc.cc
source/physical_layer/ofdm.cc
source/physical_layer/physical_config.cc
source/physical_layer/plot.cc
source/physical_layer/psk.cc
source/physical_layer/mfsk.cc
source/physical_layer/telecom_system.cc
source/common/os_interop.cc
source/common/ring_buffer_posix.cc
source/common/shm_posix.cc
source/gui/gui_main.cc
source/gui/ini_parser.cc
source/gui/widgets/waterfall.cc
source/gui/dialogs/setup_dialog.cc
source/gui/dialogs/soundcard_dialog.cc
source/compression/mercury_compress.cc
source/compression/lzhuf_buffer.cc
source/crypto/mercury_crypto.cc
"

# Compression library C sources (PPMd8 from LZMA SDK, zstd amalgamated, LZHUF for B2F)
COMPRESSION_C_SOURCES="
source/compression/ppmd/Ppmd8.c
source/compression/ppmd/Ppmd8Dec.c
source/compression/ppmd/Ppmd8Enc.c
source/compression/zstd/zstd.c
source/compression/lzhuf/lzhuf.c
"

# Crypto C sources (monocypher + ML-KEM-768)
CRYPTO_C_SOURCES="
source/crypto/monocypher.c
source/crypto/mlkem/mlkem_native.c
"

IMGUI_SOURCES="
third_party/imgui/imgui.cpp
third_party/imgui/imgui_draw.cpp
third_party/imgui/imgui_tables.cpp
third_party/imgui/imgui_widgets.cpp
third_party/imgui/backends/imgui_impl_glfw.cpp
third_party/imgui/backends/imgui_impl_opengl3.cpp
"

# Platform-specific audio backends
AUDIO_C_SOURCES="source/audioio/audioio.c"
if [ "$PLATFORM" = "windows" ]; then
    AUDIO_C_SOURCES="$AUDIO_C_SOURCES
source/audioio/ffaudio/ffaudio/dsound.c
source/audioio/ffaudio/ffaudio/wasapi.c
"
elif [ "$PLATFORM" = "macos" ]; then
    AUDIO_C_SOURCES="$AUDIO_C_SOURCES
source/audioio/ffaudio/ffaudio/coreaudio.c
"
else
    AUDIO_C_SOURCES="$AUDIO_C_SOURCES
source/audioio/ffaudio/ffaudio/alsa.c
source/audioio/ffaudio/ffaudio/pulse.c
"
fi

# Build C++ sources
echo "Compiling C++ sources..."
OBJ_FILES=""
for src in $CPP_SOURCES; do
    obj="${src%.cc}.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$CLEAN" = "1" ]; then
        echo "  $src"
        $CXX $CXXFLAGS -c -o "$obj" "$src"
    fi
    OBJ_FILES="$OBJ_FILES $obj"
done

# Build ImGui sources
echo "Compiling ImGui..."
for src in $IMGUI_SOURCES; do
    obj="${src%.cpp}.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$CLEAN" = "1" ]; then
        echo "  $src"
        $CXX $CXXFLAGS -c -o "$obj" "$src"
    fi
    OBJ_FILES="$OBJ_FILES $obj"
done

# Build audio sources
echo "Compiling audio subsystem..."
mkdir -p source/audioio/ffaudio/ffaudio
for src in $AUDIO_C_SOURCES; do
    obj="${src%.c}.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$CLEAN" = "1" ]; then
        echo "  $src"
        if [[ "$src" == *audioio.c ]]; then
            # audioio.c includes C++ headers, must compile as C++
            $CXX $CXXFLAGS -c -o "$obj" "$src"
        elif [[ "$src" == *.cc ]]; then
            $CXX $CXXFLAGS -c -o "$obj" "$src"
        else
            $CC $CFLAGS -c -o "$obj" "$src"
        fi
    fi
done

# Create audioio.a
echo "Creating audioio.a..."
AUDIO_OBJ_FILES=""
for src in $AUDIO_C_SOURCES; do
    AUDIO_OBJ_FILES="$AUDIO_OBJ_FILES ${src%.c}.o"
done
ar rc source/audioio/audioio.a $AUDIO_OBJ_FILES

# Build compression library C sources (PPMd8, zstd, LZHUF)
echo "Compiling compression libraries..."
COMPRESS_OBJ_FILES=""
for src in $COMPRESSION_C_SOURCES; do
    obj="${src%.c}.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$CLEAN" = "1" ]; then
        echo "  $src"
        EXTRA_C=""
        if [[ "$src" == *lzhuf.c ]]; then
            EXTRA_C="-DLZHUF -DB2F"
        fi
        $CC $CFLAGS -Wno-extra -Wno-sign-compare -Wno-implicit-fallthrough $EXTRA_C -c -o "$obj" "$src"
    fi
    COMPRESS_OBJ_FILES="$COMPRESS_OBJ_FILES $obj"
done

# Build crypto C sources (monocypher, ML-KEM-768)
echo "Compiling crypto libraries..."
CRYPTO_OBJ_FILES=""
for src in $CRYPTO_C_SOURCES; do
    obj="${src%.c}.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$CLEAN" = "1" ]; then
        echo "  $src"
        EXTRA_C=""
        if [[ "$src" == *mlkem_native.c ]]; then
            EXTRA_C="-I./source/crypto/mlkem -DMLK_CONFIG_PARAMETER_SET=768"
        fi
        $CC $CFLAGS -Wno-extra -Wno-sign-compare $EXTRA_C -c -o "$obj" "$src"
    fi
    CRYPTO_OBJ_FILES="$CRYPTO_OBJ_FILES $obj"
done

# Link
echo "Linking $OUTPUT..."
$CXX -o "$OUTPUT" $OBJ_FILES $COMPRESS_OBJ_FILES $CRYPTO_OBJ_FILES source/audioio/audioio.a $LDFLAGS

echo "=== Build complete: $OUTPUT ==="
ls -la "$OUTPUT"

if [ "$MODE" = "release" ]; then
    echo ""
    if [ "$PLATFORM" = "windows" ]; then
        echo "*** REMINDER: Install to Program Files with:"
        echo "    cp $OUTPUT \"/c/Program Files/Mercury/\""
    else
        echo "*** REMINDER: Install with:"
        echo "    sudo install -m 755 $OUTPUT /usr/local/bin/mercury"
    fi
fi
