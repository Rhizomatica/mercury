/* Keys for Mercury's optional encryption ([crypto], docs/TNC.md)
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   mercury_keygen new <CALLSIGN> [dir]   a station key: dir/station.key
 *                                         (0600) and dir/<CALLSIGN>.pub
 *   mercury_keygen fp <file.pub>          a public key's fingerprint
 *   mercury_keygen broadcast <file>       a broadcast group key (0600)
 *
 * station.key is the X25519 private key, 32 raw bytes, and stays on the
 * station.  <CALLSIGN>.pub is its public key, 32 raw bytes: give it to the
 * stations that will connect, for their peers_dir, and compare its
 * fingerprint with them over a channel you trust.  The fingerprint is what
 * Mercury logs when a session is encrypted: the first 8 bytes of SHA-256 of
 * the public key, in hex.  Nothing is ever overwritten.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>

#define KEYLEN 32

static int write_new(const char *path, const uint8_t *key, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    ssize_t n = write(fd, key, KEYLEN);
    if (close(fd) != 0 || n != KEYLEN) {
        fprintf(stderr, "%s: write failed\n", path);
        unlink(path);
        return -1;
    }
    return 0;
}

static void fingerprint(const uint8_t pub[KEYLEN], char out[17])
{
    uint8_t digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, pub, KEYLEN);
    for (int i = 0; i < 8; i++)
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

static int cmd_new(const char *call_in, const char *dir)
{
    char call[32], path[1024], fp[17];
    uint8_t priv[KEYLEN], pub[KEYLEN];
    size_t n = strlen(call_in);

    /* The name Mercury looks up: the callsign as it goes on air, uppercase,
     * with its SSID.  Only [A-Z0-9-] is ever made into a path. */
    if (n == 0 || n >= sizeof(call)) {
        fprintf(stderr, "bad callsign\n");
        return 1;
    }
    for (size_t i = 0; i <= n; i++) {
        int c = toupper((unsigned char)call_in[i]);
        if (c && !isalnum(c) && c != '-') {
            fprintf(stderr, "bad callsign: only letters, digits and '-'\n");
            return 1;
        }
        call[i] = (char)c;
    }

    randombytes_buf(priv, sizeof(priv));
    crypto_scalarmult_base(pub, priv);

    snprintf(path, sizeof(path), "%s/station.key", dir);
    int rc = write_new(path, priv, 0600);
    sodium_memzero(priv, sizeof(priv));
    if (rc != 0)
        return 1;
    printf("private key: %s (keep it on this station)\n", path);

    snprintf(path, sizeof(path), "%s/%s.pub", dir, call);
    if (write_new(path, pub, 0644) != 0)
        return 1;
    fingerprint(pub, fp);
    printf("public key:  %s (for the other stations' peers_dir)\n", path);
    printf("fingerprint: %s\n", fp);
    return 0;
}

static int cmd_fp(const char *path)
{
    uint8_t pub[KEYLEN + 1];
    char fp[17];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(pub, 1, sizeof(pub), f) : 0;

    if (f)
        fclose(f);
    if (n != KEYLEN) {
        fprintf(stderr, "%s: not a %d-byte public key\n", path, KEYLEN);
        return 1;
    }
    fingerprint(pub, fp);
    printf("%s\n", fp);
    return 0;
}

static int cmd_broadcast(const char *path)
{
    uint8_t key[KEYLEN];
    randombytes_buf(key, sizeof(key));
    int rc = write_new(path, key, 0600);
    sodium_memzero(key, sizeof(key));
    if (rc == 0)
        printf("broadcast key: %s (the same file on every station of the network)\n", path);
    return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "libsodium failed to start\n");
        return 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "new"))
        return cmd_new(argv[2], argc > 3 ? argv[3] : ".");
    if (argc == 3 && !strcmp(argv[1], "fp"))
        return cmd_fp(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "broadcast"))
        return cmd_broadcast(argv[2]);
    fprintf(stderr,
            "usage: %s new <CALLSIGN> [dir]   station.key and <CALLSIGN>.pub in dir (default .)\n"
            "       %s fp <file.pub>          a public key's fingerprint\n"
            "       %s broadcast <file>       a broadcast group key\n",
            argv[0], argv[0], argv[0]);
    return 2;
}
