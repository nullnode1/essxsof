"""Release assembly and bootstrap-contract checks (not a hardware test)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

class TestRelease(unittest.TestCase):
    def read(self, path):
        return (ROOT/path).read_text()

    def test_core_c_sources_identical_to_tested_rc1(self):
        import hashlib
        # SHA-256 fingerprints from the previously hardware-tested RC1 ZIP.
        known = {
            'essxsof.c': '590363402725255722da27a3aea8ebf55218a00cce9159060833f3b0c1ca7d88',
            'essx_codec.c': '06cd74257159da5c1ca51ce155afb0c5e1dc64138738b1a295fafc3c8bb561cf',
            'essx_ipc.c': '39323c7fc43da45fe64f1a8163c82d52538c43b307537797780f241ba07f8dc0',
            'essx_pcm.c': 'bd1d72e40657327427e865234c478f83cf173670c1c672c06c8ead323dee8d08',
            'essx_gpio.c': '516eeba8943ea1091108704bd2757ae88ec86c3859c71dd68576b868e248f1e1',
        }
        for name, digest in known.items():
            self.assertEqual(hashlib.sha256((ROOT/'src'/name).read_bytes()).hexdigest(), digest)

    def test_late_boot_after_login_and_uses_safe_start(self):
        late = self.read('src/rc.d/essxsoflate')
        self.assertIn('# REQUIRE: LOGIN essxsof', late)
        self.assertIn('essxsofctl start', late)
        self.assertNotIn('kldunload', late)
        self.assertNotIn('devctl detach', late)
        self.assertIn('essxsof_enable', late)

    def test_root_maintenance_backs_up_before_overwrite(self):
        script = self.read('src/native-repair.sh')
        self.assertLess(script.index('cp -p "$manifest"'), script.index('replace sbin/essxsofctl'))
        self.assertIn('INSTALL-MANIFEST', script)
        self.assertNotIn('kldunload', script)
        self.assertNotIn('devctl detach', script)

    def test_boot_files_are_uninstalled(self):
        self.assertIn('usr/local/etc/rc.d/essxsoflate', self.read('src/native-install.sh'))
        self.assertIn('usr/local/etc/rc.d/essxsoflate', self.read('src/native-uninstall.sh'))

    def test_pulseaudio_waits_for_kernel_and_audio_server(self):
        desktop = self.read('desktop/essx-desktop')
        self.assertIn('pa("info")', desktop)
        self.assertIn('hardware()', desktop)
        self.assertIn('for _ in range(90)', desktop)

    def test_optional_cleanup_does_not_abort_install(self):
        migration = self.read('desktop/migrate.py')
        self.assertIn('def optional_cleanup():', migration)
        self.assertIn('optional_cleanup()', migration)
        self.assertIn('WARNING: Optional duplicate cleanup did not finish.', migration)

    def test_cleanup_revalidates_before_unload(self):
        code = self.read('desktop/essx-desktop')
        self.assertIn('SKIP module {mid}: {name} changed during stream migration', code)
        self.assertIn('PRESERVED module {mid}: a capture source appeared', code)

    def test_optional_cleanup_warning_is_nonfatal(self):
        import importlib.util
        from unittest.mock import patch
        import subprocess
        import sys
        spec = importlib.util.spec_from_file_location('stable_migrate', ROOT/'desktop/migrate.py')
        migration = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(migration)
        failed = subprocess.CompletedProcess(['fake'], 1, stdout='REMOVE module 33\n', stderr='device changed')
        with patch.object(migration.subprocess, 'run', return_value=failed):
            self.assertFalse(migration.optional_cleanup())

    def test_readme_and_version(self):
        self.assertIn('1.0.0', self.read('README.md'))
        self.assertIn('VERSION = "1.0.0"', self.read('desktop/essx-desktop'))

if __name__ == '__main__': unittest.main()
