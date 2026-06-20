# ESP8266 secp256k1 library patches

The `secp256k1-embedded` library (from `diybitcoinhardware`) needs three
changes to work on the ESP8266's constrained 80 KB DRAM.  These are
already applied to the vendored copy in `firmware/lib/secp256k1/`.

If you ever update the vendored library from upstream, re-apply these
patches:

## Patch 1 — `libsecp256k1-config.h`

Enables `USE_ECMULT_STATIC_PRECOMPUTATION=1` and sets
`ECMULT_GEN_PREC_BITS=4`.  Without static precomp, the build-time
function allocates 100 KB+ on the stack (the ESP8266 has a 4 KB stack).

## Patch 2 — `ecmult_static_context.h`

Adds `ICACHE_RODATA_ATTR` to the 80 KB precomputed table so it lives in
**flash** instead of DRAM.  Without this the 80 KB table overflows the
80 KB total DRAM budget.  `ICACHE_RODATA_ATTR` maps the table through
the ESP8266's 32 KB SPI flash cache — signing is slightly slower but
fits comfortably in RAM.

On ESP32 this attribute is harmless (no-op macro).

## Patch 3 — `secp256k1/src/ecmult_gen_impl.h`

Converts two stack-allocated arrays (`prec` and `precj`, 100 KB+ total)
to `malloc`/`free` heap allocation.  This code path is only reached when
`USE_ECMULT_STATIC_PRECOMPUTATION` is off, so it's dead code in the
current build — but it prevents a stack overflow if someone experiments
with dynamic precomp.
