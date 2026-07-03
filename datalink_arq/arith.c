/* Mercury Modem — arithmetic coding for ARQ
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NUM_SYMBOLS 38  // 37 symbols + 1 EOF
#define CODE_BITS 32
#define MAX_CODE ((1ULL << CODE_BITS) - 1)
#define HALF (1ULL << (CODE_BITS - 1))
#define QUARTER (HALF >> 1)
#define THREE_QUARTERS (HALF + QUARTER)
#define BUFFER_SIZE 4096
#define MAX_ENCODE_BITS (BUFFER_SIZE * 8)
#define MAX_PENDING 1024

char symbols[NUM_SYMBOLS] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '0','1','2','3','4','5','6','7','8','9','-',
    '\0'  // EOF symbol
};

uint64_t cum_freq[NUM_SYMBOLS + 1];

void init_model() {
    for (int i = 0; i <= NUM_SYMBOLS; i++)
        cum_freq[i] = i;
}

int find_index(char c) {
    for (int i = 0; i < NUM_SYMBOLS; i++)
        if (symbols[i] == c)
            return i;
    return -1;
}

typedef struct {
    uint8_t* buffer;
    int bitpos;
    int bytepos;
    int total_bits;
} BitWriter;

void bw_init(BitWriter* bw, uint8_t* buf) {
    bw->buffer = buf;
    bw->bitpos = 0;
    bw->bytepos = 0;
    bw->total_bits = 0;
    memset(buf, 0, BUFFER_SIZE);
}

void bw_write_bit(BitWriter* bw, int bit) {
    if (bw->bytepos * 8 + bw->bitpos >= MAX_ENCODE_BITS) {
        fprintf(stderr, "Fatal: bit output exceeded safe limit (%d bits)\n", MAX_ENCODE_BITS);
        exit(1);
    }
    if (bit)
        bw->buffer[bw->bytepos] |= (1 << (7 - bw->bitpos));
    if (++bw->bitpos == 8) {
        bw->bitpos = 0;
        bw->bytepos++;
    }
    bw->total_bits++;
}

int bw_bytes(BitWriter* bw) {
    return bw->bytepos + ((bw->bitpos != 0)?1:0);
}

typedef struct {
    uint8_t* buffer;
    int bitpos;
    int bytepos;
    int length;
    int max_read_ahead;
} BitReader;


void br_init(BitReader* br, uint8_t* buf, int len) {
    br->buffer = buf;
    br->bitpos = 0;
    br->bytepos = 0;
    br->length = len;
    br->max_read_ahead = 24;
}

int br_read_bit(BitReader* br)
{
    // limit how far we go...
    if (br->bytepos >= br->length)
    {
        br->max_read_ahead--;
        if (br->max_read_ahead == 0)
            return -1;
        else
            return 0;
    }
    int bit = (br->buffer[br->bytepos] >> (7 - br->bitpos)) & 1;
    if (++br->bitpos == 8) {
        br->bitpos = 0;
        br->bytepos++;
    }
    return bit;
}

int arithmetic_encode(const char* msg, uint8_t* output) {
    BitWriter bw;
    bw_init(&bw, output);

    uint64_t low = 0, high = MAX_CODE;
    int pending = 0;
    uint64_t total = cum_freq[NUM_SYMBOLS];

    for (int i = 0;; i++) {
        int sym;
        if (msg[i] == '\0') {
            sym = NUM_SYMBOLS - 1; // EOF symbol
        } else {
            sym = find_index(msg[i]);
            if (sym < 0) {
                fprintf(stderr, "Unknown symbol: %c\n", msg[i]);
                return 0;
            }
        }

        uint64_t range = high - low + 1;
        uint64_t sym_low = cum_freq[sym];
        uint64_t sym_high = cum_freq[sym + 1];

        high = low + (range * sym_high) / total - 1;
        low  = low + (range * sym_low)  / total;

        while (1) {
            if (high < HALF) {
                bw_write_bit(&bw, 0);
                while (pending-- > 0)
                    bw_write_bit(&bw, 1);
                pending = 0;
            } else if (low >= HALF) {
                bw_write_bit(&bw, 1);
                while (pending-- > 0)
                    bw_write_bit(&bw, 0);
                pending = 0;
                low -= HALF;
                high -= HALF;
            } else if (low >= QUARTER && high < THREE_QUARTERS) {
                pending++;
                low -= QUARTER;
                high -= QUARTER;
            } else {
                break;
            }
            low <<= 1;
            high = (high << 1) | 1;
        }

        if (sym == NUM_SYMBOLS - 1) break; // EOF encoded
    }

    // Final bits
    pending++;
    if (low < QUARTER) {
        bw_write_bit(&bw, 0);
        while (pending-- > 0)
            bw_write_bit(&bw, 1);
    } else {
        bw_write_bit(&bw, 1);
        while (pending-- > 0)
            bw_write_bit(&bw, 0);
    }

    return bw_bytes(&bw);
}

int arithmetic_decode(uint8_t* input, int max_len, char* output, int max_output) {
    if (!output || max_output <= 0)
        return -1;
    if (max_output == 1) {
        output[0] = '\0';
        return -1;
    }
    BitReader br;
    br_init(&br, input, max_len);

    uint64_t low = 0, high = MAX_CODE;
    uint64_t value = 0;
    uint64_t total = cum_freq[NUM_SYMBOLS];
    int outpos = 0;
    for (int i = 0; i < CODE_BITS; i++) {
        int b = br_read_bit(&br);
        if (b < 0) {
            fprintf(stderr, "Decode error: bitstream ended too early (init)\n");
            return -1;
        }
        value = (value << 1) | b;
    }


    while (1) {
        uint64_t range = high - low + 1;
        uint64_t scaled = ((value - low + 1) * total - 1) / range;

        int sym = 0;
        while (!(scaled >= cum_freq[sym] && scaled < cum_freq[sym + 1])) {
            sym++;
            if (sym >= NUM_SYMBOLS) {
                fprintf(stderr, "Decode error: no matching symbol for scaled=%llu\n", (unsigned long long)scaled);
                return -1;
            }
        }

        if (symbols[sym] == '\0') {
            break;  // EOF reached
        }
        if (outpos >= max_output - 1)
            goto finish;
        output[outpos++] = symbols[sym];

        high = low + (range * cum_freq[sym + 1]) / total - 1;
        low  = low + (range * cum_freq[sym]) / total;

        while (1) {
            if (high < HALF) {
                // no renormalization needed
            } else if (low >= HALF) {
                value -= HALF;
                low   -= HALF;
                high  -= HALF;
            } else if (low >= QUARTER && high < THREE_QUARTERS) {
                value -= QUARTER;
                low   -= QUARTER;
                high  -= QUARTER;
            } else {
                break;
            }

            low <<= 1;
            high = (high << 1) | 1;
            int b = br_read_bit(&br);
            if (b < 0)
                goto finish;
            value = (value << 1) | b;
        }
    }

finish:
    output[outpos] = '\0';
    return (outpos == 0) ? -1 : 0;
}
