#!/usr/bin/env python3
"""
ESP Ethereum Transaction Signer — Host Client

Talks to the ESP device over USB serial. Sends a 32-byte hash to sign
and receives back (r, s, yParity). The Mac never sees the private key.

Usage:
    python sign_client.py --port /dev/cu.usbserial-0001 \\
        --rpc https://your-rpc-endpoint \\
        --to 0xRecipientAddress \\
        --value-eth 0.01

Add --broadcast to submit the raw signed transaction to the network.
"""

import argparse
import json
import sys
import time

import serial
from web3 import Web3

# rlp + keccak for manual EIP-1559 transaction encoding.
# We control exactly what gets hashed and assembled rather than
# relying on web3.py internals which shift across versions.
import rlp
from eth_hash.auto import keccak as _keccak


# ────────────────────────────────────────────────────────────────────
# Serial transport
# ────────────────────────────────────────────────────────────────────

class DeviceConnection:
    """Line-delimited JSON over USB serial to the ESP signer."""

    def __init__(self, port: str, baud: int = 115200, timeout: float = 60.0):
        self.port = port
        self.ser = serial.Serial(port, baud, timeout=timeout)
        # Drain any stale boot output from the device
        time.sleep(2)
        self.ser.reset_input_buffer()

    def _send(self, obj: dict) -> None:
        line = json.dumps(obj) + "\n"
        self.ser.write(line.encode("utf-8"))
        self.ser.flush()

    def _recv(self) -> dict:
        line = self.ser.readline()
        if not line:
            raise TimeoutError("Device did not respond (timeout)")
        try:
            return json.loads(line.decode("utf-8").strip())
        except json.JSONDecodeError:
            raise RuntimeError(f"Invalid JSON from device: {line!r}")

    def cmd(self, obj: dict) -> dict:
        self._send(obj)
        return self._recv()

    def ping(self) -> bool:
        resp = self.cmd({"cmd": "ping"})
        return resp.get("ok", False)

    def get_address(self) -> "tuple[str, str]":
        """Returns (address, pubkey) — both hex strings."""
        resp = self.cmd({"cmd": "get_address"})
        if not resp.get("ok"):
            raise RuntimeError(f"get_address failed: {resp.get('error')}")
        return resp["address"], resp["pubkey"]

    def sign_hash(self, hash_hex: str, display: dict) -> "tuple[str, str, int]":
        """Returns (r_hex, s_hex, yParity) or raises on rejection."""
        resp = self.cmd({
            "cmd": "sign",
            "hash": hash_hex,
            "display": display,
        })
        if not resp.get("ok"):
            error = resp.get("error", "unknown")
            raise RuntimeError(f"Device refused to sign: {error}")
        return resp["r"], resp["s"], resp["yParity"]

    def close(self) -> None:
        self.ser.close()


# ────────────────────────────────────────────────────────────────────
# EIP-1559 transaction helpers
# ────────────────────────────────────────────────────────────────────

def _to_bytes(val) -> bytes:
    """Convert a hex string or int to bytes for RLP encoding."""
    if isinstance(val, str):
        if val.startswith("0x"):
            val = val[2:]
        return bytes.fromhex(val)
    if isinstance(val, int):
        # RLP integer: big-endian, no leading zeros, zero = b''
        if val == 0:
            return b""
        n_bytes = (val.bit_length() + 7) // 8
        return val.to_bytes(n_bytes, "big")
    if isinstance(val, bytes):
        return val
    raise TypeError(f"Cannot convert {type(val)} to bytes for RLP")


def build_eip1559_signing_hash(
    chain_id: int,
    nonce: int,
    max_priority_fee: int,
    max_fee: int,
    gas_limit: int,
    to: str,
    value: int,
    data: bytes = b"",
) -> bytes:
    """
    Compute the EIP-1559 signing hash.

    Per EIP-2718: keccak256(0x02 || rlp([chain_id, nonce,
    max_priority_fee_per_gas, max_fee_per_gas, gas_limit,
    destination, value, data, access_list]))

    All integer fields are RLP-encoded without leading zeros.
    """
    to_bytes20 = bytes.fromhex(to[2:] if to.startswith("0x") else to)

    unsigned_fields = [
        _to_bytes(chain_id),
        _to_bytes(nonce),
        _to_bytes(max_priority_fee),
        _to_bytes(max_fee),
        _to_bytes(gas_limit),
        to_bytes20,
        _to_bytes(value),
        data,
        [],  # access_list — empty for now
    ]

    encoded = rlp.encode(unsigned_fields)
    payload = b"\x02" + encoded
    return _keccak(payload)


