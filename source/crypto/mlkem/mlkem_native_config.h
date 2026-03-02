/*
 * Copyright (c) The mlkem-native project authors
 * SPDX-License-Identifier: Apache-2.0 OR ISC OR MIT
 */

/* References
 * ==========
 *
 * - [FIPS140_3_IG]
 *   Implementation Guidance for FIPS 140-3 and the Cryptographic Module
 *   Validation Program
 *   National Institute of Standards and Technology
 *   https://csrc.nist.gov/projects/cryptographic-module-validation-program/fips-140-3-ig-announcements
 *
 * - [FIPS203]
 *   FIPS 203 Module-Lattice-Based Key-Encapsulation Mechanism Standard
 *   National Institute of Standards and Technology
 *   https://csrc.nist.gov/pubs/fips/203/final
 */

#ifndef MLK_CONFIG_H
#define MLK_CONFIG_H

/******************************************************************************
 * Name:        MLK_CONFIG_PARAMETER_SET
 *
 * Description: Specifies the parameter set for ML-KEM
 *              - MLK_CONFIG_PARAMETER_SET=512 corresponds to ML-KEM-512
 *              - MLK_CONFIG_PARAMETER_SET=768 corresponds to ML-KEM-768
 *              - MLK_CONFIG_PARAMETER_SET=1024 corresponds to ML-KEM-1024
 *
 *              If you want to support multiple parameter sets, build the
 *              library multiple times and set MLK_CONFIG_MULTILEVEL_BUILD.
 *              See MLK_CONFIG_MULTILEVEL_BUILD for how to do this while
 *              minimizing code duplication.
 *
 *              This can also be set using CFLAGS.
 *
 *****************************************************************************/
#ifndef MLK_CONFIG_PARAMETER_SET
#define MLK_CONFIG_PARAMETER_SET \
  768 /* Change this for different security strengths */
#endif

/******************************************************************************
 * Name:        MLK_CONFIG_FILE
 *
 * Description: If defined, this is a header that will be included instead
 *              of the default configuration file mlkem/mlkem_native_config.h.
 *
 *              When you need to build mlkem-native in multiple configurations,
 *              using varying MLK_CONFIG_FILE can be more convenient
 *              then configuring everything through CFLAGS.
 *
 *              To use, MLK_CONFIG_FILE _must_ be defined prior
 *              to the inclusion of any mlkem-native headers. For example,
 *              it can be set by passing `-DMLK_CONFIG_FILE="..."`
 *              on the command line.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_FILE "mlkem_native_config.h" */

/******************************************************************************
 * Name:        MLK_CONFIG_NAMESPACE_PREFIX
 *
 * Description: The prefix to use to namespace global symbols from mlkem/.
 *
 *              In a multi-level build, level-dependent symbols will
 *              additionally be prefixed with the parameter set (512/768/1024).
 *
 *              This can also be set using CFLAGS.
 *
 *****************************************************************************/
#if !defined(MLK_CONFIG_NAMESPACE_PREFIX)
#define MLK_CONFIG_NAMESPACE_PREFIX MLK_DEFAULT_NAMESPACE_PREFIX
#endif

/******************************************************************************
 * Name:        MLK_CONFIG_MULTILEVEL_BUILD
 *
 * Description: Set this if the build is part of a multi-level build supporting
 *              multiple parameter sets.
 *
 *              If you need only a single parameter set, keep this unset.
 *
 *              To build mlkem-native with support for all parameter sets,
 *              build it three times -- once per parameter set -- and set the
 *              option MLK_CONFIG_MULTILEVEL_WITH_SHARED for exactly one of
 *              them, and MLK_CONFIG_MULTILEVEL_NO_SHARED for the others.
 *              MLK_CONFIG_MULTILEVEL_BUILD should be set for all of them.
 *
 *              See examples/multilevel_build for an example.
 *
 *              This can also be set using CFLAGS.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_MULTILEVEL_BUILD */

