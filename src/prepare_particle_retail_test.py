"""Prepare a private, generated retail reference image for offline x86 tests.

No game code is copied into the repository. Pass --output in a temporary folder.
Requires pefile and capstone; accepts the two independently verified RotWK inputs.
"""
import argparse
from pathlib import Path
import struct
import pefile
import capstone

parser = argparse.ArgumentParser()
parser.add_argument('--retail', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--work', action='store_true', help='also verify render-list, audio and coarse-profile capabilities')
args = parser.parse_args()
pe = pefile.PE(str(args.retail))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value
section = next(s for s in pe.sections if s.Name.startswith(b'.text'))
text_hash = fnv(section.get_data()[:min(section.Misc_VirtualSize, section.SizeOfRawData)])
assert text_hash in (0x4404F3C6, 0x88C193EE)
expected = [(0x1A7DF0, 357, 0x12FD16CD, 6), (0x19CD70, 1283, 0xD537820E, 5),
            (0x1F530A, 111, 0x219D8714, 5), (0x1F5487, 34, 0x6B01CE05, 5),
            (0x1F5BC9, 42, 0x1C2E8E2B, 6), (0x1F5DAF, 51, 0x78444B01, 6),
            (0x4C4E2, 44, 0xB7E77D2B, 6), (0x3077DF, 24, 0xD86B267D, 6)]
assert fnv(pe.get_data(0x4C4BE, 36)) == 0xB9EB20C4  # original handle construction
if args.work:
    for rva, size, digest in [(0x172D07, 6000, 0x19F953F8), (0x7AF67, 16, 0x2DA9B0EB),
                              (0x53D00, 57600, 0x56EEE867), (0x20011F,100,0x266314F9),
                              (0x46DC96,100,0x3B148F4E), (0x302055,100,0x7ABC9597),
                              (0x54EB6C,100,0x57A276A9), (0x1FFE47,100,0xCE5D1552),
                              (0x7318F0,249,0x9DE80AFB),(0x7319F0,277,0x3AA2D946),
                              (0x731B50,155,0x81BF8843),(0x731B10,52,0x184C7247)]:
        assert fnv(pe.get_data(rva, size)) == digest, hex(rva)
    expected += [(0x1740D0,67,0x43E271F1,5),(0x17430D,249,0xA6ED392F,5),
                 (0x173816,51,0xCD56C32A,5),(0x61CA3,259,0x2475C0B5,5),
                 (0x2329B0,356,0x1698D50B if text_hash==0x88C193EE else 0x60BF8EBD,8),
                 (0x232409,294,0x4D60CC97,5),(0x1F5123,327,0x0698EE30,5),
                 (0x2F2364,666,0x4F58A019,6),(0x17EA80,488,0x8A2D9ECE,6)]
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
print(f'PASS: RotWK code hash {text_hash:08X}, {len(expected)+1} full body hashes and {len(expected)} detour boundaries' + ('; twelve dependency regions' if args.work else ''))
