"""Validate the source-audited release documentation exists and stays aligned."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class TestDocumentation(unittest.TestCase):
    def test_all_documents_present(self):
        for name in ('README.md', 'GITHUB_RELEASE.md', 'TECHNICAL_INFO.txt',
                     'docs/HARDWARE_REGISTERS.md', 'docs/DRIVER_INTERNALS.md',
                     'docs/DEVELOPMENT_NOTES.txt'):
            with self.subTest(name=name):
                self.assertTrue((ROOT / name).is_file())
                self.assertGreater(len((ROOT / name).read_text()), 1000)

    def test_readme_links_exist(self):
        readme = (ROOT / 'README.md').read_text()
        for name in ('docs/HARDWARE_REGISTERS.md', 'docs/DRIVER_INTERNALS.md',
                     'docs/DEVELOPMENT_NOTES.txt', 'TECHNICAL_INFO.txt'):
            self.assertIn(name, readme)
            self.assertTrue((ROOT/name).exists())

    def test_register_map_matches_codec_driver(self):
        src = (ROOT / 'src/essx_codec.c').read_text()
        reg = (ROOT / 'docs/HARDWARE_REGISTERS.md').read_text()
        for source_value in (
            'U(0x30,0x20,0x20)', 'U(0x00,0xff,0x80)',
            'U(0x03,0xff,0x32)', 'U(0x01,0xf4,0x74)',
            'U(0x07,0xff,0x90)', 'U(0x33,0xff,attenuation_db*2)',
            'U(0x34,0xff,attenuation_db*2)',
        ):
            self.assertIn(source_value, src)
        for reg_id in ('0x00', '0x01', '0x03', '0x07', '0x18',
                       '0x30', '0x33', '0x34', '0x4d', '0x4e', '0x4f'):
            self.assertIn(reg_id, reg)
        self.assertIn('KNOWN = {0x20: 1, 0x22: 0}',
                      (ROOT/'src/sbin/essxsof-jackd').read_text())

    def test_publishing_preserves_history(self):
        guide = (ROOT/'GITHUB_RELEASE.md').read_text()
        self.assertIn('git clone https://github.com/nullnode1/essxsof.git', guide)
        self.assertNotIn('git init -b main', guide)
        self.assertNotIn('git push -u origin main', guide)

if __name__ == '__main__': unittest.main()
