#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Offline tests for essxsof-jackd with a fake kernel that mirrors the native
guards (bias admission, route refusal, shutdown ordering). No hardware."""
import importlib.machinery
import importlib.util
import pathlib
import unittest

path = pathlib.Path(__file__).resolve().parents[1] / 'sbin/essxsof-jackd'
loader = importlib.machinery.SourceFileLoader('jackd', str(path))
spec = importlib.util.spec_from_loader('jackd', loader)
jackd = importlib.util.module_from_spec(spec)
loader.exec_module(jackd)
JackError = jackd.JackError


class FakeKernel:
    """script: list of (tick, plugged). Flags follow the measured behavior:
    bias off -> always 0x20; bias on -> 0x22 unplugged / 0x20 plugged."""

    def __init__(self, plugged=False, version=jackd.VERSION):
        self.plugged = plugged
        self.s = {'driver_version': version, 'pcm_enable': '1', 'jack_poll_prepare': '0',
                  'jack_detect_enable': '0', 'jack_micbias_trial': '0',
                  'jack_bias_playback': '0', 'output_mode': '0'}
        self.writes = []
        self.reads = 0
        self.flag_override = None
        self.fail_route = False
        self.fail_set = {}
        self.tick = 0
        self.script = {}
        self.stop_at = None
        self.stopped = False

    def flags(self):
        if self.flag_override is not None:
            v = self.flag_override(self.reads)
            if v is not None:
                return v
        if self.s['jack_micbias_trial'] != '1':
            return 0x20
        return 0x20 if self.plugged else 0x22

    def get(self, name):
        if name == 'jack_flags':
            self.reads += 1
            return str(self.flags())
        if name not in self.s:
            raise JackError('no such sysctl ' + name)
        return self.s[name]

    def set(self, name, value):
        self.writes.append((name, value))
        if name in self.fail_set and self.fail_set[name](value):
            raise JackError('forced failure ' + name)
        s = self.s
        if name == 'jack_poll_prepare':
            s[name] = '0'
        elif name == 'jack_detect_enable':
            if value == '0' and s['jack_micbias_trial'] == '1':
                raise JackError('EBUSY detect off while trial')
            s[name] = value
        elif name == 'jack_micbias_trial':
            if value == '1' and s['jack_detect_enable'] != '1':
                raise JackError('EBUSY')
            if value == '0' and s['jack_bias_playback'] == '1':
                raise JackError('EBUSY playback admitted')
            s[name] = value
        elif name == 'jack_bias_playback':
            if s['jack_micbias_trial'] != '1' or s['jack_detect_enable'] != '1':
                raise JackError('EBUSY')
            s[name] = value
        elif name == 'output_mode':
            if self.fail_route:
                raise JackError('route refused')
            if s['jack_micbias_trial'] == '1' and s['jack_bias_playback'] != '1':
                raise JackError('EBUSY bias without admission')
            s[name] = value
        else:
            raise JackError('read-only ' + name)

    def modes(self):
        return [v for n, v in self.writes if n == 'output_mode']

    def clean(self):
        s = self.s
        return (s['jack_detect_enable'], s['jack_micbias_trial'], s['jack_bias_playback']) == ('0', '0', '0')


def make(kernel, **kw):
    state = {'n': 0}

    def wait(seconds):
        state['n'] += 1
        kernel.tick += 1
        if kernel.tick in kernel.script:
            kernel.plugged = kernel.script[kernel.tick]
        return kernel.stop_at is not None and kernel.tick >= kernel.stop_at
    logs = []
    d = jackd.Jackd(kernel, wait, log=logs.append, **kw)
    d.logs = logs
    return d


def run(kernel, **kw):
    d = make(kernel, **kw)
    error = None
    try:
        d.setup()
        d.run()
    except (JackError, ValueError) as exc:
        error = str(exc)
    return d, error, d.teardown()


class PolicyTests(unittest.TestCase):
    def test_bounce_needs_three(self):
        p = jackd.Policy()
        p.prime(0x22)
        p.commit(0)
        self.assertIsNone(p.observe(0x20))
        self.assertIsNone(p.observe(0x22))      # bounce back resets
        self.assertIsNone(p.observe(0x20))
        self.assertIsNone(p.observe(0x20))
        self.assertEqual(p.observe(0x20), 1)

    def test_unknown_raises_never_speakers(self):
        p = jackd.Policy()
        with self.assertRaises(ValueError):
            p.observe(0x24)

    def test_commit_requires_confirmation(self):
        p = jackd.Policy()
        p.observe(0x20)
        with self.assertRaises(ValueError):
            p.commit(1)


class DaemonTests(unittest.TestCase):
    def test_unplugged_start_plug_unplug_stop(self):
        k = FakeKernel(plugged=False)
        k.script = {20: True, 40: False}      # ticks after setup sampling
        k.stop_at = 60
        d, error, errors = run(k)
        self.assertIsNone(error)
        self.assertEqual(errors, [])
        self.assertEqual(k.modes(), ['0', '1', '0'])
        self.assertTrue(k.clean())
        # setup order and reverse teardown order
        names = [n for n, v in k.writes if n != 'output_mode']
        self.assertEqual(names, ['jack_poll_prepare', 'jack_detect_enable', 'jack_micbias_trial',
                                 'jack_bias_playback', 'jack_bias_playback', 'jack_micbias_trial',
                                 'jack_detect_enable'])
        self.assertEqual([v for n, v in k.writes if n == 'jack_bias_playback'], ['1', '0'])

    def test_plugged_at_start_never_selects_speakers(self):
        k = FakeKernel(plugged=True)
        k.stop_at = 15
        d, error, errors = run(k)
        self.assertIsNone(error)
        self.assertEqual(k.modes(), ['1'])
        self.assertTrue(k.clean())

    def test_bounce_does_not_route(self):
        k = FakeKernel(plugged=False)
        k.stop_at = 40
        base = {'n': 0}
        original = k.flags

        def flip(reads):
            # one-read glitches of 0x20 in the unplugged state
            return 0x20 if reads in (12, 20, 30) else None
        k.flag_override = flip
        d, error, errors = run(k)
        self.assertIsNone(error)
        self.assertEqual(k.modes(), ['0'])

    def test_isolated_unknown_values_ignored(self):
        k = FakeKernel(plugged=False)
        k.stop_at = 40
        k.flag_override = lambda r: 0x24 if r in (12, 13, 25) else None
        d, error, errors = run(k)
        self.assertIsNone(error)
        self.assertEqual(k.modes(), ['0'])

    def test_unknown_run_ends_with_cleanup_and_no_speaker_write(self):
        k = FakeKernel(plugged=True)
        k.flag_override = lambda r: 0xff if r > 8 else None
        k.stop_at = 10000
        d, error, errors = run(k, max_bad=10)
        self.assertIn('unusable', error)
        self.assertEqual(errors, [])
        self.assertTrue(k.clean())
        self.assertEqual(k.modes(), ['1'])      # only the initial headphone route

    def test_route_failure_cleans_up(self):
        k = FakeKernel(plugged=False)
        k.script = {15: True}
        k.stop_at = 10000

        def only_headphone(value):
            return value == '1'
        k.fail_set = {'output_mode': only_headphone}
        d, error, errors = run(k)
        self.assertIn('forced failure', error)
        self.assertEqual(errors, [])
        self.assertTrue(k.clean())

    def test_unstable_initial_state_never_admits_playback(self):
        k = FakeKernel(plugged=False)
        k.flag_override = lambda r: 0x20 if r == 3 else None
        d, error, errors = run(k)
        self.assertIn('unstable', error)
        self.assertEqual(errors, [])
        self.assertTrue(k.clean())
        self.assertEqual(k.modes(), [])
        self.assertNotIn(('jack_bias_playback', '1'), k.writes)

    def test_wrong_version_or_busy_makes_no_writes(self):
        for kernel in (FakeKernel(version='0.19.0-rc1'),):
            d, error, errors = run(kernel)
            self.assertIn('version', error)
            self.assertEqual(kernel.writes, [])
        k = FakeKernel()
        k.s['jack_micbias_trial'] = '1'
        d, error, errors = run(k)
        self.assertIn('already on', error)
        self.assertEqual(k.writes, [])
        k = FakeKernel()
        k.s['pcm_enable'] = '0'
        d, error, errors = run(k)
        self.assertIn('playback', error)
        self.assertEqual(k.writes, [])

    def test_stop_during_setup_rolls_back(self):
        k = FakeKernel()
        k.stop_at = 2                          # inside the initial sampling
        d, error, errors = run(k)
        self.assertIn('stop requested', error)
        self.assertTrue(k.clean())
        self.assertEqual(k.modes(), [])

    def test_partial_setup_failure_unwinds(self):
        k = FakeKernel()
        k.fail_set = {'jack_micbias_trial': lambda v: v == '1'}
        d, error, errors = run(k)
        self.assertIn('forced failure', error)
        self.assertEqual(errors, [])
        self.assertTrue(k.clean())

    def test_teardown_failure_reported_not_hidden(self):
        k = FakeKernel()
        k.stop_at = 12
        k.fail_set = {'jack_bias_playback': lambda v: v == '0'}
        d, error, errors = run(k)
        self.assertIsNone(error)
        self.assertTrue(errors)                 # EBUSY chain surfaces
        self.assertFalse(k.clean())


if __name__ == '__main__':
    unittest.main(verbosity=2)