/******************************************************************************
 * Name:        MLK_CONFIG_EXTERNAL_API_QUALIFIER
 *
 * Description: If set, this option provides an additional function
 *              qualifier to be added to declarations of mlkem-native's
 *              public API.
 *
 *              The primary use case for this option are single-CU builds
 *              where the public API exposed by mlkem-native is wrapped by
 *              another API in the consuming application. In this case,
 *              even mlkem-native's public API can be marked `static`.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_EXTERNAL_API_QUALIFIER */

/******************************************************************************
 * Name:        MLK_CONFIG_NO_RANDOMIZED_API
 *
 * Description: If this option is set, mlkem-native will be built without the
 *              randomized API functions (crypto_kem_keypair and
 *              crypto_kem_enc).
 *              This allows users to build mlkem-native without providing a
 *              randombytes() implementation if they only need the
 *              deterministic API
 *              (crypto_kem_keypair_derand, crypto_kem_enc_derand,
 *              crypto_kem_dec).
 *
 *              NOTE: This option is incompatible with MLK_CONFIG_KEYGEN_PCT
 *              as the current PCT implementation requires crypto_kem_enc().
 *
 *****************************************************************************/
/* #define MLK_CONFIG_NO_RANDOMIZED_API */

/******************************************************************************
 * Name:        MLK_CONFIG_NO_SUPERCOP
 *
 * Description: By default, mlkem_native.h exposes the mlkem-native API in the
 *              SUPERCOP naming convention (crypto_kem_xxx). If you don't need
 *              this, set MLK_CONFIG_NO_SUPERCOP.
 *
 *              NOTE: You must set this for a multi-level build as the SUPERCOP
 *              naming does not disambiguate between the parameter sets.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_NO_SUPERCOP */

/******************************************************************************
 * Name:        MLK_CONFIG_CONSTANTS_ONLY
 *
 * Description: If you only need the size constants (MLKEM_PUBLICKEYBYTES, etc.)
 *              but no function declarations, set MLK_CONFIG_CONSTANTS_ONLY.
 *
 *              This only affects the public header mlkem_native.h, not
 *              the implementation.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_CONSTANTS_ONLY */

/******************************************************************************
 *
 * Build-only configuration options
 *
 * The remaining configurations are build-options only.
 * They do not affect the API described in mlkem_native.h.
 *
 *****************************************************************************/

#if defined(MLK_BUILD_INTERNAL)
/******************************************************************************
 * Name:        MLK_CONFIG_MULTILEVEL_WITH_SHARED
 *
 * Description: This is for multi-level builds of mlkem-native only. If you
 *              need only a single parameter set, keep this unset.
 *
 *              If this is set, all MLK_CONFIG_PARAMETER_SET-independent
 *              code will be included in the build, including code needed only
 *              for other parameter sets.
 *
 *              Example: mlk_poly_cbd3 is only needed for
 *              MLK_CONFIG_PARAMETER_SET == 512. Yet, if this option is set
 *              for a build with MLK_CONFIG_PARAMETER_SET == 768/1024, it
 *              would be included.
 *
 *              To build mlkem-native with support for all parameter sets,
 *              build it three times -- once per parameter set -- and set the
 *              option MLK_CONFIG_MULTILEVEL_WITH_SHARED for exactly one of
 *              them, and MLK_CONFIG_MULTILEVEL_NO_SHARED for the others.
 *              MLK_CONFIG_MULTILEVEL_BUILD should be set for all of them.
 *
 *              See examples/multilevel_build for an example.
 *
 *              This can also be set using CFLAGS.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_MULTILEVEL_WITH_SHARED */

