#!/usr/bin/env python3
"""Check guacamole port wiring without generating a Soong graph or ROM image.

Run after extracting the stock blobs. Requires Python, readelf and GNU make;
make only evaluates the product's small camera/Dolby configuration fragments.
"""
from pathlib import Path
import os
import re
import struct
import subprocess
import unittest

ROOT = Path(os.environ.get('PORT_ROOT', Path(__file__).resolve().parents[4]))
DOLBY = ROOT / 'vendor/oneplus/dolby-guacamole'
HARDWARE = ROOT / 'hardware/oplus-guacamole'

class PortLinks(unittest.TestCase):
    def test_dolby_effect_host_and_native_dependencies(self):
        text=(DOLBY/'Android.bp').read_text()
        blocks=re.split(r'\n(?=\w+\s*\{)',text)
        shared=0
        for block in blocks:
            if not block.startswith('cc_prebuilt_library_shared {'):
                continue
            name=re.search(r'name:\s*"([^"]+)"',block)[1]
            self.assertIn('compile_multilib: "both"',block,name)
            sources=re.findall(r'srcs:\s*\["([^"]+)"\]',block)
            self.assertEqual(len(sources),2,name)
            classes=set()
            libs=re.search(r'shared_libs:\s*\[(.*?)\]',block,re.S)[1]
            declared={n+'.so' for n in re.findall(r'"([^"]+)"',libs)}
            for src in sources:
                path=DOLBY/src
                with path.open('rb') as stream:header=stream.read(20)
                self.assertEqual(header[:4],b'\x7fELF',src)
                machine=struct.unpack_from('<H',header,18)[0]
                classes.add((header[4],machine))
                dependencies=subprocess.check_output(['readelf','-d',str(path)],text=True)
                needed=set(re.findall(r'Shared library: \[([^\]]+)\]',dependencies))
                self.assertFalse(needed-declared,(src,needed-declared))
            self.assertEqual(classes,{(1,40),(2,183)},name)
            shared+=1
        self.assertEqual(shared,7)
        # A 32-bit audio HAL cannot load ARM64 effects; retain its stock architecture.
        host=(ROOT/'hardware/interfaces/audio/common/all-versions/default/service/Android.bp').read_text()
        self.assertIn('compile_multilib: "prefer32"',host)
        device=(ROOT/'device/oneplus/sm8150-common/common.mk').read_text()
        self.assertIn('android.hardware.audio.service',device)

    def test_extraction_covers_both_abis(self):
        listed={line.split(':')[-1].split(';')[0] for line in (DOLBY/'proprietary-files.txt').read_text().splitlines() if line and not line.startswith('#')}
        sources=re.findall(r'srcs:\s*\["proprietary/([^"]+)"\]',(DOLBY/'Android.bp').read_text())
        self.assertTrue(sources)
        self.assertFalse(set(sources)-listed)

    def test_dolby_policy_follows_product_toggle(self):
        harness='''
define inherit-product
endef
define inherit-product-if-exists
$(if $(wildcard $(1)),$(eval include $(1)))
endef
include device/oneplus/guacamole/device.mk
$(info POLICY=$(SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS))
.PHONY: probe
probe: ;
'''
        for enabled in ('true','false'):
            result=subprocess.run(['make','-rR','--no-print-directory','-f','-',f'TARGET_ENABLE_ONEPLUS_DOLBY={enabled}','probe'],cwd=ROOT,input=harness,text=True,capture_output=True,check=True)
            line=next(l for l in result.stdout.splitlines() if l.startswith('POLICY='))
            self.assertEqual('vendor/oneplus/dolby-guacamole/sepolicy/public' in line,enabled=='true')
            self.assertIn('vendor/oplus/camera-guacamole/sepolicy/public',line)
        board=(ROOT/'device/oneplus/guacamole/BoardConfig.mk').read_text()
        self.assertNotIn('include vendor/oneplus/dolby-guacamole/',board)

    def test_camera_data_label_migration(self):
        init=(ROOT/'device/oneplus/sm8150-common/init/init.qcom.rc').read_text()
        self.assertIn('mkdir /data/vendor/camera 0777 camera camera',init)
        self.assertIn('restorecon_recursive /data/vendor/camera',init)
        camera=ROOT/'vendor/oplus/camera-guacamole'
        self.assertNotIn('init.oneplus.camera.rc',(camera/'opluscamera.mk').read_text())
        self.assertIn('app_domain(opluscamera_app)',(camera/'sepolicy/private/opluscamera_app.te').read_text())

    def test_vendor_namespace_matches_extractor(self):
        text=(ROOT/'vendor/oneplus/sm8150-common/Android.bp').read_text()
        imports=re.search(r'soong_namespace\s*\{(.*?)\}',text,re.S)[1]
        self.assertIn('"hardware/oplus-guacamole"',imports)
        self.assertNotIn('"hardware/oplus"',imports)
        extractor=(ROOT/'device/oneplus/sm8150-common/extract-files.py').read_text()
        self.assertIn("'hardware/oplus-guacamole'",extractor)

    def test_dms_property_labels_and_clients(self):
        context=(HARDWARE/'sepolicy/qti/vendor/property_contexts').read_text()
        for prop in ('persist.vendor.dolby.loglevel','vendor.dolby.dap.param.tee','vendor.dolby.debug.dap_pcm_dump','vendor.dolby.mi.metadata.log'):
            lines=[l for l in context.splitlines() if l.split() and l.split()[0]==prop]
            self.assertEqual(len(lines),1,prop)
            self.assertIn('vendor_dolby_dms_prop',lines[0])
        policy=(HARDWARE/'sepolicy/qti/vendor/hal_dms.te').read_text()
        for kind in ('client','server'):
            self.assertIn(f'get_prop(hal_dms_{kind}, vendor_dolby_dms_prop)',policy)
        self.assertIn('hal_client_domain(hal_audio_default, hal_dms)',policy)

    def test_face_backend_property_matches_hal(self):
        context=(HARDWARE/'sepolicy/qti/vendor/property_contexts').read_text()
        policy=(HARDWARE/'sepolicy/qti/vendor/hal_oneplus_face.te').read_text()
        for name in ('Session.cpp','AuthEngine.cpp'):
            source=(ROOT/'hardware/oneplus/aidl/face'/name).read_text()
            prop=re.search(r'GetBoolProperty\("(persist.vendor.faceunlock.tee)"',source)[1]
            line=next(l for l in context.splitlines() if l.split() and l.split()[0]==prop)
            self.assertIn('vendor_face_prop:s0 exact bool',line)
        self.assertIn('get_prop(hal_oneplus_face, vendor_face_prop)',policy)

if __name__ == '__main__':
    unittest.main()
