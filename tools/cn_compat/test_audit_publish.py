"""No game inputs needed: guard against accidental source publication."""
import unittest
from tools.cn_compat.audit_publish import inspect


class PublishGuard(unittest.TestCase):
    def test_source_and_address_metadata_allowed(self):
        for name, data in [('src/input/controller_mapping.h', b'/* handwritten code */'),
                           ('tools/cn_compat/localization_slots.json', b'["0x001CBE64"]'),
                           ('README.zh-CN.md', '中文说明'.encode('utf-8'))]:
            inspect(name, data)

    def test_local_data_and_binaries_rejected(self):
        for name in ['game_files/profile.json', 'analysis/report.md',
                     'port/recomp/gen/recomp_0000.c', 'build-cn-apk/log.txt',
                     'cn_profile.h', 'disc.iso', 'game.xbe', 'jetfont.dat',
                     'release.apk', 'private.keystore', 'password.pass']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                inspect(name, b'not uploaded')

    def test_disguised_binary_or_secret_rejected(self):
        for data in [b'XBEH\0binary', b'x'*2_000_001,
                     b'-----BEGIN ' + b'PRIVATE KEY-----',
                     b'gh' + b'p_' + b'a'*36]:
            with self.assertRaises(ValueError):
                inspect('accident.py', data)


if __name__ == '__main__':
    unittest.main()
