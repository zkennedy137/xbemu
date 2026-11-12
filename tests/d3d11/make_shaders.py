#!/usr/bin/env python3
"""Generate the test's SM4.0 shader bytecode as a C header (no D3D compiler needed).

Vertex shader:  o0 = v0 (POSITION passed to SV_Position)
Pixel shader:   SV_Target0 = (1, 0, 0, 1)

Encodings follow the D3D10 shader token format (d3d10tokenizedprogramformat.h):
opcodes, operand tokens (component count, selection mode, mask/swizzle, register type,
index dimension), and DXBC container chunks (ISGN, OSGN, SHDR).
"""
import math
import struct
import sys

# MD5 compression function (RFC 1321) for the DXBC digest, applied to whole blocks only.
_MD5_S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4
_MD5_K = [int(abs(math.sin(i + 1)) * (1 << 32)) & 0xFFFFFFFF for i in range(64)]


def md5_blocks(stream):
    assert len(stream) % 64 == 0
    a0, b0, c0, d0 = 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476
    for off in range(0, len(stream), 64):
        m = struct.unpack("<16I", stream[off:off + 64])
        a, b, c, d = a0, b0, c0, d0
        for i in range(64):
            if i < 16:
                f, g = (b & c) | (~b & d), i
            elif i < 32:
                f, g = (d & b) | (~d & c), (5 * i + 1) % 16
            elif i < 48:
                f, g = b ^ c ^ d, (3 * i + 5) % 16
            else:
                f, g = c ^ (b | (~d & 0xFFFFFFFF)), (7 * i) % 16
            f = (f + a + _MD5_K[i] + m[g]) & 0xFFFFFFFF
            a, d, c = d, c, b
            b = (b + ((f << _MD5_S[i]) | (f >> (32 - _MD5_S[i])))) & 0xFFFFFFFF
        a0 = (a0 + a) & 0xFFFFFFFF
        b0 = (b0 + b) & 0xFFFFFFFF
        c0 = (c0 + c) & 0xFFFFFFFF
        d0 = (d0 + d) & 0xFFFFFFFF
    return struct.pack("<4I", a0, b0, c0, d0)

OP_MOV = 54
OP_RET = 62
OP_DCL_INPUT = 95
OP_DCL_OUTPUT = 101
OP_DCL_OUTPUT_SIV = 103
NAME_POSITION = 1

REG_INPUT = 1
REG_OUTPUT = 2
REG_IMMEDIATE32 = 4

COMPS_4 = 2
SEL_MASK = 0
SEL_SWIZZLE = 1
MASK_XYZW = 0xF
SWIZZLE_XYZW = 0xE4  # x,y,z,w -> 0,1,2,3


def opcode(op, length):
    return op | (length << 24)


def operand(reg, sel, bits, index_dim):
    return COMPS_4 | (sel << 2) | (bits << 4) | (reg << 12) | (index_dim << 20)


def reg_operand(reg, sel=SEL_MASK, bits=MASK_XYZW):
    return [operand(reg, sel, bits, 1), 0]  # one immediate register index


def imm_operand(values):
    return [operand(REG_IMMEDIATE32, SEL_MASK, MASK_XYZW, 0)] + list(values)


def shader(program_type, tokens):
    body = [(program_type << 16) | (4 << 4) | 0, 2 + len(tokens)] + tokens
    return struct.pack("<%dI" % len(body), *body)


def signature(elements):
    """ISGN/OSGN body: count, header size, then 24-byte elements, then the names."""
    names = bytearray()
    header_size = 8
    elem_bytes = 24 * len(elements)
    name_base = header_size + elem_bytes
    out = bytearray(struct.pack("<II", len(elements), header_size))
    entries = []
    for name, sem_index, sys_value, reg, mask in elements:
        off = name_base + len(names)
        names += name.encode() + b"\0"
        entries.append(struct.pack("<IIIIIBBH", off, sem_index, sys_value, 3, reg, mask, mask, 0))
    for e in entries:
        out += e
    out += names
    while len(out) % 4:
        out += b"\0"
    return bytes(out)


