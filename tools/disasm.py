# Desensambla un PE desde una VA. Uso: python disasm.py <pe> <va> [instrucciones=12]
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
n = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
va = int(sys.argv[2], 16)
if va < base:
    va += base
for i in range(n):
    o = pe + 24 + opt + i * 40
    vs, sva, rs, ro = struct.unpack_from('<IIII', d, o + 8)
    if base + sva <= va < base + sva + rs:
        off = ro + va - base - sva
        break
cnt = int(sys.argv[3]) if len(sys.argv) > 3 else 12
for i, ins in enumerate(Cs(CS_ARCH_X86, CS_MODE_32).disasm(d[off:off + cnt * 15], va)):
    if i >= cnt:
        break
    print(f'{ins.address:#x} {ins.bytes.hex():18} {ins.mnemonic} {ins.op_str}')
