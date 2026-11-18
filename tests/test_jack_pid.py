"""Offline regression for FreeBSD PID-file verification semantics."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SRC = (Path(__file__).resolve().parents[1] / "src/sbin/essxsofctl").read_text()
FUN = SRC[SRC.index("jackd_pid() {"):SRC.index("# Started OUTSIDE")]
SCRIPT = "/usr/local/libexec/essxsof/essxsof-jackd"


class JackPidTest(unittest.TestCase):
    def call(self, listing, pidfile="17876", unknown_alive=False, fn="jackd_pid"):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "pid"
            p.write_text(pidfile)
            # All commands are local shell functions, so this never queries
            # or affects a real process, PID or kernel state.
            code = f'''jackpid="{p}"
jackd="{SCRIPT}"
ps() {{ cat <<'PS_END'
{listing}
PS_END
}}
kill() {{ [ "${{2:-}}" = 17876 ] && [ "${{1:-}}" = -0 ] && [ "{int(unknown_alive)}" = 1 ]; }}
{FUN}
{fn}
'''
            return subprocess.run(["sh", "-c", code], capture_output=True, text=True)

    def test_real_process(self):
        r = self.call(f''' PID TT STAT TIME COMMAND
17831 - Is 0:00 daemon: {SCRIPT}[17876] (daemon)
17876 - S 0:00 /usr/local/bin/python3 {SCRIPT} (python3.12)''')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "17876")

    def test_parent_wrapper_only_does_not_match(self):
        r = self.call(f"PID TT STAT TIME COMMAND\n17831 - Is 0:00 daemon: {SCRIPT}[17876]")
        self.assertNotEqual(r.returncode, 0)

    def test_wrong_pid_same_script(self):
        r = self.call(f"PID TT STAT TIME COMMAND\n17831 - Is 0:00 python3 {SCRIPT}")
        self.assertNotEqual(r.returncode, 0)

    def test_wrong_script_same_pid(self):
        r = self.call("PID TT STAT TIME COMMAND\n17876 - S 0:00 python3 /tmp/not-essxsof-jackd")
        self.assertNotEqual(r.returncode, 0)

    def test_malformed_pid_file(self):
        r = self.call(f"PID TT STAT TIME COMMAND\n17876 - S 0:00 python3 {SCRIPT}", pidfile="1; echo bad")
        self.assertNotEqual(r.returncode, 0)

    def test_unidentified_live_pid_detected(self):
        r = self.call("PID TT STAT TIME COMMAND\n17876 - S 0:00 python3 /usr/local/bin/other", unknown_alive=True, fn="jackd_unknown_live_pid")
        self.assertEqual(r.returncode, 0)

    def test_identified_live_pid_not_unknown(self):
        r = self.call(f"PID TT STAT TIME COMMAND\n17876 - S 0:00 python3 {SCRIPT}", unknown_alive=True, fn="jackd_unknown_live_pid")
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
