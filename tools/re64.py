"""Static analysis helpers over a dump of the game's main module (main.bin + main.base).

    python re64.py <dir> xref <offset> [...]     code that loads main+offset (ADRP+ADD/LDR) or calls it (BL/B)
    python re64.py <dir> dis <offset> [count]     disassemble from main+offset; names hash constants
    python re64.py <dir> func <offset>            disassemble the whole function containing main+offset
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

D = Path(sys.argv[1])
BIN = (D / "main.bin").read_bytes()
TEXT_END = 0x3C80000  # code ends before rodata (first strings ~0x3c8xxxx)
W = np.frombuffer(BIN[:TEXT_END], dtype="<u4")  # the code as 32-bit instruction words
NAMES = {}  # MurmurHash3 -> save field name, for naming constants in disassembly
DATA_DIR = Path(__file__).resolve().parents[1] / "data"
for names_file in (DATA_DIR / "rom_names.json", DATA_DIR / "memory_names.json"):  # later files win
    if names_file.exists():
        entries = json.loads(names_file.read_text(encoding="utf-8"))
        NAMES.update({int(h, 16): entry[0] for h, entry in entries.items()})  # entry: [name, type, ...]

# AArch64 encodings: (mask, value) pairs match an instruction class
ADRP = (0x9F000000, 0x90000000)
ADD_IMM = (0xFFC00000, 0x91000000)  # ADD Xd, Xn, #imm12
LDR_IMM = (0xFFC00000, 0xF9400000)  # LDR Xt, [Xn, #imm12 * 8]
B_OR_BL = (0x7C000000, 0x14000000)
PACIBSP = 0xD503237F  # first instruction of most functions
RETURNS = (0xD65F03C0, 0xD65F0FFF)  # RET, RETAB
PAGE_MASK = ~0xFFF
ADRP_USE_WINDOW = 7  # look this many instructions after an ADRP for the ADD / LDR that uses it
FUNC_SEARCH_LIMIT = 0x20000  # how far back func_start looks
WHOLE_FUNCTION = 5000  # instruction count for 'func'; dis stops at a return when asked for more than 4000


def sign_extend(values, bits):
    return np.where(values & (1 << (bits - 1)), values - (1 << bits), values)


def adrp_pages(pcs):
    """For every word, the page an ADRP there would load (only meaningful where the word is an ADRP)."""
    imm_lo = (W >> 29) & 3
    imm_hi = (W >> 5) & 0x7FFFF
    imm = (imm_hi.astype(np.int64) << 2) | imm_lo
    imm = sign_extend(imm, 21)
    return (pcs & PAGE_MASK) + (imm << 12)


def adrp_target(page, register, start):
    """The address an ADRP of page into register leads to: the first ADD or LDR using register in the next
    few instructions after index start (None if there isn't one)."""
    for j in range(start, min(start + ADRP_USE_WINDOW, len(W))):
        word = int(W[j])
        if (word >> 5) & 31 != register:
            continue
        if (word & ADD_IMM[0]) == ADD_IMM[1]:
            return page + ((word >> 10) & 0xFFF)
        if (word & LDR_IMM[0]) == LDR_IMM[1]:
            return page + ((word >> 10) & 0xFFF) * 8
    return None


def xref(targets):
    """{target: [offsets of code that loads its address (ADRP + ADD/LDR) or branches to it (B/BL)]}."""
    pcs = np.arange(len(W), dtype=np.int64) * 4
    is_adrp = (W & ADRP[0]) == ADRP[1]
    pages = adrp_pages(pcs)
    dest_registers = W & 31
    out = {target: [] for target in targets}
    target_pages = {target & PAGE_MASK for target in targets}
    for i in np.nonzero(is_adrp)[0]:
        page = int(pages[i])
        if page not in target_pages:
            continue
        address = adrp_target(page, int(dest_registers[i]), i + 1)
        if address in out:
            out[address].append(i * 4)
    is_branch = (W & B_OR_BL[0]) == B_OR_BL[1]
    branch_offsets = sign_extend((W & 0x03FFFFFF).astype(np.int64), 26)
    destinations = pcs + branch_offsets * 4
    for target in targets:
        out[target] += [int(pc) for pc in pcs[np.nonzero(is_branch & (destinations == target))[0]]]
    return out


def func_start(off):
    """The start of the function containing off: a PACIBSP, or just after the previous return or zero word."""
    for o in range(off, max(off - FUNC_SEARCH_LIMIT, 0), -4):
        word = struct.unpack_from("<I", BIN, o)[0]
        if word == PACIBSP:
            return o
        if (word in RETURNS or word == 0) and o != off:
            return o + 4
    return off


def dis(off, n):
    """Disassemble n instructions from off, naming 32-bit constants built with MOV / MOVK that are field hashes."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    registers = {}  # register (as wN) -> constant last moved into it
    for ins in md.disasm(BIN[off:off + 4 * n], off):
        note = ""
        operands = ins.op_str
        if ins.mnemonic in ("mov", "movz", "movk") and "#" in operands:
            register = operands.split(",")[0].strip().replace("x", "w")
            try:
                value = int(operands.split("#")[1].split(",")[0], 0)
            except ValueError:
                value = None
            if value is not None:
                if ins.mnemonic == "movk" and "lsl #16" in operands:
                    value = (registers.get(register, 0) & 0xFFFF) | (value << 16)
                registers[register] = value & 0xFFFFFFFF
                if registers[register] in NAMES:
                    note = f"   ; {NAMES[registers[register]]}"
        print(f"  {ins.address:#09x}: {ins.mnemonic:<6} {operands}{note}")
        if ins.mnemonic in ("ret", "retab") and n > 4000:
            break


def string_at(off):
    end = BIN.index(b"\0", off)
    return BIN[off:end].decode("latin-1")


def main():
    command = sys.argv[2]
    if command == "xref":
        targets = [int(x, 0) for x in sys.argv[3:]]
        for target, refs in xref(targets).items():
            print(f"{target:#x} {string_at(target)[:50]!r}: {len(refs)} ref(s)")
            for ref in refs:
                print(f"   {ref:#x}  (function {func_start(ref):#x})")
    elif command == "dis":
        dis(int(sys.argv[3], 0), int(sys.argv[4]) if len(sys.argv) > 4 else 64)
    elif command == "func":
        start = func_start(int(sys.argv[3], 0))
        print(f"function at {start:#x}")
        dis(start, WHOLE_FUNCTION)


if __name__ == "__main__":
    main()