/******************************************************************************
 * Name:        MLK_CONFIG_MULTILEVEL_NO_SHARED
 *
 * Description: This is for multi-level builds of mlkem-native only. If you
 *              need only a single parameter set, keep this unset.
 *
 *              If this is set, no MLK_CONFIG_PARAMETER_SET-independent code
 *              will be included in the build.
 *
 *              To build mlkem-native with support for all parameter sets,
 *              build it three times -- once per parameter set -- and set the
 *              option MLK_CONFIG_MULTILEVEL_WITH_SHARED for exactly one of
 *              them, and MLK_CONFIG_MULTILEVEL_NO_SHARED for the others.
 *              MLK_CONFIG_MULTILEVEL_BUILD should be set for all of them.
 *
 *              See examples/multilevel_build for an example.
 *
 *              This can also be set using CFLAGS.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_MULTILEVEL_NO_SHARED */

/******************************************************************************
 * Name:        MLK_CONFIG_MONOBUILD_KEEP_SHARED_HEADERS
 *
 * Description: This is only relevant for single compilation unit (SCU)
 *              builds of mlkem-native. In this case, it determines whether
 *              directives defined in parameter-set-independent headers should
 *              be #undef'ined or not at the of the SCU file. This is needed
 *              in multilevel builds.
 *
 *              See examples/multilevel_build_native for an example.
 *
 *              This can also be set using CFLAGS.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_MONOBUILD_KEEP_SHARED_HEADERS */

/******************************************************************************
 * Name:        MLK_CONFIG_USE_NATIVE_BACKEND_ARITH
 *
 * Description: Determines whether an native arithmetic backend should be used.
 *
 *              The arithmetic backend covers performance critical functions
 *              such as the number-theoretic transform (NTT).
 *
 *              If this option is unset, the C backend will be used.
 *
 *              If this option is set, the arithmetic backend to be use is
 *              determined by MLK_CONFIG_ARITH_BACKEND_FILE: If the latter is
 *              unset, the default backend for your the target architecture
 *              will be used. If set, it must be the name of a backend metadata
 *              file.
 *
 *              This can also be set using CFLAGS.
 *
 *****************************************************************************/
#if !defined(MLK_CONFIG_USE_NATIVE_BACKEND_ARITH)
/* #define MLK_CONFIG_USE_NATIVE_BACKEND_ARITH */
#endif

/******************************************************************************
 * Name:        MLK_CONFIG_ARITH_BACKEND_FILE
 *
 * Description: The arithmetic backend to use.
 *
 *              If MLK_CONFIG_USE_NATIVE_BACKEND_ARITH is unset, this option
 *              is ignored.
 *
 *              If MLK_CONFIG_USE_NATIVE_BACKEND_ARITH is set, this option must
 *              either be undefined or the filename of an arithmetic backend.
 *              If unset, the default backend will be used.
 *
 *              This can be set using CFLAGS.
 *
 *****************************************************************************/
#if defined(MLK_CONFIG_USE_NATIVE_BACKEND_ARITH) && \
    !defined(MLK_CONFIG_ARITH_BACKEND_FILE)
#define MLK_CONFIG_ARITH_BACKEND_FILE "native/meta.h"
#endif

/******************************************************************************
 * Name:        MLK_CONFIG_USE_NATIVE_BACKEND_FIPS202
 *
 * Description: Determines whether an native FIPS202 backend should be used.
 *
 *              The FIPS202 backend covers 1x/2x/4x-fold Keccak-f1600, which is
 *              the performance bottleneck of SHA3 and SHAKE.
 *
 *              If this option is unset, the C backend will be used.
 *
 *              If this option is set, the FIPS202 backend to be use is
 *              determined by MLK_CONFIG_FIPS202_BACKEND_FILE: If the latter is
 *              unset, the default backend for your the target architecture
 *              will be used. If set, it must be the name of a backend metadata
 *              file.
 *
 *              This can also be set using CFLAGS.
 *
 *****************************************************************************/
#if !defined(MLK_CONFIG_USE_NATIVE_BACKEND_FIPS202)
/* #define MLK_CONFIG_USE_NATIVE_BACKEND_FIPS202 */
#endif

