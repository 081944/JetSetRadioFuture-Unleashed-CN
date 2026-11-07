"""Synthetic fixtures exercise resource lookup and corrupt capture rejection."""
import struct
import unittest

from inspect_character_switch import inspect


class CharacterInspectionTests(unittest.TestCase):
    def setUp(self):
        self.data = bytearray(0x240000)
        self.root, self.res, self.record, self.bank = 0x10000, 0x20000, 0x30000, 0x40000
        self.put(0x22FCE0, self.root)
        self.put(self.root + 0x98 + 7 * 4, self.res)
        self.put(self.res + 0x1E4, self.record)
        self.put(self.record + 8, 1)
        self.put(self.record + 0x640, 0x50000)
        self.put(self.record + 0x644, 0x51000)
        self.put(0x1EFFB0 + 0x14, 1 << 1)

    def put(self, at, value):
        struct.pack_into('<I', self.data, at, value)

    def test_exact_ready_and_locked(self):
        self.assertEqual(inspect(self.data, 1)['resource_action'], 'resources_ready')
        self.assertEqual(inspect(self.data, 2)['resource_action'], 'locked')
        self.assertFalse(inspect(self.data, 1)['live_swap_verified'])

    def test_preload_and_busy(self):
        self.put(self.res + 0x1E4, 0)
        self.assertEqual(inspect(self.data, 1)['resource_action'], 'preload_command_2')
        self.put(self.res + 0x44, 1)
        self.assertEqual(inspect(self.data, 1)['resource_action'], 'wait_loader')

    def test_shared_fallback(self):
        self.put(self.res + 0x1F0, self.bank)
        self.put(self.bank, 3)
        self.put(self.record + 0x10, 3)
        self.put(self.record + 0x644, 0)
        self.put(self.bank + 0x534, 0x52000)
        report = inspect(self.data, 1)
        self.assertEqual(report['resolved_model'], 0x50000)
        self.assertEqual(report['resolved_properties'], 0x52000)

    def test_masked_match_is_not_exact_residency(self):
        self.put(self.record + 8, 0x101)
        report = inspect(self.data, 1)
        self.assertEqual(report['resolved_model'], 0x50000)
        self.assertFalse(report['target_exact_resident'])
        self.assertEqual(report['resource_action'], 'preload_command_2')

    def test_bad_pointer_and_cycle(self):
        self.put(self.record + 0x65C, self.record)
        with self.assertRaisesRegex(ValueError, 'cyclic'):
            inspect(self.data, 1)
        self.put(self.record + 0x65C, 0x3FFFFFF)
        with self.assertRaisesRegex(ValueError, 'invalid CN guest range'):
            inspect(self.data, 1)

    def test_incomplete_and_invalid_target(self):
        self.put(self.record + 0x644, 0)
        self.assertEqual(inspect(self.data, 1)['resource_action'], 'incomplete_resources')
        with self.assertRaises(ValueError):
            inspect(self.data, 24)


if __name__ == '__main__':
    unittest.main()
