#!/usr/bin/env python3
"""User-run source contracts. Never invoked by Codex."""
from pathlib import Path
import unittest
D=Path(__file__).resolve().parents[2]
T=D.parents[2]
class ProductionStatic(unittest.TestCase):
 def text(self,path): return (D/path).read_text()
 def test_user_path_has_no_debug_dependency(self):
  for p in (D/'production/src').glob('*.cpp'):
   s=p.read_text()
   for x in ['ro.debuggable','ro.system.build.type','userdebug','ctl.start','GUACAMOLE_W1_DEBUG']:
    self.assertNotIn(x,s)
 def test_settings_cannot_call_mutation(self):
  for p in (T/'packages/apps/Settings/guacamole/src').rglob('*.java'):
   s=p.read_text()
   for x in ['.installKoreaConfiguration(','.rollbackKoreaConfiguration(','.applyCarrier(']:self.assertNotIn(x,s)
  self.assertNotIn('ListPreference',(T/'packages/apps/Settings/guacamole/res/xml/guacamole_efsd_settings.xml').read_text())
 def test_worker_fd_only_and_fresh(self):
  s=self.text('production/src/worker_main.cpp')
  self.assertIn('argc!=1',s);self.assertIn('read(4,cap',s)
  self.assertIn('expected.st_ino!=lock.st_ino',s)
  self.assertLess(s.index('journal.EnterWorker(rollback)'),s.index('factory.OpenWrite('))
  self.assertEqual(s.count('factory.OpenWrite('),1);self.assertEqual(s.count('factory.OpenRead('),1)
  launch=self.text('production/src/production_launcher.cpp')
  for x in ['posix_spawn(&child','waitpid(child','child_unreaped','DecodeProcessReport(frame, request, nonce']:self.assertIn(x,launch)
 def test_authorization_precedes_submit(self):
  s=self.text('efs_probe/src/guacamole_efsd_service.cpp').split('GuacamoleEfsdService::Submit',1)[1]
  self.assertLess(s.index('AuthorizedController(caller)'),s.index('coordinator_.Submit('))
  self.assertIn('AIBinder_getCallingSid()',s)
 def test_legacy_authority_cannot_mutate(self):
  s=self.text('production/src/evidence_main.cpp')
  for x in ['O_WRONLY','O_RDWR','unlink(','rename(','chmod(','chown(','WriteWholeFile','SetSelected','Activate']:self.assertNotIn(x,s)
  self.assertIn('peer.uid==kDaemonUid',s)
 def test_no_cache_or_legacy_transaction_in_production_modules(self):
  s=self.text('production/Android.bp')
  for x in ['w1_status','guacamole_efs_common','efs_transaction_backend','process_efs_posix']:self.assertNotIn(x,s)
 def test_policy_no_capability_workaround(self):
  for p in [D/'sepolicy/vendor/guacamole_efsd.te',D/'sepolicy/vendor/guacamole_efs_production.te']:
   s=p.read_text()
   for x in ['fsetid','dac_override','chown','permissive','dontaudit']:self.assertNotIn(x,s)
  self.assertNotIn('vendor_diag_device',self.text('sepolicy/vendor/guacamole_efsd.te'))
 def test_controller_unique_identity_and_confirmation(self):
  m=self.text('controller/AndroidManifest.xml')
  self.assertNotIn('android:sharedUserId=',m);self.assertIn('protectionLevel="signature"',m)
  s=self.text('controller/src/com/guacamole/efsd/controller/ConfirmationActivity.java')
  self.assertIn('setPositiveButton("Confirm once"',s);self.assertNotIn('getIntent()',s)
 def test_auto_remains_closed(self):
  self.assertRegex(self.text('pdc_probe/include/auto_slot0_experiment.h'),r'kDeviceAutoSlot0SendOpen\s*=\s*false')
 def test_current_unknown_never_reaches_write(self):
  s=self.text('production/src/production_coordinator.cpp')
  self.assertIn('io.RequireExactBaseline()',s)
  self.assertIn('Classify(o.bytes)==DbState::kUnknown',s)
  self.assertLess(s.index('const auto now=ReadRadio()'),s.index('return journal.Checkpoint(rollback)'))
 def test_no_radio_mutation_in_coordinator(self):
  s=self.text('production/src/production_coordinator.cpp')+self.text('production/src/production_radio.cpp')
  for x in ['SetSelectedConfig(', 'LoadConfig(', 'EnableSlot0(', 'Activate(']:self.assertNotIn(x,s)
if __name__=='__main__':unittest.main()
