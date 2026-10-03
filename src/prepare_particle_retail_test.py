"""Prepare a private, generated retail reference image for offline x86 tests.

No game code is copied into the repository. Pass --output in a temporary folder.
Requires pefile and capstone; accepts only the verified RotWK retail baseline.
"""
import argparse
from pathlib import Path
import struct
import pefile
import capstone

parser = argparse.ArgumentParser()
parser.add_argument('--retail', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
pe = pefile.PE(str(args.retail))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value
section = next(s for s in pe.sections if s.Name.startswith(b'.text'))
assert fnv(section.get_data()[:min(section.Misc_VirtualSize, section.SizeOfRawData)]) == 0x4404F3C6
expected = [(0x1A7DF0, 357, 0x12FD16CD, 6), (0x19CD70, 1283, 0xD537820E, 5),
            (0x1F530A, 111, 0x219D8714, 5), (0x1F5487, 34, 0x6B01CE05, 5),
            (0x1F5BC9, 42, 0x1C2E8E2B, 6), (0x1F5DAF, 51, 0x78444B01, 6),
            (0x4C4E2, 44, 0xB7E77D2B, 6), (0x3077DF, 24, 0xD86B267D, 6)]
assert fnv(pe.get_data(0x4C4BE, 36)) == 0xB9EB20C4  # original handle construction
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for rva, size, digest, stolen in expected:
    assert fnv(pe.get_data(rva, size)) == digest
    instructions = list(decoder.disasm(pe.get_data(rva, stolen), rva + 0x400000))
    assert sum(i.size for i in instructions) == stolen
    assert not any(i.mnemonic.startswith(('call', 'j', 'loop')) for i in instructions)

# A compact mapped-image input: headers and initialized section bytes. It is
# loaded only in a fixture linked above the retail image's preferred address.
with args.output.open('wb') as output:
    output.write(b'RWFX')
    output.write(struct.pack('<II', pe.OPTIONAL_HEADER.SizeOfImage, len(pe.sections)))
    for section in pe.sections:
        raw = section.get_data()
        output.write(struct.pack('<II', section.VirtualAddress, len(raw)))
        output.write(raw)
print('PASS: RotWK retail hash, nine full body hashes and eight detour instruction boundaries')
