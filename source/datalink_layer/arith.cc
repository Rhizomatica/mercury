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
    return bw->bytepos + (bw->bitpos != 0);
}

typedef struct {
    uint8_t* buffer;
    int bitpos;
    int bytepos;
    int length;
} BitReader;

void br_init(BitReader* br, uint8_t* buf, int len) {
    br->buffer = buf;
    br->bitpos = 0;
    br->bytepos = 0;
    br->length = len;
}

int br_read_bit(BitReader* br) {
    if (br->bytepos >= br->length) return 0;
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

    printf("\n[ENCODE DONE] Total bits written: %d (%d bytes)\n", bw.total_bits, bw_bytes(&bw));
    printf("Bit packing: MSB-first within each byte (big-endian per byte)\n");

    return bw_bytes(&bw);
}

int arithmetic_decode(uint8_t* input, int len, char* output) {
    BitReader br;
    br_init(&br, input, len);

    uint64_t low = 0, high = MAX_CODE;
    uint64_t value = 0;
    uint64_t total = cum_freq[NUM_SYMBOLS];

    for (int i = 0; i < CODE_BITS; i++)
        value = (value << 1) | br_read_bit(&br);

    int outpos = 0;
    while (1) {
        uint64_t range = high - low + 1;
        uint64_t scaled = ((value - low + 1) * total - 1) / range;

        int sym = 0;
        while (!(scaled >= cum_freq[sym] && scaled < cum_freq[sym + 1])) {
            sym++;
            if (sym >= NUM_SYMBOLS) {
                fprintf(stderr, "Decode error: no matching symbol for scaled=%llu\n", scaled);
                return -1;
            }
        }

        if (symbols[sym] == '\0') break;  // EOF reached
        output[outpos++] = symbols[sym];

        high = low + (range * cum_freq[sym + 1]) / total - 1;
        low  = low + (range * cum_freq[sym]) / total;

        while (1) {
            if (high < HALF) {
                // do nothing
            } else if (low >= HALF) {
                value -= HALF;
                low   -= HALF;
                high  -= HALF;
            } else if (low >= QUARTER && high < THREE_QUARTERS) {
                value -= QUARTER;
                low   -= QUARTER;
                high  -= QUARTER;
            } else break;

            low <<= 1;
            high = (high << 1) | 1;
            value = (value << 1) | br_read_bit(&br);
        }
    }

    output[outpos] = '\0';

    return 0;
}
#if 0
int main() {
    init_model();
    const char* msg = "BRA31LA-15";
    printf("Input:   %s\n", msg);

    uint8_t encoded[BUFFER_SIZE];
    int enc_len = arithmetic_encode(msg, encoded);
    printf("Encoded length: %d bytes\n", enc_len);

    // init_model();
    char decoded[100];
    arithmetic_decode(encoded, enc_len, decoded);
    printf("Decoded: %s\n", decoded);

    return 0;
}
#endif
