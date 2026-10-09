#!/usr/bin/env python3
"""Macro C source/artifact contracts. User-run only; no build/device commands."""
import importlib.util
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET
EFS = Path(__file__).resolve().parents[1]
DEVICE = EFS.parent
TREE = DEVICE.parents[2]
spec = importlib.util.spec_from_file_location('artifact', EFS/'tools/package_w1_artifact.py')
artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifact)
class W1Static(unittest.TestCase):
    def test_exact_artifact_preserves_original(self):
        self.assertEqual(len(artifact.validate((EFS/'artifacts/mcfg_sel_db.korea.xml').read_bytes())), 57655)
    def test_capabilities_and_common_unchanged(self):
        data = (EFS/'artifacts/mcfg_sel_db.korea.xml').read_bytes()
        original = artifact.validate(data)
        records = ET.fromstring(data).findall('.//SelRecord')
        for name in ['SKT', 'KT', 'LGU']:
            r = [r for r in records if r.get('carrier_name') == name]
            self.assertEqual(len(r), 1)
            self.assertEqual((r[0].get('volte'), r[0].get('vowifi')), ('true', 'false'))
        before = [r.attrib for r in ET.fromstring(original).findall('.//SelRecord')]
        after = [r.attrib for r in records if r.get('carrier_name') not in ['SKT','KT','LGU']]
        self.assertEqual(before, after)
    def test_production_binder_closed(self):
        service = (EFS/'src/guacamole_efsd_service.cpp').read_text()
        for name in ['prepareKoreanSupport','applyCarrier','restoreOriginal']:
            body = service.split('GuacamoleEfsdService::'+name,1)[1].split('}',1)[0]
            self.assertRegex(body,r'\*o\s*=\s*GuacamoleEfsdResult::BLOCKED_MAPPING;')
            for forbidden in ['WriteTarget(', 'WriteWholeFile(', 'RunExperiment(', 'SetSelected', 'LoadConfig']:
                self.assertNotIn(forbidden,body)
    def test_existing_executor_write_stays_closed(self):
        s=(EFS/'src/process_efs_posix.cpp').read_text()
        self.assertIn('request.operation != ProcessOperation::kRead',s)
        self.assertIn('device write CLOSED',s)
    def test_user_stub_and_init_only(self):
        s=(EFS/'src/w1_main.cpp').read_text()
        self.assertLess(s.index('#ifndef GUACAMOLE_W1_DEBUG'),s.index('int main'))
        rc=(EFS/'init/guacamole_efs_experiment.rc').read_text()
        self.assertEqual(rc.count('    disabled'),3)
        self.assertEqual(rc.count('    oneshot'),3)
        self.assertNotIn('    start ',rc)
        policy=(DEVICE/'sepolicy/vendor/guacamole_efs_experiment.te').read_text()
        self.assertNotIn('domain_auto_trans(su,',policy)
        self.assertNotIn('domain_auto_trans(shell,',policy)
    def test_worker_has_fresh_exec_and_reap(self):
        s=(EFS/'src/w1_process_launcher.cpp').read_text()
        self.assertIn('posix_spawn(&child',s)
        self.assertIn('waitpid(child',s)
        self.assertIn('child_unreaped',s)
        self.assertIn('DecodeProcessReport(frame, request, nonce',s)
        worker=(EFS/'src/w1_device.cpp').read_text()
        self.assertEqual(worker.count('factory.OpenRead('),1)
        self.assertEqual(worker.count('factory.OpenWrite('),1)
        self.assertLess(worker.index('journal.EnterWorker(rollback)'),worker.index('factory.OpenWrite('))
        self.assertIn('supplied!=nonce',worker)
    def test_auto_is_still_closed(self):
        s=(DEVICE/'pdc_probe/include/auto_slot0_experiment.h').read_text()
        self.assertRegex(s,r'kDeviceAutoSlot0SendOpen\s*=\s*false')
    def test_radio_readonly_no_mutation_call(self):
        s=(DEVICE/'pdc_probe/src/w1_pdc_readonly.cpp').read_text()
        for name in ['SetSelectedConfig(', 'LoadConfig(', 'Activate(', 'EnableSlot0(']:
            self.assertNotIn(name,s)
    def test_ui_has_no_mutation_binder_calls(self):
        gui=TREE/'packages/apps/Settings/guacamole'
        repo=(gui/'src/com/android/settings/guacamole/GuacamoleEfsdRepository.java').read_text()
        for name in ['.applyCarrier(', '.prepareKoreanSupport(', '.restoreOriginal(', '.installKoreaConfiguration(', '.rollbackKoreaConfiguration(']:
            self.assertNotIn(name,repo)
        root=ET.parse(gui/'res/xml/guacamole_efsd_settings.xml').getroot()
        a='{http://schemas.android.com/apk/res/android}'
        keys=['w1_sim1_carrier','w1_sim2_carrier','guacamole_efsd_apply','guacamole_efsd_restore','guacamole_efsd_prepare']
        for key in keys:
            nodes=[n for n in root.iter() if n.get(a+'key')==key]
            self.assertFalse(nodes)  # D1 removes unsupported controls instead of enabling them.
        self.assertFalse(list(root.iter('ListPreference')))
    def test_only_exact_init_controls(self):
        s=(DEVICE/'sepolicy/system_ext/private/property_contexts').read_text()
        lines=[l for l in s.splitlines() if 'guacamole_efs_ctl_prop' in l]
        self.assertEqual(len(lines),3)
        for l in lines:
            self.assertTrue(l.startswith('ctl.start$vendor.guacamole-efs-'))
            self.assertTrue(l.endswith(' exact string'))
    def test_c1_recovery_does_not_mutate_radio(self):
        coordinator=(EFS/'src/w1_experiment.cpp').read_text()
        self.assertIn('pre.RollbackAllowed()',coordinator)
        for name in ['SetSelectedConfig(', 'LoadConfig(', 'EnableSlot0(', 'Activate(']:
            self.assertNotIn(name,coordinator)
        radio=(DEVICE/'pdc_probe/src/w1_pdc_readonly.cpp').read_text()
        self.assertIn('r.pending_none=!s.data.pending_id;',radio)
        self.assertIn('*s.data.active_id==Id(i)',radio)
    def test_c1_backup_reuse_adds_no_destructive_file_api(self):
        journal=(EFS/'src/w1_journal.cpp').read_text()
        for name in ['unlink(', 'unlinkat(', 'rename(', 'renameat(', 'truncate(', 'O_TRUNC']:
            self.assertNotIn(name,journal)
        self.assertIn('O_WRONLY|O_CREAT|O_EXCL',journal)
        self.assertIn('saved==b',journal)
        self.assertIn('Read("korea_sel_db.before",&saved,true)',journal)
if __name__=='__main__':
    unittest.main()
