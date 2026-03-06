/*
 * Copyright (c) The mlkem-native project authors
 * SPDX-License-Identifier: Apache-2.0 OR ISC OR MIT
 */
#ifndef MLK_FIPS202_FIPS202_H
#define MLK_FIPS202_FIPS202_H

#include "../cbmc.h"
#include "../common.h"

#define SHAKE128_RATE 168
#define SHAKE256_RATE 136
#define SHA3_256_RATE 136
#define SHA3_384_RATE 104
#define SHA3_512_RATE 72

/* Context for non-incremental API */
typedef struct
{
  uint64_t ctx[25];
} MLK_ALIGN mlk_shake128ctx;

#define mlk_shake128_absorb_once MLK_NAMESPACE(shake128_absorb_once)
/*************************************************
 * Name:        mlk_shake128_absorb_once
 *
 * Description: One-shot absorb step of the SHAKE128 XOF.
 *
 *              For call-sites (in mlkem-native):
 *              - This function MUST ONLY be called straight after
 *                mlk_shake128_init().
 *              - This function MUST ONLY be called once.
 *
 *              Consequently, for providers of custom FIPS202 code
 *              to be used with mlkem-native:
 *              - You may assume that the input context is
 *                freshly initialized via mlk_shake128_init().
 *              - You may assume that this function is
 *                called exactly once.
 *
 * Arguments:   - mlk_shake128ctx *state:   pointer to SHAKE128 context
 *              - const uint8_t *input: pointer to input to be absorbed into
 *                                      the state
 *              - size_t inlen:         length of input in bytes
 **************************************************/
void mlk_shake128_absorb_once(mlk_shake128ctx *state, const uint8_t *input,
                              size_t inlen)
__contract__(
  requires(inlen <= MLK_MAX_BUFFER_SIZE)
  requires(memory_no_alias(state, sizeof(mlk_shake128ctx)))
  requires(memory_no_alias(input, inlen))
  assigns(memory_slice(state, sizeof(mlk_shake128ctx)))
);

#define mlk_shake128_squeezeblocks MLK_NAMESPACE(shake128_squeezeblocks)
/*************************************************
 * Name:        mlk_shake128_squeezeblocks
 *
 * Description: Squeeze step of SHAKE128 XOF. Squeezes full blocks of
 *              SHAKE128_RATE bytes each. Modifies the state. Can be called
 *              multiple times to keep squeezing, i.e., is incremental.
 *
 * Arguments:   - uint8_t *output:     pointer to output blocks
 *              - size_t nblocks:      number of blocks to be squeezed (written
 *                                     to output)
 *              - mlk_shake128ctx *state:  pointer to in/output Keccak state
 **************************************************/
void mlk_shake128_squeezeblocks(uint8_t *output, size_t nblocks,
                                mlk_shake128ctx *state)
__contract__(
  requires(nblocks <= 8 /* somewhat arbitrary bound */)
  requires(memory_no_alias(state, sizeof(mlk_shake128ctx)))
  requires(memory_no_alias(output, nblocks * SHAKE128_RATE))
  assigns(memory_slice(output, nblocks * SHAKE128_RATE), memory_slice(state, sizeof(mlk_shake128ctx)))
);

#define mlk_shake128_init MLK_NAMESPACE(shake128_init)
void mlk_shake128_init(mlk_shake128ctx *state);

#define mlk_shake128_release MLK_NAMESPACE(shake128_release)
void mlk_shake128_release(mlk_shake128ctx *state);

/* One-stop SHAKE256 call. Aliasing between input and
 * output is not permitted */
#define mlk_shake256 MLK_NAMESPACE(shake256)
/*************************************************
 * Name:        mlk_shake256
 *
 * Description: SHAKE256 XOF with non-incremental API
 *
 * Arguments:   - uint8_t *output:      pointer to output
 *              - size_t outlen:        requested output length in bytes
 *              - const uint8_t *input: pointer to input
 *              - size_t inlen:         length of input in bytes
 **************************************************/
void mlk_shake256(uint8_t *output, size_t outlen, const uint8_t *input,
                  size_t inlen)
__contract__(
  requires(inlen <= MLK_MAX_BUFFER_SIZE)
  requires(outlen <= MLK_MAX_BUFFER_SIZE)
  requires(memory_no_alias(input, inlen))
  requires(memory_no_alias(output, outlen))
  assigns(memory_slice(output, outlen))
);

/* One-stop SHA3_256 call. Aliasing between input and
 * output is not permitted */
#define SHA3_256_HASHBYTES 32
#define mlk_sha3_256 MLK_NAMESPACE(sha3_256)
/*************************************************
 * Name:        mlk_sha3_256
 *
 * Description: SHA3-256 with non-incremental API
 *
 * Arguments:   - uint8_t *output:      pointer to output
 *              - const uint8_t *input: pointer to input
 *              - size_t inlen:         length of input in bytes
 **************************************************/
void mlk_sha3_256(uint8_t *output, const uint8_t *input, size_t inlen)
__contract__(
  requires(inlen <= MLK_MAX_BUFFER_SIZE)
  requires(memory_no_alias(input, inlen))
  requires(memory_no_alias(output, SHA3_256_HASHBYTES))
  assigns(memory_slice(output, SHA3_256_HASHBYTES))
);

/* One-stop SHA3_512 call. Aliasing between input and
 * output is not permitted */
#define SHA3_512_HASHBYTES 64
#define mlk_sha3_512 MLK_NAMESPACE(sha3_512)
/*************************************************
 * Name:        mlk_sha3_512
 *
 * Description: SHA3-512 with non-incremental API
 *
 * Arguments:   - uint8_t *output:      pointer to output
 *              - const uint8_t *input: pointer to input
 *              - size_t inlen:         length of input in bytes
 **************************************************/
void mlk_sha3_512(uint8_t *output, const uint8_t *input, size_t inlen)
__contract__(
  requires(inlen <= MLK_MAX_BUFFER_SIZE)
  requires(memory_no_alias(input, inlen))
  requires(memory_no_alias(output, SHA3_512_HASHBYTES))
  assigns(memory_slice(output, SHA3_512_HASHBYTES))
);

#if !defined(MLK_CONFIG_USE_NATIVE_BACKEND_FIPS202) || \
    !defined(MLK_USE_FIPS202_X4_NATIVE)
/* If you provide your own FIPS-202 implementation where the x4-
 * Keccak-f1600-x4 implementation falls back to 4-fold Keccak-f1600,
 * set this to gain a small speedup. */
#define FIPS202_X4_DEFAULT_IMPLEMENTATION
#endif /* !MLK_CONFIG_USE_NATIVE_BACKEND_FIPS202 || !MLK_USE_FIPS202_X4_NATIVE \
        */


#endif /* !MLK_FIPS202_FIPS202_H */
