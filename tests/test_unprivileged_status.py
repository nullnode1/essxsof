"""Offline read-only monitor visibility tests for RC5."""
from pathlib import Path
import subprocess
import unittest

SRC = (Path(__file__).resolve().parents[1] / 'src/sbin/essxsofctl').read_text()
FUN = SRC[SRC.index('jackd_visible_without_pid() {'):SRC.index('# The monitor must be gone')]
SCRIPT = '/usr/local/libexec/essxsof/essxsof-jackd'

class TestUnprivilegedStatus(unittest.TestCase):
    def verify_process_listing(self, listing):
        # The AWK body from our production fallback runs over a mocked ps.
        awk = FUN.split("ps axww 2>/dev/null | awk -v script=", 1)[1].split("\n    \'", 1)[0].split(" \'\n",1)[1]
        awk = awk.split("\n    '", 1)[0]
        return subprocess.run(['awk', '-v', 'script='+SCRIPT, awk], input=listing,
                              capture_output=True, text=True).returncode

    def test_one_real_monitor(self):
        listing = ('PID TT STAT TIME COMMAND\n'
                   '17831 - Is 0:00 daemon: '+SCRIPT+'[17876] (daemon)\n'
                   '17876 - S 0:01 /usr/local/bin/python3 '+SCRIPT+' (python3.12)\n')
        self.assertEqual(self.verify_process_listing(listing), 0)

    def test_nonroot_fallback_when_pid_directory_is_inaccessible(self):
        # Exercise the production shell function with an unreadable-path
        # sentinel and a mocked process table, without any kernel access.
        # A synthetic id function supplies the non-root result.
        script = """jackpid=/var/run/essxsof/jackd.pid
jackd=/usr/local/libexec/essxsof/essxsof-jackd
id() { printf '1000\n'; }
ps() { cat <<'PROCESSES'
PID TT STAT TIME COMMAND
17831 - Is 0:00 daemon: /usr/local/libexec/essxsof/essxsof-jackd[17876] (daemon)
17876 - S 0:01 /usr/local/bin/python3 /usr/local/libexec/essxsof/essxsof-jackd (python3.12)
PROCESSES
}
""" + FUN + '\njackd_visible_without_pid\n'
        r = subprocess.run(['sh', '-c', script], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_root_cannot_use_unprivileged_fallback(self):
        script = """jackd=/usr/local/libexec/essxsof/essxsof-jackd
id() { printf '0\n'; }
ps() { printf 'PID COMMAND\n17876 python3 /usr/local/libexec/essxsof/essxsof-jackd\n'; }
""" + FUN + '\njackd_visible_without_pid\n'
        r = subprocess.run(['sh', '-c', script], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)

    def test_parent_only(self):
        self.assertNotEqual(self.verify_process_listing('PID COMMAND\n17831 daemon: '+SCRIPT+'[17876]\n'), 0)

    def test_duplicate_monitors(self):
        self.assertNotEqual(self.verify_process_listing('PID COMMAND\n17876 python3 '+SCRIPT+'\n17877 python3 '+SCRIPT+'\n'), 0)

    def test_wrong_path(self):
        self.assertNotEqual(self.verify_process_listing('PID COMMAND\n17876 python3 /tmp/essxsof-jackd\n'), 0)

    def test_no_monitor(self):
        self.assertNotEqual(self.verify_process_listing('PID COMMAND\n'), 0)

    def test_protected_parent_directory_not_probed(self):
        # FreeBSD's /var/run/essxsof is mode 0700. Non-root callers
        # cannot stat jackd.pid, even with `test -f`.
        self.assertNotIn('[ -f "$jackpid" ]', FUN)
        self.assertNotIn('[ ! -r "$jackpid" ]', FUN)
        self.assertIn('"$(id -u)" -ne 0', FUN)
        self.assertIn('jackd_visible_without_pid;', SRC)
        # No privileged start/stop path uses the unprivileged fallback.
        a = SRC[SRC.index('stop_jackd() {'):SRC.index('status_driver() {')]
        self.assertNotIn('jackd_visible_without_pid', a)

if __name__ == '__main__': unittest.main()