/******************************************************************************
 * Name:        MLK_CONFIG_FIPS202_BACKEND_FILE
 *
 * Description: The FIPS-202 backend to use.
 *
 *              If MLK_CONFIG_USE_NATIVE_BACKEND_FIPS202 is set, this option
 *              must either be undefined or the filename of a FIPS202 backend.
 *              If unset, the default backend will be used.
 *
 *              This can be set using CFLAGS.
 *
 *****************************************************************************/
#if defined(MLK_CONFIG_USE_NATIVE_BACKEND_FIPS202) && \
    !defined(MLK_CONFIG_FIPS202_BACKEND_FILE)
#define MLK_CONFIG_FIPS202_BACKEND_FILE "fips202/native/auto.h"
#endif

/******************************************************************************
 * Name:        MLK_CONFIG_FIPS202_CUSTOM_HEADER
 *
 * Description: Custom header to use for FIPS-202
 *
 *              This should only be set if you intend to use a custom
 *              FIPS-202 implementation, different from the one shipped
 *              with mlkem-native.
 *
 *              If set, it must be the name of a file serving as the
 *              replacement for mlkem/fips202/fips202.h, and exposing
 *              the same API (see FIPS202.md).
 *
 *****************************************************************************/
/* #define MLK_CONFIG_FIPS202_CUSTOM_HEADER "SOME_FILE.h" */

/******************************************************************************
 * Name:        MLK_CONFIG_FIPS202X4_CUSTOM_HEADER
 *
 * Description: Custom header to use for FIPS-202-X4
 *
 *              This should only be set if you intend to use a custom
 *              FIPS-202 implementation, different from the one shipped
 *              with mlkem-native.
 *
 *              If set, it must be the name of a file serving as the
 *              replacement for mlkem/fips202/fips202x4.h, and exposing
 *              the same API (see FIPS202.md).
 *
 *****************************************************************************/
/* #define MLK_CONFIG_FIPS202X4_CUSTOM_HEADER "SOME_FILE.h" */

/******************************************************************************
 * Name:        MLK_CONFIG_CUSTOM_ZEROIZE
 *
 * Description: In compliance with @[FIPS203, Section 3.3], mlkem-native
 *              zeroizes intermediate stack buffers before returning from
 *              function calls.
 *
 *              Set this option and define `mlk_zeroize` if you want to
 *              use a custom method to zeroize intermediate stack buffers.
 *              The default implementation uses SecureZeroMemory on Windows
 *              and a memset + compiler barrier otherwise. If neither of those
 *              is available on the target platform, compilation will fail,
 *              and you will need to use MLK_CONFIG_CUSTOM_ZEROIZE to provide
 *              a custom implementation of `mlk_zeroize()`.
 *
 *              WARNING:
 *              The explicit stack zeroization conducted by mlkem-native
 *              reduces the likelihood of data leaking on the stack, but
 *              does not eliminate it! The C standard makes no guarantee about
 *              where a compiler allocates structures and whether/where it makes
 *              copies of them. Also, in addition to entire structures, there
 *              may also be potentially exploitable leakage of individual values
 *              on the stack.
 *
 *              If you need bullet-proof zeroization of the stack, you need to
 *              consider additional measures instead of of what this feature
 *              provides. In this case, you can set mlk_zeroize to a no-op.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_CUSTOM_ZEROIZE
   #if !defined(__ASSEMBLER__)
   #include <stdint.h>
   #include "src/sys.h"
   static MLK_INLINE void mlk_zeroize(void *ptr, size_t len)
   {
       ... your implementation ...
   }
   #endif
*/

