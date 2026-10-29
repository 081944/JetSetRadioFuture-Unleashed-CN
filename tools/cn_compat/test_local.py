"""Local integration checks; requires owned analysis inputs and unicorn.

Run from the repository: python -m unittest tools.cn_compat.test_local -v
"""
from pathlib import Path
import os
import re
import struct
import unittest

from tools.cn_compat.prepare import (ROOT, hybrid, offset, delta, rd,
                                     localized_slots, validate_custom_text)
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import *

ANALYSIS = ROOT.parent/'analysis'
PROFILE = ROOT/'game_files/cn_compat'
STOP = 0x400000
STACK = 0x600000


class Prototype(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.us = Path(os.environ.get('JSRF_US_XBE', ANALYSIS/'us.xbe')).read_bytes()
        cls.cn = Path(os.environ.get('JSRF_CN_XBE', ANALYSIS/'cn.xbe')).read_bytes()
        cls.target = hybrid(cls.us, cls.cn)

    def machine(self, data=None):
        data = data or self.target
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(0x10000, 0x700000)
        table = rd(data, 0x120)-0x10000
        for i in range(rd(data, 0x11C)):
            va, _, raw, size = struct.unpack_from('<IIII', data, table+i*56+4)
            uc.mem_write(va, data[raw:raw+size])
        return uc

    def call(self, uc, va, *args):
        uc.reg_write(UC_X86_REG_ESP, STACK)
        uc.mem_write(STACK, struct.pack('<'+'I'*(len(args)+1), STOP, *args))
        uc.emu_start(va, STOP, count=100000)
        self.assertEqual(uc.reg_read(UC_X86_REG_EIP), STOP)
        return uc.reg_read(UC_X86_REG_EAX)

    def test_profile_reconstructs_exact_target(self):
        out = bytearray(self.cn)
        for start, data in delta(self.cn, self.target):
            out[start:start+len(data)] = data
        self.assertEqual(bytes(out), self.target)

    def test_layout_and_language(self):
        self.assertEqual(rd(self.target, offset(0x1FC724)), 0)
        table = rd(self.us, 0x120)-0x10000
        for i in range(11):
            # Compare original section layout, excluding refreshed digests.
            self.assertEqual(self.us[table+i*56:table+i*56+36],
                             self.target[0xA00+i*56:0xA00+i*56+36])

    def test_localized_literals_preserve_us_layout_and_other_rdata(self):
        expected = bytearray(self.us)
        slots = list(localized_slots(self.us, self.cn))
        self.assertEqual(len(slots), 534)
        for start, end, literal in slots:
            expected[start:end] = literal
            self.assertEqual(self.target[start:end], literal)
            self.assertEqual(literal.split(b'\0')[0], self.cn[start:self.cn.index(0, start)])
        self.assertEqual(expected[0x1B4000:0x1DB7F0], self.target[0x1B4000:0x1DB7F0])

    def test_save_load_prompts_preserve_custom_encoded_translation(self):
        # Regression: these were excluded by strict standard CP932 decoding.
        for va in (0x1CB318, 0x1CB884, 0x1CB8C0, 0x1CB8E8,
                   0x1CBE9C, 0x1CBEBC, 0x1CBED8):
            start = offset(va)
            original = self.cn[start:self.cn.index(0, start)]
            with self.subTest(va=hex(va)):
                with self.assertRaises(UnicodeDecodeError):
                    original.decode('cp932')
                self.assertEqual(self.target[start:self.target.index(0, start)], original)
                self.assertNotEqual(original, self.us[start:self.us.index(0, start)])

    def test_custom_encoding_rejects_unknown_glyphs_and_truncated_pairs(self):
        glyphs = {0x8898}
        validate_custom_text(bytes.fromhex('8898') + b'$nA%s', glyphs)
        for invalid in (b'\x88', b'\x88\x99', b'\x01'):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                validate_custom_text(invalid, glyphs)

    def test_translated_rdata_strings_are_not_omitted_by_reference_filter(self):
        # Audit independent of the allowlist and of JP references. This catches
        # translated literals still used by US tables but omitted by JP tables.
        glyphs = {struct.unpack_from('<H', self.cn, offset(base)+i*4)[0]
                  for base, count in [(0x1F7088, 0x486), (0x289080, 0x183)]
                  for i in range(count)}
        checked = 0
        for match in re.finditer(rb'[^\0]{1,8192}\0', self.us[0x1B4000:0x1DB7F0]):
            start = 0x1B4000 + match.start()
            va = start + 0xFF60
            old = match.group()[:-1]
            new_end = self.cn.index(0, start)
            new = self.cn[start:new_end]
            if old == new or self.cn[start-1] or not any(b >= 128 for b in new):
                continue
            if struct.pack('<I', va) not in self.us:
                continue
            try:
                text = old.decode('cp932')
                validate_custom_text(new, glyphs)
            except (UnicodeDecodeError, ValueError):
                continue
            if any(ord(ch) < 32 and ch not in '\n\r\t' for ch in text):
                continue
            with self.subTest(va=hex(va)):
                self.assertEqual(self.target[start:self.target.index(0, start)], new)
            checked += 1
        # Other allowlisted entries contain only ASCII or have binary prefix
        # bytes before their string boundary; this independent scan covers 481.
        self.assertEqual(checked, 481)

    def test_all_character_mappings_match_chinese_original(self):
        expected = {}
        for i in range(0x486):
            code = struct.unpack_from('<H', self.cn, offset(0x1F7088)+i*4)[0]
            expected.setdefault(code, i)
        for i in range(0x183):
            code, glyph = struct.unpack_from('<HH', self.cn, offset(0x289080)+i*4)
            expected.setdefault(code, glyph)
        uc = self.machine()
        original = self.machine(self.cn)
        for code, glyph in expected.items():
            self.assertEqual(self.call(uc, 0x3C830, code), glyph, hex(code))
            self.assertEqual(self.call(original, 0x3C830, code), glyph, hex(code))
            self.assertEqual(uc.reg_read(UC_X86_REG_ESP), STACK+8)
        self.assertEqual(self.call(uc, 0x3C830, 0xFFFF), 0xFFFFFFFF)

    def test_seventh_texture_getter_and_stack_cleanup(self):
        uc = self.machine()
        obj, handle = 0x450000, 0x467890
        uc.mem_write(obj+0x15BC, struct.pack('<I', 1))
        uc.mem_write(0x289F00, struct.pack('<I', handle))
        for i in range(6):
            uc.mem_write(obj+0x1598+i*4, struct.pack('<I', 100+i))
        for index in range(8):
            uc.reg_write(UC_X86_REG_ECX, obj)
            result = self.call(uc, 0x2C010, index, 0, 0)
            self.assertEqual(result, 100+index if index < 6 else handle if index == 6 else 0)
            self.assertEqual(uc.reg_read(UC_X86_REG_ESP), STACK+16)
        uc.mem_write(obj+0x15BC, b'\0'*4)
        uc.reg_write(UC_X86_REG_ECX, obj)
        self.assertEqual(self.call(uc, 0x2C010, 6, 0, 0), 0)

    def test_draw_path_uses_extended_lookup_and_extra_font_page(self):
        uc = self.machine()
        obj = 0x450000
        for i in range(0x183):
            code, glyph = struct.unpack_from('<HH', self.cn, offset(0x289080)+i*4)
            uc.reg_write(UC_X86_REG_ECX, obj)
            uc.reg_write(UC_X86_REG_ESP, STACK)
            uc.mem_write(STACK, struct.pack('<II', STOP, code))
            uc.emu_start(0x3CA20, 0x3CA9B, count=100000)
            self.assertEqual(uc.reg_read(UC_X86_REG_EIP), 0x3CA9B)
            self.assertEqual(uc.reg_read(UC_X86_REG_ECX), glyph//361)

    def test_extra_resource_load_and_release_use_us_manager(self):
        uc = self.machine()
        obj, node, payload, handle = 0x450000, 0x460000, 0x470000, 0x480000
        calls = []
        def intercept(machine, address, size, user):
            if address not in (0x6E1C0, 0x6E7B0):
                return
            sp = machine.reg_read(UC_X86_REG_ESP)
            ra, arg = struct.unpack('<II', machine.mem_read(sp, 8))
            calls.append((address, machine.reg_read(UC_X86_REG_ECX), arg))
            machine.reg_write(UC_X86_REG_EAX, handle)
            machine.reg_write(UC_X86_REG_ESP, sp+8)  # thiscall, ret 4
            machine.reg_write(UC_X86_REG_EIP, ra)
        uc.hook_add(UC_HOOK_CODE, intercept)
        uc.mem_write(node, struct.pack('<I', payload))
        uc.mem_write(obj+0x15B8, struct.pack('<I', 0x490000))
        uc.reg_write(UC_X86_REG_EDI, node)
        uc.reg_write(UC_X86_REG_ESI, obj)
        uc.reg_write(UC_X86_REG_ESP, STACK)
        uc.emu_start(0x289700, 0x2C0EE, count=1000)
        self.assertEqual(calls, [(0x6E1C0, 0x252790, payload)])
        self.assertEqual(rd(uc.mem_read(0x289F00, 4), 0), handle)
        self.assertEqual(uc.reg_read(UC_X86_REG_EDX), 0x490000)
        self.assertEqual(uc.reg_read(UC_X86_REG_ESP), STACK)
        uc.emu_start(0x289800, 0x2C253, count=1000)
        self.assertEqual(calls[-1], (0x6E7B0, 0x252790, handle))
        self.assertEqual(bytes(uc.mem_read(0x289F00, 4)), b'\0'*4)
        self.assertEqual(bytes(uc.mem_read(obj+0x15BC, 4)), b'\0'*4)
        self.assertEqual(uc.reg_read(UC_X86_REG_ESP), STACK)


if __name__ == '__main__':
    unittest.main()