def assemble_signed_eip1559(
    chain_id: int,
    nonce: int,
    max_priority_fee: int,
    max_fee: int,
    gas_limit: int,
    to: str,
    value: int,
    data: bytes,
    y_parity: int,
    r_hex: str,
    s_hex: str,
) -> bytes:
    """
    Build the final signed EIP-1559 transaction:
    0x02 || rlp([chain_id, nonce, ..., access_list, yParity, r, s])
    """
    to_bytes20 = bytes.fromhex(to[2:] if to.startswith("0x") else to)

    # r and s are 32-byte values; strip leading zeros for RLP
    r_int = int(r_hex, 16)
    s_int = int(s_hex, 16)

    signed_fields = [
        _to_bytes(chain_id),
        _to_bytes(nonce),
        _to_bytes(max_priority_fee),
        _to_bytes(max_fee),
        _to_bytes(gas_limit),
        to_bytes20,
        _to_bytes(value),
        data,
        [],  # access_list
        _to_bytes(y_parity),
        _to_bytes(r_int),
        _to_bytes(s_int),
    ]

    encoded = rlp.encode(signed_fields)
    return b"\x02" + encoded


# ────────────────────────────────────────────────────────────────────
# CLI
# ────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description="ESP Ethereum Transaction Signer — Host Client"
    )
    p.add_argument(
        "--port", required=True,
        help="Serial device (e.g. /dev/cu.usbserial-0001 on macOS)"
    )
    p.add_argument(
        "--rpc",
        help="Ethereum JSON-RPC endpoint for nonce/gas estimation and broadcast"
    )
    p.add_argument(
        "--to", required=True,
        help="Recipient address (0x...)"
    )
    p.add_argument(
        "--value-eth", type=float, required=True,
        help="Amount to send in ETH"
    )
    p.add_argument(
        "--gas-limit", type=int, default=21000,
        help="Gas limit (default: 21000 for simple ETH transfer)"
    )
    p.add_argument(
        "--max-priority-fee-gwei", type=float, default=1.0,
        help="Max priority fee in gwei (default: 1.0)"
    )
    p.add_argument(
        "--max-fee-gwei", type=float, default=None,
        help="Max total fee per gas in gwei (auto-estimated if omitted)"
    )
    p.add_argument(
        "--nonce", type=int, default=None,
        help="Nonce (auto-fetched from RPC if omitted)"
    )
    p.add_argument(
        "--chain-id", type=int, default=1,
        help="Chain ID (default: 1 = Ethereum mainnet)"
    )
    p.add_argument(
        "--data", type=str, default="0x",
        help="Transaction data in hex (default: 0x = empty)"
    )
    p.add_argument(
        "--broadcast", action="store_true",
        help="Actually broadcast the signed transaction to the network"
    )
    p.add_argument(
        "--dry-run", action="store_true",
        help="Build and hash everything but don't contact the device"
    )
    return p.parse_args()