/******************************************************************************
 * Name:        MLK_CONFIG_CUSTOM_RANDOMBYTES
 *
 * Description: mlkem-native does not provide a secure randombytes
 *              implementation. Such an implementation has to be provided by the
 *              consumer.
 *
 *              If this option is not set, mlkem-native expects a function
 *              int randombytes(uint8_t *out, size_t outlen).
 *              It is expected to return zero on success, and non-zero on
 *              failure. In case of failure, the top level APIs will return a
 *              MLK_ERR_RNG_FAIL error code.
 *
 *              Set this option and define `mlk_randombytes` (with the same
 *              signature and behaviour) if you want to use a custom method to
 *              sample randombytes with a different name or signature.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_CUSTOM_RANDOMBYTES
   #if !defined(__ASSEMBLER__)
   #include <stdint.h>
   #include "src/sys.h"
   static MLK_INLINE int mlk_randombytes(uint8_t *ptr, size_t len)
   {
       ... your implementation ...
       return 0;
   }
   #endif
*/

/******************************************************************************
 * Name:        MLK_CONFIG_CUSTOM_CAPABILITY_FUNC
 *
 * Description: mlkem-native backends may rely on specific hardware features.
 *              Those backends will only be included in an mlkem-native build
 *              if support for the respective features is enabled at
 *              compile-time. However, when building for a heteroneous set
 *              of CPUs to run the resulting binary/library on, feature
 *              detection at _runtime_ is needed to decided whether a backend
 *              can be used or not.
 *
 *              Set this option and define `mlk_sys_check_capability` if you
 *              want to use a custom method to dispatch between implementations.
 *
 *              If this option is not set, mlkem-native uses compile-time
 *              feature detection only to decide which backend to use.
 *
 *              If you compile mlkem-native on a system with different
 *              capabilities than the system that the resulting binary/library
 *              will be run on, you must use this option.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_CUSTOM_CAPABILITY_FUNC
   static MLK_INLINE int mlk_sys_check_capability(mlk_sys_cap cap)
   __contract__(
     ensures(return_value == 0 || return_value == 1)
   )
   {
       ... your implementation ...
   }
*/

/******************************************************************************
 * Name:        MLK_CONFIG_CUSTOM_ALLOC_FREE [EXPERIMENTAL]
 *
 * Description: Set this option and define `MLK_CUSTOM_ALLOC` and
 *              `MLK_CUSTOM_FREE` if you want to use custom allocation for
 *              large local structures or buffers.
 *
 *              By default, all buffers/structures are allocated on the stack.
 *              If this option is set, most of them will be allocated via
 *              MLK_CUSTOM_ALLOC.
 *
 *              Parameters to MLK_CUSTOM_ALLOC:
 *              - T* v: Target pointer to declare.
 *              - T: Type of structure to be allocated
 *              - N: Number of elements to be allocated.
 *
 *              Parameters to MLK_CUSTOM_FREE:
 *              - T* v: Target pointer to free. May be NULL.
 *              - T: Type of structure to be freed.
 *              - N: Number of elements to be freed.
 *
 *              WARNING: This option is experimental!
 *              Its scope, configuration and function/macro signatures may
 *              change at any time. We expect a stable API for v2.
 *
 *              NOTE: Even if this option is set, some allocations further down
 *              the call stack will still be made from the stack, consuming up
 *              to 3KB of stack space. Those will likely be added to the scope
 *              of this option in the future.
 *
 *              NOTE: MLK_CUSTOM_ALLOC need not guarantee a successful
 *              allocation nor include error handling. Upon failure, the
 *              target pointer should simply be set to NULL. The calling
 *              code will handle this case and invoke MLK_CUSTOM_FREE.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_CUSTOM_ALLOC_FREE
   #if !defined(__ASSEMBLER__)
   #include <stdlib.h>
   #define MLK_CUSTOM_ALLOC(v, T, N)                              \
     T* (v) = (T *)aligned_alloc(MLK_DEFAULT_ALIGN,               \
                                 MLK_ALIGN_UP(sizeof(T) * (N)))
   #define MLK_CUSTOM_FREE(v, T, N) free(v)
   #endif
*/

