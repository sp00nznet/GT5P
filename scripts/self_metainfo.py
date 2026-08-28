#!/usr/bin/env python3
"""Derive the decrypted 0x40-byte metadata info of an NPDRM SELF from its RAP.

ps3sce/scetool can then do the segment decryption and ELF rebuild:

    python scripts/self_metainfo.py EBOOT.BIN license.rap
    ps3sce -m <hex> -d EBOOT.BIN EBOOT.ELF

Split out because scetool's own RAP path needs an IDPS/act.dat for
network-licensed (type 1) titles, which GT5 Prologue is; the RAP alone is
enough once the metadata info is handed over ready-decrypted.
"""
import struct, sys
from pathlib import Path
from Crypto.Cipher import AES

sys.path.insert(0, str(Path(__file__).parent))
from decrypt_self import rap_to_klicensee, NP_KLIC_KEY, NP_KLIC_FREE, load_appldr_keys


def metadata_info(self_data, klicensee):
    _, _, key_rev, _, meta_off, _, _ = struct.unpack(">IIHHIQQ", self_data[:0x20])
    enc = self_data[meta_off + 0x20:meta_off + 0x20 + 0x40]

    # NPDRM layer: AES-128-CBC (zero IV) with the klicensee decrypted under
    # NP_klic_key, then the appldr AES-256-CBC pass.
    if klicensee is not None:
        k = AES.new(NP_KLIC_KEY, AES.MODE_ECB).decrypt(klicensee)
        enc = AES.new(k, AES.MODE_CBC, bytes(16)).decrypt(enc)

    keys = load_appldr_keys()
    order = [("NPDRM", key_rev), ("APP", key_rev)] + \
            [k for k in keys if k[1] != key_rev]
    for k in order:
        if k not in keys:
            continue
        erk, riv = keys[k]
        out = AES.new(erk, AES.MODE_CBC, riv).decrypt(enc)
        if out[16:32] == bytes(16) and out[48:64] == bytes(16):
            return out, k
    return None, None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    data = Path(sys.argv[1]).read_bytes()
    if len(sys.argv) > 2:
        klic = rap_to_klicensee(Path(sys.argv[2]).read_bytes())
    else:
        klic = NP_KLIC_FREE
    info, keyset = metadata_info(data, klic)
    if info is None:
        print("could not derive metadata info (wrong RAP?)", file=sys.stderr)
        return 1
    print(f"keyset  {keyset[0]} 0x{keyset[1]:04X}", file=sys.stderr)
    print(info.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
