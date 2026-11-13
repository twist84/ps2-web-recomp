"""Execute the original IRX pitch routine for optional differential tests.

Requires a locally supplied, hash-verified module. No game instructions or
tables are embedded here. Only the MIPS operations reached by this routine
are supported; unexpected control flow or instructions fail the test.
"""

import hashlib
import struct


def signed(value):
    return value - 0x100000000 if value & 0x80000000 else value


class PitchReference:
    def __init__(self, image):
        if hashlib.sha256(image).hexdigest() != (
            "b61c35c41f71c489b7f07d871e6b5293f0f872dbf3ab66672c487835e3bb460a"
        ):
            raise ValueError("expected the documented EU beta ARKD_DVD.IRX")
        # Locate executable .text through ELF section headers, avoiding
        # assumptions about its file offset versus its module address.
        section_offset = struct.unpack_from("<I", image, 32)[0]
        section_size, section_count = struct.unpack_from("<HH", image, 46)
        sections = [struct.unpack_from("<10I", image,
                    section_offset + index * section_size)
                    for index in range(section_count)]
        text, = [section for section in sections
                 if section[1] == 1 and section[2] & 4]
        self.code = {}
        for pc in range(0x33A4, 0x37FC, 4):
            word, = struct.unpack_from("<I", image, text[4] + pc - text[3])
            imm = word & 0xFFFF
            self.code[pc] = (word >> 26, (word >> 21) & 31,
                             (word >> 16) & 31, (word >> 11) & 31,
                             (word >> 6) & 31, word & 63,
                             imm - 65536 if imm & 0x8000 else imm,
                             (word & 0x3FFFFFF) << 2)

    def run(self, base_pitch, request):
        memory = bytearray(0x11000)
        registers = [0] * 32
        registers[30] = 0x1000  # Frame pointer.
        struct.pack_into("<I", memory, 0x1040, 0x2000)  # Command array.
        struct.pack_into("<h", memory, 0x2006, request)
        struct.pack_into("<H", memory, 0x106C0, base_pitch)
        hi = lo = 0
        pc = 0x33A4
        delayed_target = None
        for _ in range(400):
            if pc == 0x37FC:
                return struct.unpack_from("<H", memory, 0x1030)[0]
            op, rs, rt, rd, shift, function, imm, target = self.code[pc]
            left, right = registers[rs], registers[rt]
            next_pc = delayed_target if delayed_target is not None else pc + 4
            delayed_target = None
            if op == 0:
                if function == 0:  # sll (including nop)
                    registers[rd] = right << shift
                elif function == 3:  # sra
                    registers[rd] = signed(right) >> shift
                elif function == 0x21:  # addu
                    registers[rd] = left + right
                elif function == 0x23:  # subu
                    registers[rd] = left - right
                elif function == 0x25:  # or
                    registers[rd] = left | right
                elif function == 0x18:  # signed mult
                    product = signed(left) * signed(right)
                    hi, lo = (product >> 32) & 0xFFFFFFFF, product & 0xFFFFFFFF
                elif function == 0x10:
                    registers[rd] = hi
                elif function == 0x12:
                    registers[rd] = lo
                elif function in (0x1A, 0x1B):  # div / divu
                    numerator = signed(left) if function == 0x1A else left
                    denominator = signed(right) if function == 0x1A else right
                    if not denominator:
                        raise AssertionError("original routine divided by zero")
                    quotient = abs(numerator) // abs(denominator)
                    if (numerator < 0) != (denominator < 0):
                        quotient = -quotient
                    lo = quotient & 0xFFFFFFFF
                    hi = (numerator - quotient * denominator) & 0xFFFFFFFF
                else:
                    raise AssertionError(f"unsupported SPECIAL {function:x} at {pc:x}")
            elif op == 9:  # addiu
                registers[rt] = left + imm
            elif op == 13:  # ori
                registers[rt] = left | (imm & 0xFFFF)
            elif op == 15:  # lui
                registers[rt] = (imm & 0xFFFF) << 16
            elif op in (33, 35, 37, 41, 43):  # lh/lw/lhu/sh/sw
                address = (left + imm) & 0xFFFFFFFF
                width = 4 if op in (35, 43) else 2
                if address + width > len(memory):
                    raise AssertionError(f"out-of-range memory access at {pc:x}")
                if op in (41, 43):
                    struct.pack_into("<I" if width == 4 else "<H", memory,
                                     address, right & ((1 << (8 * width)) - 1))
                else:
                    fmt = {33: "<h", 35: "<I", 37: "<H"}[op]
                    registers[rt] = struct.unpack_from(fmt, memory, address)[0]
            elif op in (5, 6):  # bne / blez, with delay slot
                taken = left != right if op == 5 else signed(left) <= 0
                delayed_target = pc + 4 + imm * 4 if taken else pc + 8
            elif op == 2:  # j, with delay slot
                delayed_target = target
            else:
                raise AssertionError(f"unsupported opcode {op:x} at {pc:x}")
            registers[rd if op == 0 else rt] &= 0xFFFFFFFF
            registers[0] = 0
            pc = next_pc
        raise AssertionError("original pitch routine did not terminate")
