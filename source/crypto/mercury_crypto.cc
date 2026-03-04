/*
 * Mercury post-quantum hybrid encryption suite — implementation.
 *
 * See include/crypto/mercury_crypto.h for API documentation.
 * See ENCRYPTION_PLAN.md for design rationale and security invariants.
 */

#include "../../include/crypto/mercury_crypto.h"
#include "monocypher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ML-KEM-768 API (C linkage from mlkem-native)
extern "C" {
#include "mlkem/mlkem_native.h"
}

// ---------------------------------------------------------------------------
// OS CSPRNG — Security Invariant #6
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
// Link: -lbcrypt
static int os_random(uint8_t* out, size_t len)
{
    NTSTATUS status = BCryptGenRandom(NULL, out, (ULONG)len,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return (status == 0) ? 0 : -1;  // STATUS_SUCCESS == 0
}
#else
#include <sys/random.h>
static int os_random(uint8_t* out, size_t len)
{
    ssize_t got = getrandom(out, len, 0);
    return (got == (ssize_t)len) ? 0 : -1;
}
#endif

// Provide randombytes() for mlkem-native (see randombytes.h)
extern "C" int randombytes(uint8_t* out, size_t outlen)
{
    return os_random(out, outlen);
}

// ---------------------------------------------------------------------------
// HKDF-Blake2b (Extract + Expand using monocypher's Blake2b)
// ---------------------------------------------------------------------------

// HKDF-Extract: PRK = Blake2b-keyed(salt, IKM)
static void hkdf_extract(const uint8_t* salt, size_t salt_len,
                          const uint8_t* ikm, size_t ikm_len,
                          uint8_t prk[32])
{
    crypto_blake2b_keyed(prk, 32, salt, salt_len, ikm, ikm_len);
}

// HKDF-Expand: OKM = Blake2b-keyed(PRK, info || counter)
// Single-round expansion (output <= 32 bytes = Blake2b output size)
static void hkdf_expand(const uint8_t prk[32],
                         const uint8_t* info, size_t info_len,
                         uint8_t okm[32])
{
    // T(1) = Blake2b-keyed(PRK, info || 0x01)
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, 32, prk, 32);
    crypto_blake2b_update(&ctx, info, info_len);
    uint8_t one = 0x01;
    crypto_blake2b_update(&ctx, &one, 1);
    crypto_blake2b_final(&ctx, okm);
    crypto_wipe(&ctx, sizeof(ctx));
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

cl_cipher_suite::cl_cipher_suite()
{
    memset(x25519_sk, 0, sizeof(x25519_sk));
    memset(x25519_pk, 0, sizeof(x25519_pk));
    memset(x25519_shared, 0, sizeof(x25519_shared));
    x25519_ready = false;

    mlkem_sk = nullptr;
    memset(mlkem_shared, 0, sizeof(mlkem_shared));
    mlkem_ready = false;

    memset(session_key, 0, sizeof(session_key));
    encryption_active = false;
    pq_active = false;
    kx_phase = KX_IDLE;
}

cl_cipher_suite::~cl_cipher_suite()
{
    wipe();
}

// ---------------------------------------------------------------------------
// Secure wipe — Security Invariant #2 (ephemeral keys, zero reuse)
// ---------------------------------------------------------------------------

void cl_cipher_suite::wipe()
{
    crypto_wipe(x25519_sk, sizeof(x25519_sk));
    crypto_wipe(x25519_pk, sizeof(x25519_pk));
    crypto_wipe(x25519_shared, sizeof(x25519_shared));
    x25519_ready = false;

    if (mlkem_sk)
    {
        crypto_wipe(mlkem_sk, MLKEM_SK_SIZE);
        free(mlkem_sk);
        mlkem_sk = nullptr;
    }
    crypto_wipe(mlkem_shared, sizeof(mlkem_shared));
    mlkem_ready = false;

    crypto_wipe(session_key, sizeof(session_key));
    encryption_active = false;
    pq_active = false;
    kx_phase = KX_IDLE;
}

// ---------------------------------------------------------------------------
// X25519 Key Exchange — Phase 1
// ---------------------------------------------------------------------------

int cl_cipher_suite::generate_x25519_keypair(uint8_t pubkey_out[X25519_KEY_SIZE])
{
    // Generate 32 random bytes as secret key — Security Invariant #6
    if (os_random(x25519_sk, 32) != 0)
    {
        printf("[CRYPTO] ERROR: RNG failure generating X25519 keypair\n");
        fflush(stdout);
        return -1;
    }

    // Derive public key from secret key
    crypto_x25519_public_key(x25519_pk, x25519_sk);
    memcpy(pubkey_out, x25519_pk, 32);

    printf("[CRYPTO] X25519 keypair generated\n");
    fflush(stdout);
    return 0;
}

int cl_cipher_suite::compute_x25519_shared(const uint8_t peer_pubkey[X25519_KEY_SIZE])
{
    crypto_x25519(x25519_shared, x25519_sk, peer_pubkey);

    // Check for all-zero shared secret (low-order point attack)
    uint8_t zero[32] = {0};
    if (memcmp(x25519_shared, zero, 32) == 0)
    {
        printf("[CRYPTO] ERROR: X25519 shared secret is zero (bad peer key)\n");
        fflush(stdout);
        crypto_wipe(x25519_shared, 32);
        return -1;
    }

    x25519_ready = true;
    printf("[CRYPTO] X25519 shared secret computed\n");
    fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------
// ML-KEM-768 Key Exchange — Phase 2
// ---------------------------------------------------------------------------

int cl_cipher_suite::generate_mlkem_keypair(uint8_t encaps_key_out[MLKEM_PK_SIZE])
{
    if (!mlkem_sk)
    {
        mlkem_sk = (uint8_t*)malloc(MLKEM_SK_SIZE);
        if (!mlkem_sk)
        {
            printf("[CRYPTO] ERROR: Failed to allocate ML-KEM secret key\n");
            fflush(stdout);
            return -1;
        }
    }

    uint8_t pk[MLKEM_PK_SIZE];
    int rc = crypto_kem_keypair(pk, mlkem_sk);
    if (rc != 0)
    {
        printf("[CRYPTO] ERROR: ML-KEM keypair generation failed (rc=%d)\n", rc);
        fflush(stdout);
        return -1;
    }

    memcpy(encaps_key_out, pk, MLKEM_PK_SIZE);
    printf("[CRYPTO] ML-KEM-768 keypair generated (pk=%d bytes, sk=%d bytes)\n",
           MLKEM_PK_SIZE, MLKEM_SK_SIZE);
    fflush(stdout);
    return 0;
}

int cl_cipher_suite::encapsulate_mlkem(const uint8_t encaps_key[MLKEM_PK_SIZE],
                                        uint8_t ciphertext_out[MLKEM_CT_SIZE])
{
    int rc = crypto_kem_enc(ciphertext_out, mlkem_shared, encaps_key);
    if (rc != 0)
    {
        printf("[CRYPTO] ERROR: ML-KEM encapsulation failed (rc=%d)\n", rc);
        fflush(stdout);
        return -1;
    }

    mlkem_ready = true;
    printf("[CRYPTO] ML-KEM-768 encapsulated (ct=%d bytes)\n", MLKEM_CT_SIZE);
    fflush(stdout);
    return 0;
}

int cl_cipher_suite::decapsulate_mlkem(const uint8_t ciphertext[MLKEM_CT_SIZE])
{
    if (!mlkem_sk)
    {
        printf("[CRYPTO] ERROR: No ML-KEM secret key (call generate_mlkem_keypair first)\n");
        fflush(stdout);
        return -1;
    }

    int rc = crypto_kem_dec(mlkem_shared, ciphertext, mlkem_sk);
    if (rc != 0)
    {
        printf("[CRYPTO] ERROR: ML-KEM decapsulation failed (rc=%d)\n", rc);
        fflush(stdout);
        return -1;
    }

    mlkem_ready = true;
    printf("[CRYPTO] ML-KEM-768 decapsulated\n");
    fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------
// Key Derivation — HKDF-Blake2b
// ---------------------------------------------------------------------------

void cl_cipher_suite::derive_session_key(const char* commander_call,
                                          const char* responder_call,
                                          const uint8_t* psk, int psk_len,
                                          bool mlkem_done)
{
    // IKM = x25519_shared || mlkem_shared (or just x25519_shared if classical-only)
    uint8_t ikm[64];
    int ikm_len;
    if (mlkem_done && mlkem_ready)
    {
        memcpy(ikm, x25519_shared, 32);
        memcpy(ikm + 32, mlkem_shared, 32);
        ikm_len = 64;
        pq_active = true;
    }
    else
    {
        memcpy(ikm, x25519_shared, 32);
        ikm_len = 32;
        pq_active = false;
    }

    // Salt = "mercury-v1" || [psk_hash] || commander_call || responder_call
    // If PSK is provided, hash it first (don't put raw PSK in salt)
    uint8_t salt[256];
    int salt_len = 0;
    const char* prefix = "mercury-v1";
    int prefix_len = (int)strlen(prefix);
    memcpy(salt, prefix, prefix_len);
    salt_len += prefix_len;

    if (psk && psk_len > 0)
    {
        uint8_t psk_hash[32];
        crypto_blake2b(psk_hash, 32, psk, psk_len);
        memcpy(salt + salt_len, psk_hash, 32);
        salt_len += 32;
        crypto_wipe(psk_hash, 32);
    }

    if (commander_call)
    {
        int cl = (int)strlen(commander_call);
        memcpy(salt + salt_len, commander_call, cl);
        salt_len += cl;
    }
    if (responder_call)
    {
        int cl = (int)strlen(responder_call);
        memcpy(salt + salt_len, responder_call, cl);
        salt_len += cl;
    }

    // Extract: PRK = HKDF-Extract(salt, IKM)
    uint8_t prk[32];
    hkdf_extract(salt, salt_len, ikm, ikm_len, prk);

    // Expand: session_key = HKDF-Expand(PRK, "data-encryption", 32)
    const char* info = "data-encryption";
    hkdf_expand(prk, (const uint8_t*)info, strlen(info), session_key);

    // Wipe intermediates
    crypto_wipe(ikm, sizeof(ikm));
    crypto_wipe(prk, sizeof(prk));
    crypto_wipe(salt, sizeof(salt));

    // Log a fingerprint derived from the session key (not the raw key bytes)
    {
        uint8_t fp[32];
        const char* fp_label = "mercury-fingerprint";
        crypto_blake2b_keyed(fp, 32, session_key, SESSION_KEY_SIZE,
                             (const uint8_t*)fp_label, strlen(fp_label));
        printf("[CRYPTO] Session key derived (%s), fingerprint: %02x%02x%02x%02x\n",
               pq_active ? "hybrid PQ" : "classical X25519",
               fp[0], fp[1], fp[2], fp[3]);
        crypto_wipe(fp, sizeof(fp));
    }
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Key Confirmation — PSK mismatch detection
// ---------------------------------------------------------------------------

void cl_cipher_suite::compute_key_confirmation(uint8_t tag_out[8])
{
    // BLAKE2b(session_key, "mercury-key-confirm") → 8 bytes
    const char* msg = "mercury-key-confirm";
    uint8_t full_hash[32];
    crypto_blake2b_keyed(full_hash, 32,
                         session_key, SESSION_KEY_SIZE,
                         (const uint8_t*)msg, strlen(msg));
    memcpy(tag_out, full_hash, 8);
    crypto_wipe(full_hash, sizeof(full_hash));
}

// ---------------------------------------------------------------------------
// Per-Batch Encrypt/Decrypt — ChaCha20-Poly1305 (IETF)
// ---------------------------------------------------------------------------

// Build deterministic nonce from direction + batch counter
static void build_nonce(uint8_t nonce[NONCE_SIZE],
                         uint32_t direction, uint64_t batch_counter)
{
    // nonce[0..3] = direction (big-endian)
    nonce[0] = (uint8_t)((direction >> 24) & 0xFF);
    nonce[1] = (uint8_t)((direction >> 16) & 0xFF);
    nonce[2] = (uint8_t)((direction >>  8) & 0xFF);
    nonce[3] = (uint8_t)((direction      ) & 0xFF);
    // nonce[4..11] = batch_counter (big-endian)
    nonce[4]  = (uint8_t)((batch_counter >> 56) & 0xFF);
    nonce[5]  = (uint8_t)((batch_counter >> 48) & 0xFF);
    nonce[6]  = (uint8_t)((batch_counter >> 40) & 0xFF);
    nonce[7]  = (uint8_t)((batch_counter >> 32) & 0xFF);
    nonce[8]  = (uint8_t)((batch_counter >> 24) & 0xFF);
    nonce[9]  = (uint8_t)((batch_counter >> 16) & 0xFF);
    nonce[10] = (uint8_t)((batch_counter >>  8) & 0xFF);
    nonce[11] = (uint8_t)((batch_counter      ) & 0xFF);
}

// ---------------------------------------------------------------------------
// Low-level AEAD helpers for truncated-tag support
// ---------------------------------------------------------------------------

// Compute the Poly1305 MAC for ChaCha20-Poly1305 AEAD (RFC 8439 §2.8).
// MAC = Poly1305(poly_key, pad16(aad) || pad16(ct) || len(aad) || len(ct))
// We have no AAD, so: pad16(ct) || 0u64 || len(ct) as u64 LE
static void aead_compute_mac(uint8_t mac[16],
                              const uint8_t poly_key[32],
                              const uint8_t* ct, int ct_len)
{
    // Build Poly1305 message: ct || padding || aad_len(0) || ct_len
    int padded_ct = ((ct_len + 15) / 16) * 16;
    int msg_len = padded_ct + 16;  // + 8 bytes aad_len + 8 bytes ct_len
    uint8_t* msg = (uint8_t*)calloc(1, msg_len);
    if (!msg) return;

    memcpy(msg, ct, ct_len);
    // Zero padding already from calloc
    // AAD length = 0 (bytes padded_ct..padded_ct+7 already zero)
    // CT length as u64 LE
    uint64_t ct_len64 = (uint64_t)ct_len;
    msg[padded_ct + 8]  = (uint8_t)(ct_len64       & 0xFF);
    msg[padded_ct + 9]  = (uint8_t)((ct_len64 >> 8) & 0xFF);
    msg[padded_ct + 10] = (uint8_t)((ct_len64 >> 16) & 0xFF);
    msg[padded_ct + 11] = (uint8_t)((ct_len64 >> 24) & 0xFF);
    msg[padded_ct + 12] = (uint8_t)((ct_len64 >> 32) & 0xFF);
    msg[padded_ct + 13] = (uint8_t)((ct_len64 >> 40) & 0xFF);
    msg[padded_ct + 14] = (uint8_t)((ct_len64 >> 48) & 0xFF);
    msg[padded_ct + 15] = (uint8_t)((ct_len64 >> 56) & 0xFF);

    crypto_poly1305(mac, msg, msg_len, poly_key);

    crypto_wipe(msg, msg_len);
    free(msg);
}

// ---------------------------------------------------------------------------
// Encrypt
// ---------------------------------------------------------------------------

int cl_cipher_suite::encrypt(const uint8_t* in, int in_len,
                              uint8_t* out, int out_capacity,
                              uint64_t batch_counter, uint32_t direction,
                              int tag_size)
{
    if (!encryption_active || in_len <= 0)
        return -1;
    if (in_len + tag_size > out_capacity)
        return -1;

    uint8_t nonce[NONCE_SIZE];
    build_nonce(nonce, direction, batch_counter);

    // Always use the streaming AEAD API (computes full 16-byte MAC)
    uint8_t full_mac[16];
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, session_key, nonce);
    crypto_aead_write(&ctx, out, full_mac, NULL, 0, in, in_len);
    crypto_wipe(&ctx, sizeof(ctx));

    // Append auth tag (truncated if ROBUST)
    memcpy(out + in_len, full_mac, tag_size);
    crypto_wipe(full_mac, sizeof(full_mac));

    return in_len + tag_size;
}

// ---------------------------------------------------------------------------
// Decrypt
// ---------------------------------------------------------------------------

int cl_cipher_suite::decrypt(const uint8_t* in, int in_len,
                              uint8_t* out, int out_capacity,
                              uint64_t batch_counter, uint32_t direction,
                              int tag_size)
{
    if (!encryption_active || in_len <= tag_size)
        return -1;

    int plain_len = in_len - tag_size;
    if (plain_len > out_capacity)
        return -1;

    uint8_t nonce[NONCE_SIZE];
    build_nonce(nonce, direction, batch_counter);

    const uint8_t* ciphertext = in;
    const uint8_t* received_tag = in + plain_len;

    if (tag_size == AUTH_TAG_SIZE)
    {
        // Full 16-byte tag — use crypto_aead_read (constant-time verify + decrypt)
        crypto_aead_ctx ctx;
        crypto_aead_init_ietf(&ctx, session_key, nonce);
        int rc = crypto_aead_read(&ctx, out, received_tag, NULL, 0,
                                   ciphertext, plain_len);
        crypto_wipe(&ctx, sizeof(ctx));
        if (rc != 0)
        {
            crypto_wipe(out, plain_len);
            return -1;
        }
    }
    else
    {
        // Truncated tag — use low-level ChaCha20 + Poly1305.
        // 1. Derive Poly1305 key from ChaCha20 block 0
        // 2. Compute expected MAC over ciphertext (RFC 8439 construction)
        // 3. Verify truncated portion
        // 4. Decrypt with ChaCha20 (counter=1)

        // Block 0: encrypt 64 zero bytes at counter=0 → keystream → Poly1305 key
        uint8_t block0[64] = {0};
        crypto_chacha20_ietf(block0, block0, 64, session_key, nonce, 0);
        uint8_t poly_key[32];
        memcpy(poly_key, block0, 32);
        crypto_wipe(block0, sizeof(block0));

        // Compute expected full MAC over ciphertext
        uint8_t expected_mac[16];
        aead_compute_mac(expected_mac, poly_key, ciphertext, plain_len);
        crypto_wipe(poly_key, sizeof(poly_key));

        // Constant-time comparison of truncated tag
        uint8_t diff = 0;
        for (int i = 0; i < tag_size; i++)
            diff |= expected_mac[i] ^ received_tag[i];
        crypto_wipe(expected_mac, sizeof(expected_mac));

        if (diff != 0)
        {
            crypto_wipe(out, plain_len);
            return -1;  // Auth failure
        }

        // MAC verified — decrypt at counter=1 (block 0 was Poly1305 key)
        crypto_chacha20_ietf(out, ciphertext, plain_len, session_key, nonce, 1);
    }

    return plain_len;
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

void cl_cipher_suite::activate()
{
    encryption_active = true;
    kx_phase = KX_ACTIVE;
    printf("[CRYPTO] Encryption ACTIVATED (%s)\n",
           pq_active ? "hybrid PQ" : "classical X25519");
    fflush(stdout);
}