/******************************************************************************
 * Name:        MLK_CONFIG_CUSTOM_MEMCPY
 *
 * Description: Set this option and define `mlk_memcpy` if you want to
 *              use a custom method to copy memory instead of the standard
 *              library memcpy function.
 *
 *              The custom implementation must have the same signature and
 *              behavior as the standard memcpy function:
 *              void *mlk_memcpy(void *dest, const void *src, size_t n)
 *
 *****************************************************************************/
/* #define MLK_CONFIG_CUSTOM_MEMCPY
   #if !defined(__ASSEMBLER__)
   #include <stdint.h>
   #include "src/sys.h"
   static MLK_INLINE void *mlk_memcpy(void *dest, const void *src, size_t n)
   {
       ... your implementation ...
   }
   #endif
*/

/******************************************************************************
 * Name:        MLK_CONFIG_CUSTOM_MEMSET
 *
 * Description: Set this option and define `mlk_memset` if you want to
 *              use a custom method to set memory instead of the standard
 *              library memset function.
 *
 *              The custom implementation must have the same signature and
 *              behavior as the standard memset function:
 *              void *mlk_memset(void *s, int c, size_t n)
 *
 *****************************************************************************/
/* #define MLK_CONFIG_CUSTOM_MEMSET
   #if !defined(__ASSEMBLER__)
   #include <stdint.h>
   #include "src/sys.h"
   static MLK_INLINE void *mlk_memset(void *s, int c, size_t n)
   {
       ... your implementation ...
   }
   #endif
*/

/******************************************************************************
 * Name:        MLK_CONFIG_INTERNAL_API_QUALIFIER
 *
 * Description: If set, this option provides an additional function
 *              qualifier to be added to declarations of internal API.
 *
 *              The primary use case for this option are single-CU builds,
 *              in which case this option can be set to `static`.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_INTERNAL_API_QUALIFIER */

/******************************************************************************
 * Name:        MLK_CONFIG_CT_TESTING_ENABLED
 *
 * Description: If set, mlkem-native annotates data as secret / public using
 *              valgrind's annotations VALGRIND_MAKE_MEM_UNDEFINED and
 *              VALGRIND_MAKE_MEM_DEFINED, enabling various checks for secret-
 *              dependent control flow of variable time execution (depending
 *              on the exact version of valgrind installed).
 *
 *****************************************************************************/
/* #define MLK_CONFIG_CT_TESTING_ENABLED */

/******************************************************************************
 * Name:        MLK_CONFIG_NO_ASM
 *
 * Description: If this option is set, mlkem-native will be built without
 *              use of native code or inline assembly.
 *
 *              By default, inline assembly is used to implement value barriers.
 *              Without inline assembly, mlkem-native will use a global volatile
 *              'opt blocker' instead; see verify.h.
 *
 *              Inline assembly is also used to implement a secure zeroization
 *              function on non-Windows platforms. If this option is set and
 *              the target platform is not Windows, you MUST set
 *              MLK_CONFIG_CUSTOM_ZEROIZE and provide a custom zeroization
 *              function.
 *
 *              If this option is set, MLK_CONFIG_USE_NATIVE_BACKEND_FIPS202 and
 *              and MLK_CONFIG_USE_NATIVE_BACKEND_ARITH will be ignored, and no
 *              native backends will be used.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_NO_ASM */

/******************************************************************************
 * Name:        MLK_CONFIG_NO_ASM_VALUE_BARRIER
 *
 * Description: If this option is set, mlkem-native will be built without
 *              use of native code or inline assembly for value barriers.
 *
 *              By default, inline assembly (if available) is used to implement
 *              value barriers.
 *              Without inline assembly, mlkem-native will use a global volatile
 *              'opt blocker' instead; see verify.h.
 *
 *****************************************************************************/
/* #define MLk_CONFIG_NO_ASM_VALUE_BARRIER */

