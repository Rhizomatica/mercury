/*
 * Mercury post-quantum hybrid encryption suite.
 *
 * Hybrid key exchange: X25519 (classical EC-DH) + ML-KEM-768 (post-quantum KEM).
 * Symmetric encryption: ChaCha20-Poly1305 (IETF, 256-bit key, 96-bit nonce).
 * Key derivation: HKDF-Blake2b (monocypher).
 *
 * An attacker must break BOTH X25519 AND ML-KEM to recover the session key.
 * This defends against passive "store now, decrypt later" (SNDL) attacks
 * by future quantum computers, while maintaining classical security via X25519.
 *
 * See ENCRYPTION_PLAN.md for full design rationale and security invariants.
 */

#ifndef MERCURY_CRYPTO_H
#define MERCURY_CRYPTO_H

#include <cstdint>
#include <cstddef>

// Key material sizes (ML-KEM-768)
#define MLKEM_PK_SIZE    1184   // ML-KEM-768 encapsulation key (public)
#define MLKEM_SK_SIZE    2400   // ML-KEM-768 decapsulation key (secret)
#define MLKEM_CT_SIZE    1088   // ML-KEM-768 ciphertext
#define MLKEM_SS_SIZE    32     // ML-KEM-768 shared secret

#define X25519_KEY_SIZE  32     // X25519 public/secret key
#define SESSION_KEY_SIZE 32     // ChaCha20-Poly1305 key
#define AUTH_TAG_SIZE    16     // Poly1305 authentication tag
#define AUTH_TAG_ROBUST  4      // Truncated tag for ROBUST modes
#define NONCE_SIZE       12     // IETF ChaCha20-Poly1305 nonce

// Direction tags for nonce derivation
#define DIRECTION_CMD_TO_RSP  0x00000000
#define DIRECTION_RSP_TO_CMD  0x00000001

// Encryption modes (CLI -E flag)
#define ENCRYPT_OFF       0    // No encryption
#define ENCRYPT_STRICT    1    // SNDL-safe: hold data until full PQ key exchange
#define ENCRYPT_FAST      2    // Classical-first: X25519 immediate, PQ upgrade later

// Key exchange phases
#define KX_IDLE           0    // No key exchange in progress
#define KX_X25519_SENT    1    // X25519 pubkey sent, awaiting peer's
#define KX_X25519_DONE    2    // X25519 shared secret computed
#define KX_MLKEM_PK_SENT  3    // ML-KEM encaps key sent (commander only)
#define KX_MLKEM_CT_SENT  4    // ML-KEM ciphertext sent (responder only)
#define KX_HYBRID_DONE    5    // Both shared secrets ready, session key derived
#define KX_ACTIVE         6    // Encryption active (KEY_ACTIVATE exchanged)

// Per-batch encryption overhead
// encrypt() adds auth tag; decrypt() removes it
// Caller must account for this in batch capacity calculations
// Full tag (16 bytes) for CONFIG_0+, truncated (4 bytes) for ROBUST

class cl_cipher_suite {
public:
    cl_cipher_suite();
    ~cl_cipher_suite();

    // --- Key Exchange ---

    // Phase 1: X25519 (classical Diffie-Hellman)
    // Generate ephemeral X25519 keypair, write 32-byte pubkey to out.
    // Returns 0 on success, -1 on RNG failure.
    int generate_x25519_keypair(uint8_t pubkey_out[X25519_KEY_SIZE]);

    // Compute X25519 shared secret from peer's public key.
    // Must call generate_x25519_keypair() first.
    // Returns 0 on success.
    int compute_x25519_shared(const uint8_t peer_pubkey[X25519_KEY_SIZE]);

    // Phase 2: ML-KEM-768 (post-quantum KEM)
    // Generate ephemeral ML-KEM keypair, write 1184-byte encaps key to out.
    // Commander calls this. Returns 0 on success, -1 on RNG failure.
    int generate_mlkem_keypair(uint8_t encaps_key_out[MLKEM_PK_SIZE]);

    // Encapsulate: generate shared secret using peer's encaps key.
    // Writes 1088-byte ciphertext and 32-byte shared secret.
    // Responder calls this. Returns 0 on success.
    int encapsulate_mlkem(const uint8_t encaps_key[MLKEM_PK_SIZE],
                          uint8_t ciphertext_out[MLKEM_CT_SIZE]);

    // Decapsulate: recover shared secret from ciphertext.
    // Commander calls this (has the secret key from generate_mlkem_keypair).
    // Returns 0 on success.
    int decapsulate_mlkem(const uint8_t ciphertext[MLKEM_CT_SIZE]);

    // --- Key Derivation ---

    // Derive session key from X25519 + ML-KEM shared secrets via HKDF-Blake2b.
    // If psk is NULL, no PSK is mixed in (unauthenticated mode).
    // If mlkem_done is false, derives from X25519 only (classical-first mode).
    void derive_session_key(const char* commander_call,
                            const char* responder_call,
                            const uint8_t* psk, int psk_len,
                            bool mlkem_done);

    // Re-derive with both shared secrets after ML-KEM completes (PQ upgrade).
    // Call derive_session_key again with mlkem_done=true.

    // --- Per-Batch Encrypt/Decrypt ---

    // Encrypt plaintext batch. Writes ciphertext + auth tag to out.
    // Returns total bytes written (in_len + tag_size), or -1 on error.
    // tag_size is AUTH_TAG_SIZE (16) for CONFIG, AUTH_TAG_ROBUST (4) for ROBUST.
    int encrypt(const uint8_t* in, int in_len,
                uint8_t* out, int out_capacity,
                uint64_t batch_counter, uint32_t direction,
                int tag_size);

    // Decrypt ciphertext batch. Writes plaintext to out.
    // Returns plaintext bytes (in_len - tag_size), or -1 on auth failure.
    int decrypt(const uint8_t* in, int in_len,
                uint8_t* out, int out_capacity,
                uint64_t batch_counter, uint32_t direction,
                int tag_size);

    // --- State Queries ---
    bool is_active() const { return encryption_active; }
    bool is_pq_upgraded() const { return pq_active; }
    int  get_kx_phase() const { return kx_phase; }
    int  get_tag_size(bool is_robust) const { return is_robust ? AUTH_TAG_ROBUST : AUTH_TAG_SIZE; }

    // --- Key Confirmation ---
    // Compute 8-byte confirmation tag from session key.
    // Both sides compute this; mismatch = PSK wrong.
    void compute_key_confirmation(uint8_t tag_out[8]);

    // --- Activation ---
    void activate();     // Set encryption_active = true after KEY_ACTIVATE ACK

    // --- Cleanup ---
    // Securely wipe all key material. Called on disconnect.
    void wipe();

private:
    // X25519 state
    uint8_t x25519_sk[X25519_KEY_SIZE];
    uint8_t x25519_pk[X25519_KEY_SIZE];
    uint8_t x25519_shared[X25519_KEY_SIZE];
    bool x25519_ready;

    // ML-KEM state
    uint8_t* mlkem_sk;    // 2400 bytes (heap — too large for stack)
    uint8_t  mlkem_shared[MLKEM_SS_SIZE];
    bool mlkem_ready;

    // Session key
    uint8_t session_key[SESSION_KEY_SIZE];
    bool encryption_active;
    bool pq_active;
    int  kx_phase;
};

#endif
