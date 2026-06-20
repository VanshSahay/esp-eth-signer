// Original Keccak-256 implementation — matches Ethereum's keccak256 exactly.
// Test vector: keccak256("") = c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
#include "keccak256.h"
#include <string.h>

// Round constants for Keccak-f[1600] ι step
static const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
    0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

// Rotation offsets for ρ step: ROT[x][y]
static const int ROT[5][5] = {
    { 0, 36,  3, 41, 18},
    { 1, 44, 10, 45,  2},
    {62,  6, 43, 15, 61},
    {28, 55, 25, 21, 56},
    {27, 20, 39,  8, 14}
};

static inline uint64_t rotl64(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

// Keccak-f[1600] permutation — 24 rounds over a 1600-bit (5×5×64) state
static void keccak_f(uint64_t state[25]) {
    for (int round = 0; round < 24; round++) {
        // θ (theta): column parity diffusion
        uint64_t C[5];
        for (int x = 0; x < 5; x++) {
            C[x] = state[x] ^ state[x + 5] ^ state[x + 10]
                 ^ state[x + 15] ^ state[x + 20];
        }
        for (int x = 0; x < 5; x++) {
            uint64_t d = C[(x + 4) % 5] ^ rotl64(C[(x + 1) % 5], 1);
            for (int y = 0; y < 5; y++) {
                state[x + 5 * y] ^= d;
            }
        }

        // ρ (rho) and π (pi): lane rotation and permutation
        uint64_t B[25] = {0};
        for (int x = 0; x < 5; x++) {
            for (int y = 0; y < 5; y++) {
                B[y + 5 * ((2 * x + 3 * y) % 5)] =
                    rotl64(state[x + 5 * y], ROT[x][y]);
            }
        }

        // χ (chi): non-linear row mixing
        for (int y = 0; y < 5; y++) {
            for (int x = 0; x < 5; x++) {
                C[x] = B[x + 5 * y]
                     ^ ((~B[((x + 1) % 5) + 5 * y])
                      & B[((x + 2) % 5) + 5 * y]);
            }
            for (int x = 0; x < 5; x++) {
                B[x + 5 * y] = C[x];
            }
        }

        // ι (iota): round-constant injection
        B[0] ^= RC[round];

        memcpy(state, B, sizeof(B));
    }
}

void keccak256(const uint8_t* input, size_t len, uint8_t output[32]) {
    // Keccak-256: rate = 1088 bits (136 bytes), capacity = 512 bits
    // Output: 256 bits (32 bytes)
    static const size_t RATE = 136;

    uint64_t state[25];
    memset(state, 0, sizeof(state));

    // Absorb phase
    size_t offset = 0;
    while (offset + RATE <= len) {
        // Full blocks
        for (size_t i = 0; i < RATE / 8; i++) {
            uint64_t word = 0;
            for (size_t j = 0; j < 8; j++) {
                word |= ((uint64_t)input[offset + i * 8 + j]) << (8 * j);
            }
            state[i] ^= word;
        }
        keccak_f(state);
        offset += RATE;
    }

    // Final block with padding (pad10*1 rule)
    uint8_t last_block[136];
    memset(last_block, 0, RATE);
    size_t remaining = len - offset;
    memcpy(last_block, input + offset, remaining);

    // pad10*1: append 0x01, then pad with zeros, then set bit 7 of final byte
    last_block[remaining] = 0x01;
    last_block[RATE - 1] |= 0x80;

    for (size_t i = 0; i < RATE / 8; i++) {
        uint64_t word = 0;
        for (size_t j = 0; j < 8; j++) {
            word |= ((uint64_t)last_block[i * 8 + j]) << (8 * j);
        }
        state[i] ^= word;
    }
    keccak_f(state);

    // Squeeze phase — extract first 32 bytes (Keccak-256 output)
    for (size_t i = 0; i < 4; i++) {
        uint64_t w = state[i];
        for (size_t j = 0; j < 8; j++) {
            output[i * 8 + j] = (uint8_t)(w >> (8 * j));
        }
    }
}