/******************************************************************************
 * Name:        MLK_CONFIG_KEYGEN_PCT
 *
 * Description: Compliance with @[FIPS140_3_IG, p.87] requires a
 *              Pairwise Consistency Test (PCT) to be carried out on a freshly
 *              generated keypair before it can be exported.
 *
 *              Set this option if such a check should be implemented.
 *              In this case, crypto_kem_keypair_derand and crypto_kem_keypair
 *              will return a non-zero error code if the PCT failed.
 *
 *              NOTE: This feature will drastically lower the performance of
 *              key generation.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_KEYGEN_PCT */

/******************************************************************************
 * Name:        MLK_CONFIG_KEYGEN_PCT_BREAKAGE_TEST
 *
 * Description: If this option is set, the user must provide a runtime
 *              function `static inline int mlk_break_pct() { ... }` to
 *              indicate whether the PCT should be made fail.
 *
 *              This option only has an effect if MLK_CONFIG_KEYGEN_PCT is set.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_KEYGEN_PCT_BREAKAGE_TEST
   #if !defined(__ASSEMBLER__)
   #include "src/sys.h"
   static MLK_INLINE int mlk_break_pct(void)
   {
       ... return 0/1 depending on whether PCT should be broken ...
   }
   #endif
*/

/******************************************************************************
 * Name:        MLK_CONFIG_SERIAL_FIPS202_ONLY
 *
 * Description: Set this to use a FIPS202 implementation with global state
 *              that supports only one active Keccak computation at a time
 *              (e.g. some hardware accelerators).
 *
 *              If this option is set, batched Keccak operations are
 *              disabled for rejection sampling during matrix generation.
 *              Instead, matrix entries will be generated one at a time.
 *
 *              This allows offloading Keccak computations to a hardware
 *              accelerator that holds only a single Keccak state locally,
 *              rather than requiring support for batched (4x) Keccak states.
 *
 *              NOTE: Depending on the target CPU, disabling batched Keccak
 *              may reduce performance when using software FIPS202
 *              implementations. Only enable this when you have to.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_SERIAL_FIPS202_ONLY */

/******************************************************************************
 * Name:        MLK_CONFIG_CONTEXT_PARAMETER
 *
 * Description: Set this to add a context parameter that is provided to public
 *              API functions and is then available in custom callbacks.
 *
 *              The type of the context parameter is configured via
 *              MLK_CONFIG_CONTEXT_PARAMETER_TYPE.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_CONTEXT_PARAMETER */

/******************************************************************************
 * Name:        MLK_CONFIG_CONTEXT_PARAMETER_TYPE
 *
 * Description: Set this to define the type for the context parameter used by
 *              MLK_CONFIG_CONTEXT_PARAMETER.
 *
 *              This is only relevant if MLK_CONFIG_CONTEXT_PARAMETER is set.
 *
 *****************************************************************************/
/* #define MLK_CONFIG_CONTEXT_PARAMETER_TYPE void* */

/*************************  Config internals  ********************************/

#endif /* MLK_BUILD_INTERNAL */

/* Default namespace
 *
 * Don't change this. If you need a different namespace, re-define
 * MLK_CONFIG_NAMESPACE_PREFIX above instead, and remove the following.
 *
 * The default MLKEM namespace is
 *
 *   PQCP_MLKEM_NATIVE_MLKEM<LEVEL>_
 *
 * e.g., PQCP_MLKEM_NATIVE_MLKEM512_
 */

#if MLK_CONFIG_PARAMETER_SET == 512
#define MLK_DEFAULT_NAMESPACE_PREFIX PQCP_MLKEM_NATIVE_MLKEM512
#elif MLK_CONFIG_PARAMETER_SET == 768
#define MLK_DEFAULT_NAMESPACE_PREFIX PQCP_MLKEM_NATIVE_MLKEM768
#elif MLK_CONFIG_PARAMETER_SET == 1024
#define MLK_DEFAULT_NAMESPACE_PREFIX PQCP_MLKEM_NATIVE_MLKEM1024
#endif

#endif /* !MLK_CONFIG_H */
