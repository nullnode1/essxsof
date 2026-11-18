#!/usr/bin/env python3
"""Offline/mock tests. These do not prove FreeBSD hardware compatibility."""
import importlib.machinery
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / 'desktop/essx-desktop'
loader = importlib.machinery.SourceFileLoader('desktop', str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
desktop = importlib.util.module_from_spec(spec)
loader.exec_module(desktop)

class FakePulse:
    def __init__(self):
        self.next = 100
        self.mod = {6: ('module-oss', 'device=/dev/dsp0'),
                    7: ('module-oss', 'device=/dev/dsp1'),
                    21: ('module-oss', 'device=/dev/dsp0')}
        self.items = {
            'oss_output.dsp0': self.sink(0, 6),
            'oss_output.dsp0.2': self.sink(3, 21),
            'oss_output.dsp1': self.sink(4, 7),
        }
        self.source_list = [{'name':'oss_output.dsp0.monitor', 'owner_module':6},
                            {'name':'oss_output.dsp0.2.monitor', 'owner_module':21},
                            {'name':'oss_input.dsp1', 'owner_module':7}]
        self.default = 'oss_output.dsp0'
        self.streams = [{'index':7,'sink':0,'owner_module':44}]
        self.order = []

    @staticmethod
    def sink(index, owner):
        return {'index':index, 'owner_module':owner, 'mute':False,
                'volume':{'front-left':{'value':65536},'front-right':{'value':65536}}}

    def pa(self, *args):
        self.order.append(args)
        action=args[0]
        if action == 'load-module':
            self.next += 1
            fields = args[2:]
            name = next((v.split('=',1)[1] for v in fields if v.startswith('sink_name=')), None)
            if not name:
                name = 'oss_output.dsp0.99'
            self.mod[self.next] = (args[1], ' '.join(fields))
            self.items[name] = self.sink(self.next + 1000, self.next)
            if args[1] == 'module-oss' and name.startswith('oss_output'):
                self.source_list.append({'name':name+'.monitor','owner_module':self.next})
            return str(self.next)
        if action == 'get-default-sink':
            return self.default
        if action == 'set-default-sink':
            self.default=args[1]; return ''
        if action == 'set-sink-volume':
            value=int(args[2].rstrip('%')) * 65536//100
            self.items[args[1]]['volume']={'front-left':{'value':value},'front-right':{'value':value}}
            return ''
        if action == 'set-sink-mute':
            self.items[args[1]]['mute']=args[2] == '1'
            return ''
        if action == 'move-sink-input':
            for s in self.streams:
                if str(s['index']) == args[1]:
                    s['sink']=self.items[args[2]]['index']
            return ''
        if action == 'unload-module':
            mid=int(args[1]); self.mod.pop(mid)
            self.items={n:s for n,s in self.items.items() if s['owner_module'] != mid}
            self.source_list=[s for s in self.source_list if s['owner_module'] != mid]
            return ''
        raise AssertionError(args)

class TestDesktop(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.m=desktop.Manager(self.tmp.name); self.m.prepare()
        self.pa=FakePulse()
        patchers=[patch.object(desktop, 'pa', self.pa.pa),
                  patch.object(desktop, 'hardware', lambda: (0,'/dev/dsp0')),
                  patch.object(desktop, 'sinks', lambda: self.pa.items),
                  patch.object(desktop, 'modules', lambda: self.pa.mod),
                  patch.object(desktop, 'json_list', self.listing)]
        for p in patchers:
            p.start(); self.addCleanup(p.stop)

    def listing(self, kind):
        return {'sources':self.pa.source_list,'sink-inputs':self.pa.streams}[kind]

    def test_create_profiles_and_48k_backend(self):
        s=self.m.create()
        self.assertTrue(self.m.valid(s))
        self.assertEqual(self.pa.default, desktop.PORTS[0])
        backend=self.pa.mod[s['backend']['id']][1]
        self.assertIn('rate=48000',backend)
        self.assertIn('record=no',backend)

    def test_switch_keeps_profiles_and_updates_default(self):
        s=self.m.create(); self.m.switch(s,1)
        self.assertEqual(self.pa.default,desktop.PORTS[1])
        self.assertTrue(self.pa.items[desktop.PORTS[0]]['mute'])
        self.assertFalse(self.pa.items[desktop.PORTS[1]]['mute'])

    def test_cleanup_removes_only_verified_dsp0_duplicates(self):
        self.m.create(); count=self.m.cleanup(apply=True)
        self.assertEqual(count,2)
        self.assertNotIn(6,self.pa.mod)
        self.assertNotIn(21,self.pa.mod)
        self.assertIn(7,self.pa.mod)  # USB microphone not touched
        self.assertIn('oss_input.dsp1',[x['name'] for x in self.pa.source_list])

    def test_cleanup_defaults_to_dry_run(self):
        self.m.create(); count=self.m.cleanup()
        self.assertEqual(count,2)
        self.assertIn(6,self.pa.mod)
        self.assertIn(21,self.pa.mod)

    def test_capture_on_dsp0_blocks_that_candidate(self):
        self.m.create()
        self.pa.source_list.append({'name':'some_real_input', 'owner_module':6})
        self.assertEqual(self.m.cleanup(apply=True),1)
        self.assertIn(6,self.pa.mod)

    def test_cleanup_skips_module_disappearing_between_discovery_and_unload(self):
        self.m.create()
        saved_modules = desktop.modules
        call_count = [0]
        def churn_modules():
            call_count[0] += 1
            # 1: initial discovery, 2: revalidation of candidate 6,
            # 3: final pre-unload check -> PulseAudio automatically removes 6.
            if call_count[0] == 3:
                self.pa.mod.pop(6, None)
                self.pa.items.pop('oss_output.dsp0', None)
                self.pa.source_list = [s for s in self.pa.source_list if s['owner_module'] != 6]
            return self.pa.mod
        with patch.object(desktop, 'modules', churn_modules):
            self.assertEqual(self.m.cleanup(apply=True), 1)
        self.assertNotIn(('unload-module', '6'), self.pa.order)
        self.assertNotIn(21, self.pa.mod)
        self.assertIn(7, self.pa.mod)

    def test_cleanup_skips_changed_module_id_without_unload(self):
        self.m.create()
        original = desktop.sinks
        calls = [0]
        def swap_sink():
            calls[0] += 1
            if calls[0] == 2:
                # Same name now owned by a new foreign module.
                self.pa.items['oss_output.dsp0']['owner_module'] = 999
            return self.pa.items
        with patch.object(desktop, 'sinks', swap_sink):
            self.assertEqual(self.m.cleanup(apply=True), 1)
        self.assertNotIn(('unload-module', '6'), self.pa.order)
        self.assertIn(7, self.pa.mod)

    def test_cleanup_preserves_capture_added_during_revalidation(self):
        self.m.create()
        original = desktop.json_list
        calls = [0]
        def attach_capture(kind):
            if kind == 'sources':
                calls[0] += 1
                if calls[0] == 2:
                    self.pa.source_list.append({'name':'new_capture', 'owner_module':6})
            return self.listing(kind)
        with patch.object(desktop, 'json_list', attach_capture):
            self.assertEqual(self.m.cleanup(apply=True), 1)
        self.assertIn(6, self.pa.mod)
        self.assertIn(7, self.pa.mod)

    def test_restores_fallback_after_cleanup(self):
        state=self.m.create(); self.m.cleanup(apply=True)
        self.m.restore(state)
        self.assertNotIn(desktop.PORTS[0],self.pa.items)
        self.assertNotIn(desktop.PORTS[1],self.pa.items)
        self.assertTrue(any(n.startswith('oss_output.dsp0') for n in self.pa.items))
        self.assertIn(7,self.pa.mod)

    def test_external_default_remains_external(self):
        self.pa.default='oss_output.dsp1'
        s=self.m.create()
        self.assertEqual(self.pa.default,'oss_output.dsp1')
        self.m.restore(s)

    def test_refuse_unmanaged_legacy_volume(self):
        self.pa.items['essx_volume_200']=self.pa.sink(22,20)
        with self.assertRaisesRegex(desktop.AudioError,'Legacy'):
            self.m.create()

    def test_no_unknown_module_unload_on_collision(self):
        self.pa.items[desktop.PORTS[0]]=self.pa.sink(999,333)
        with self.assertRaises(desktop.AudioError):
            self.m.create()
        self.assertFalse(any(a[0]=='unload-module' for a in self.pa.order))

if __name__ == '__main__':
    unittest.main(verbosity=2)