def main():
    args = parse_args()

    # ── Connect to device ─────────────────────────────────────────
    if not args.dry_run:
        print(f"Connecting to device on {args.port}...")
        dev = DeviceConnection(args.port)
        if not dev.ping():
            print("ERROR: device did not respond to ping", file=sys.stderr)
            sys.exit(1)
        print("  Device online.")

        address, pubkey = dev.get_address()
        print(f"  Signer address: {address}")
        print(f"  Signer pubkey : {pubkey}")
    else:
        dev = None
        address = "0xDRY_RUN_NO_DEVICE"
        print("[dry-run] Skipping device connection")

    # ── Connect to RPC ────────────────────────────────────────────
    w3 = None
    if args.rpc:
        w3 = Web3(Web3.HTTPProvider(args.rpc))
        if not w3.is_connected():
            print(f"ERROR: cannot reach RPC at {args.rpc}", file=sys.stderr)
            sys.exit(1)
        print(f"  RPC connected (chain {w3.eth.chain_id})")
    else:
        print("  No RPC provided — using manual nonce/fee values")

    # ── Fetch nonce ───────────────────────────────────────────────
    nonce = args.nonce
    if nonce is None:
        if w3:
            nonce = w3.eth.get_transaction_count(
                w3.to_checksum_address(address)
            )
            print(f"  Fetched nonce: {nonce}")
        else:
            print("ERROR: --nonce is required when --rpc is not provided",
                  file=sys.stderr)
            sys.exit(1)

    # ── Fee estimation ────────────────────────────────────────────
    max_priority_fee = int(args.max_priority_fee_gwei * 1e9)
    max_fee = None
    if args.max_fee_gwei is not None:
        max_fee = int(args.max_fee_gwei * 1e9)
    elif w3:
        # Use latest base fee + priority fee + a 25 % buffer
        block = w3.eth.get_block("latest")
        base_fee = block.get("baseFeePerGas", 0)
        max_fee = int(base_fee * 1.25) + max_priority_fee
        print(f"  Estimated max fee: {max_fee / 1e9:.2f} gwei "
              f"(base={base_fee / 1e9:.2f} + priority={max_priority_fee / 1e9:.2f})")
    else:
        print("ERROR: --max-fee-gwei is required when --rpc is not provided",
              file=sys.stderr)
        sys.exit(1)

    # ── Value ─────────────────────────────────────────────────────
    value = int(args.value_eth * 1e18)

    # ── Data ──────────────────────────────────────────────────────
    data_bytes = bytes.fromhex(args.data[2:] if args.data.startswith("0x") else args.data)

    # ── Build and hash the transaction ────────────────────────────
    gas_eth = (args.gas_limit * max_fee) / 1e18
    print()
    print("Transaction details:")
    print(f"  To:        {args.to}")
    print(f"  Value:     {args.value_eth} ETH")
    print(f"  Gas limit: {args.gas_limit}")
    print(f"  Max fee:   {max_fee / 1e9:.2f} gwei")
    print(f"  Priority:  {max_priority_fee / 1e9:.2f} gwei")
    print(f"  Max gas cost: ~{gas_eth:.6f} ETH")
    print(f"  Nonce:     {nonce}")
    print(f"  Chain ID:  {args.chain_id}")

    signing_hash = build_eip1559_signing_hash(
        chain_id=args.chain_id,
        nonce=nonce,
        max_priority_fee=max_priority_fee,
        max_fee=max_fee,
        gas_limit=args.gas_limit,
        to=args.to,
        value=value,
        data=data_bytes,
    )
    print(f"\n  Signing hash: 0x{signing_hash.hex()}")

    if args.dry_run:
        print("\n[dry-run] Done. Add --broadcast and remove --dry-run to sign for real.")
        return

    # ── Ask device to sign ─────────────────────────────────────────
    display_info = {
        "to":        args.to,
        "value_eth": f"{args.value_eth:.6f}",
        "gas_eth":   f"{gas_eth:.6f}",
        "nonce":     nonce,
        "chain_id":  args.chain_id,
    }

    print("\nSending signing request to device...")
    print("Look at the OLED and press CONFIRM or REJECT.")

    try:
        r_hex, s_hex, y_parity = dev.sign_hash(
            f"0x{signing_hash.hex()}", display_info
        )
    except RuntimeError as e:
        print(f"\n{e}", file=sys.stderr)
        sys.exit(1)
    finally:
        dev.close()

    print(f"  Signed!")
    print(f"  r:       {r_hex}")
    print(f"  s:       {s_hex}")
    print(f"  yParity: {y_parity}")

    # ── Assemble the signed transaction ───────────────────────────
    signed_tx = assemble_signed_eip1559(
        chain_id=args.chain_id,
        nonce=nonce,
        max_priority_fee=max_priority_fee,
        max_fee=max_fee,
        gas_limit=args.gas_limit,
        to=args.to,
        value=value,
        data=data_bytes,
        y_parity=y_parity,
        r_hex=r_hex,
        s_hex=s_hex,
    )
    signed_hex = "0x" + signed_tx.hex()
    print(f"\n  Signed TX: {signed_hex}")

    # ── Verify: recover the sender from the signature ─────────────
    # Recover the Ethereum address from the raw hash and (yParity, r, s).
    # _recover_hash is a stable internal eth-account API used by
    # etherscan, brownie, ape, and other tools.
    from eth_account.account import Account
    try:
        recovered = Account._recover_hash(
            signing_hash,
            vrs=(y_parity, int(r_hex, 16), int(s_hex, 16)),
        )
        if recovered.lower() == address.lower():
            print(f"  ✓ Signature verified — sender matches device address")
        else:
            print(f"  ✗ WARNING: recovered {recovered} != expected {address}",
                  file=sys.stderr)
    except Exception as e:
        print(f"  ⚠ Could not verify signature: {e}")

    # ── Broadcast ─────────────────────────────────────────────────
    if args.broadcast:
        if not w3:
            print("ERROR: --rpc is required for --broadcast", file=sys.stderr)
            sys.exit(1)
        print("\nBroadcasting transaction...")
        tx_hash = w3.eth.send_raw_transaction(signed_hex)
        print(f"  TX hash: {tx_hash.hex()}")
        print(f"  Explorer: https://etherscan.io/tx/{tx_hash.hex()}")
    else:
        print("\nNot broadcasting (add --broadcast to submit).")


if __name__ == "__main__":
    main()
