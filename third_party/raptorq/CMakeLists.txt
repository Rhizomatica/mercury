add_library(raptorq STATIC)

target_sources(raptorq PRIVATE
    lib/chooser.c
    lib/io.c
    lib/nanorq.c
    lib/nanorq_core.c
    lib/ops.c
    lib/params.c
    lib/partition.c
    lib/precode.c
    lib/rand.c
    lib/sopi.c
    lib/tuple.c
    lib/uvec.c
)

target_include_directories(raptorq PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_sources(raptorq PRIVATE
    deps/obl/gf2_8_affine_mat.h
    deps/obl/gf2_8_tables.h
    deps/obl/oblas_common.c
    deps/obl/oblas_common.h
    deps/obl/oblas_lite.c
    deps/obl/oblas_lite.h
)

target_include_directories(raptorq PRIVATE deps)