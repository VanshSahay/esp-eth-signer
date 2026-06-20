#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compute the original Keccak-256 hash (NOT NIST SHA3-256).
// This is the hash used by Ethereum: Keccak-256 with rate=1088, capacity=512,
// and the pad10*1 padding rule.
void keccak256(const uint8_t* input, size_t len, uint8_t output[32]);

#ifdef __cplusplus
}
#endif