def chunk(fourcc, data):
    return fourcc.encode() + struct.pack("<I", len(data)) + data


def dxbc_hash(data):
    """DXBC container digest, as checked by DXVK (dxbc_container.cpp, hashDxbcBinary).

    It is MD5 over the container from the version field on, but the final block is padded
    in a custom way: the bit count and a derived magic number are placed inside the padding.
    The stream is a whole number of blocks, so it is compressed with no extra finalization.
    """
    offset = 4 + 16  # magic + digest
    body = data[offset:]
    size = len(body)
    a_num = (size * 8) & 0xFFFFFFFF
    b_num = ((a_num >> 2) | 1) & 0xFFFFFFFF
    a = struct.pack("<I", a_num)
    b = struct.pack("<I", b_num)
    remainder = size % 64
    pad_size = 64 - remainder
    pad = b"\x80" + b"\0" * 63
    stream = body[:size - remainder]
    if remainder >= 56:
        stream += body[size - remainder:] + pad[:pad_size] + a + b"\0" * 56 + b
    else:
        stream += a
        if remainder:
            stream += body[size - remainder:]
        stream += pad[:pad_size - 8] + b
    return md5_blocks(stream)


def dxbc(chunks):
    header_len = 4 + 16 + 4 + 4 + 4 + 4 * len(chunks)
    offsets = []
    pos = header_len
    for c in chunks:
        offsets.append(pos)
        pos += len(c)
    body = b"".join(chunks)
    head = b"DXBC" + b"\0" * 16 + struct.pack("<III", 1, header_len + len(body), len(chunks))
    head += struct.pack("<%dI" % len(offsets), *offsets)
    container = bytearray(head + body)
    container[4:20] = dxbc_hash(bytes(container))
    return bytes(container)


def vertex_shader():
    tokens = []
    tokens += [opcode(OP_DCL_INPUT, 3)] + reg_operand(REG_INPUT)
    tokens += [opcode(OP_DCL_OUTPUT_SIV, 4)] + reg_operand(REG_OUTPUT) + [NAME_POSITION]
    src = [operand(REG_INPUT, SEL_SWIZZLE, SWIZZLE_XYZW, 1), 0]
    tokens += [opcode(OP_MOV, 5)] + reg_operand(REG_OUTPUT) + src
    tokens += [opcode(OP_RET, 1)]
    return shader(1, tokens)   # SM4 program type 1 = vertex shader


def pixel_shader():
    tokens = []
    tokens += [opcode(OP_DCL_OUTPUT, 3)] + reg_operand(REG_OUTPUT)
    red = [0x3F800000, 0, 0, 0x3F800000]  # 1.0, 0.0, 0.0, 1.0
    tokens += [opcode(OP_MOV, 8)] + reg_operand(REG_OUTPUT) + imm_operand(red)
    tokens += [opcode(OP_RET, 1)]
    return shader(0, tokens)   # SM4 program type 0 = pixel shader


def build():
    vs_code = vertex_shader()
    ps_code = pixel_shader()
    vs = dxbc([
        chunk("ISGN", signature([("POSITION", 0, 0, 0, MASK_XYZW)])),
        chunk("OSGN", signature([("SV_POSITION", 0, 1, 0, MASK_XYZW)])),
        chunk("SHDR", vs_code),
    ])
    ps = dxbc([
        chunk("ISGN", signature([("SV_POSITION", 0, 1, 0, MASK_XYZW)])),
        chunk("OSGN", signature([("SV_TARGET", 0, 0, 0, MASK_XYZW)])),
        chunk("SHDR", ps_code),
    ])
    return vs, ps


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 12]) + ",")
    return "static const unsigned char %s[%d] = {\n%s\n};\n" % (name, len(data), "\n".join(lines))


def main():
    vs, ps = build()
    out = ["/* Generated by make_shaders.py. Do not edit. */", c_array("g_vs_dxbc", vs), c_array("g_ps_dxbc", ps)]
    text = "\n".join(out)
    if len(sys.argv) > 1:
        open(sys.argv[1], "w").write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
