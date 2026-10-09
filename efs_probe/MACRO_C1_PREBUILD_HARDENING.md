# Macro C.1 — recovery pre-build hardening

Source-only 후속 수정이다. 기존 Macro C 0026/0027/0028 source는 이미 적용된 상태에서
이번 delta만 추가한다. 기존 patch 파일은 수정/재생성/apply/revert하지 않는다.
빌드·test·adb·fastboot·device mutation은 실행하지 않았다.

## Gate 분리

- Apply: 기존 `Preconditions::Allowed()` 유지. Common exact active / pending NONE 필수.
- Rollback: `RollbackAllowed()` 사용. exact-known Common/SKT/KT/LGU active 중 하나,
  pending NONE 필수. UNKNOWN/empty active, 어떤 pending TLV라도 존재하면 block한다.
- active_known/pending_none은 성공하고 valid한 GetSelected response에서 exact ID bytes로만
  계산한다. 로그 문자열이나 cached UI 상태로 판단하지 않는다.
- root/debug/exact context, AUTO evidence, slot0=3/true, slot1=0/false, 네 profile metadata,
  artifact/journal gate는 두 mutation 경로 모두 유지한다.
- rollback에는 기존 apply pending, exact backup, no rollback pending, fresh successful
  current read 및 별도 confirmation이 계속 필요하다. SetSelected/Common recovery/AUTO
  disable/Load/Activate 호출은 추가하지 않았다. sel_db before-image만 복원한다.
- 기존 `preflight_allowed` 로그는 apply gate다. 별도 `preflight_rollback_allowed`,
  `active_known`, `pending_none`을 함께 출력한다. known non-Common에서 첫 값만 false인
  것은 rollback 금지를 의미하지 않는다. 최종 outcome 및 나머지 gate를 함께 읽는다.

## Exact immutable backup 재사용

`SaveBefore()`는 현재 fresh baseline이 STOCK_EXACT이며 전체 journal validation이 성공한
경우만 진행한다. 파일이 없으면 기존 O_EXCL/write/file+dir fsync/reread 경로를 사용한다.
파일이 있으면 기존 Read()의 regular/owner/group/0600/nlink=1/SELinux label 검사를 거쳐
STOCK_EXACT + saved bytes == fresh baseline인지 확인한다. overwrite/delete/rename/truncate는 없다.

이전 invocation이 fsync 완료 직전에 종료됐을 가능성도 있으므로 재사용 시 검증한
read-only descriptor와 directory에 fsync를 다시 수행한다. fsync 실패도 block한다.
이는 modem retry가 아닌 local backup durability 확인이다. inode/mtime/ctime 및 bytes가
변하지 않는 테스트를 작성했다. atime은 filesystem mount policy에 따라 읽기로 변할 수 있다.

기존 pending은 여전히 RunExperiment에서 baseline/SaveBefore 전에 apply를 차단한다.
Torn/mismatch/unknown/잘못된 metadata backup을 고치거나 지우는 기능은 없다.

## 변경 파일

모든 소스 경로는 `device/oneplus/guacamole/` 기준이다.

- `efs_probe/include/w1_experiment.h`, `src/w1_experiment.cpp`: rollback 전용 gate.
- `pdc_probe/include/w1_pdc_readonly.h`, `src/w1_pdc_readonly.cpp`: exact-known/pending 상태.
- `efs_probe/src/w1_main.cpp`: typed 상태 전달 및 분리된 gate report.
- `efs_probe/include/w1_journal.h`, `src/w1_journal.cpp`: immutable exact backup reuse/durability.
- `efs_probe/tests/w1_experiment_test.cpp`: C++ 테스트 20개 추가, 총 선언 64개.
- `efs_probe/tests/w1_static_test.py`: 정적 테스트 2개 추가, 총 12개.
- `efs_probe/MACRO_C_FINDINGS.md`, 이 문서: 복구 계약 갱신.
- 외부 `patches/MACRO_C_INTEGRATION_README.md`: fastboot 및 rollback 설명 정정.

새 테스트는 gate 분리, unknown/pending/profile/AUTO 차단, 강한 rollback confirmation,
backup exact reuse/metadata/link/hash 거부, orphan backup 재사용 후 실제 journal checkpoint,
new coordinator에서도 permanent pending 차단을 다룬다. PDC mutation/destructive journal
API가 추가되지 않았는지 정적 테스트도 추가했다. 기존 테스트를 삭제하지 않았다.
**64/12는 소스 선언 수이며 실행 PASS를 뜻하지 않는다.** 기존 PDC/EFS suites도 유지한다.

## 첫 통합 build/test 명령

기존 Macro C의 단일 통합 build/test 명령은 그대로 유효하다. 새 모듈이나 별도 hardware
round가 필요하지 않다. 아래는 사용자가 실행할 명령이며 이 작업에서는 실행하지 않았다.

```bash
(
  set -e
  cd "$HOME/android/pixelos"
  source build/envsetup.sh
  lunch custom_guacamole cp2a userdebug
  m guacamole_pdc_probe_test guacamole_efs_probe_test guacamole_w1_experiment_test \
    guacamole_pdc_probe guacamole_pdc_mapping_probe guacamole_pdc_carrier_probe \
    guacamole_efs_probe guacamole_efs_experiment guacamole_efsd Settings selinux_policy
  out/host/linux-x86/nativetest64/guacamole_pdc_probe_test/guacamole_pdc_probe_test \
    --gtest_output=xml:/tmp/guacamole-macro-c-pdc.xml
  out/host/linux-x86/nativetest64/guacamole_efs_probe_test/guacamole_efs_probe_test \
    --gtest_output=xml:/tmp/guacamole-macro-c-efs.xml
  out/host/linux-x86/nativetest64/guacamole_w1_experiment_test/guacamole_w1_experiment_test \
    --gtest_output=xml:/tmp/guacamole-macro-c-w1.xml
  PYTHONDONTWRITEBYTECODE=1 python3 device/oneplus/guacamole/efs_probe/tests/w1_static_test.py
  PYTHONDONTWRITEBYTECODE=1 python3 device/oneplus/guacamole/efs_probe/tests/korea_sel_db_generator_test.py
)
```

모든 exit=0, failures=0/errors=0 및 SELinux build 성공이 필요하다. 그 후 같은 lunch 환경에서:
```bash
m systemimage vendorimage
```

## 정정된 guacamole 배포 절차

사용자가 검증한 이 bootloader에서는 `fastboot getvar is-userspace`가
`FAILED (remote: 'GetVar Variable Not found')`를 반환한다. 이것을 실패 gate로 보지 않으며
`is-userspace: no` 응답을 요구하지 않는다. 해당 명령 자체는 필요 없다.

사용자가 **bootloader fastboot 화면**으로 진입했음을 확인한다. fastbootd를 사용하지 않는다.
각 단계 결과를 확인하며 한 줄씩 진행한다.

```bash
fastboot getvar current-slot
```

`current-slot: a`를 확인한 경우에만:
```bash
fastboot flash system_a "$HOME/android/pixelos/out/target/product/guacamole/system.img"
fastboot flash vendor_a "$HOME/android/pixelos/out/target/product/guacamole/vendor.img"
```

두 image 모두 **Writing OKAY**를 확인한 후:
```bash
fastboot reboot
```

다른 current-slot 또는 불명확한 응답이면 중단한다. set_active는 사용하지 않는다.
플래시 후 기존 Macro C read-only inspect부터 수행하고 로그를 검토한다. 이번 수정은
최초 EFS write/rollback 실행 승인이 아니다.

## 보존한 조건 / patch

AUTO CLOSED와 기존 slot0.pending, apply one-shot, no retry, fresh process별 I/O,
no automatic rollback, production BLOCKED_MAPPING, Settings disabled를 유지한다.
SELinux/worker/wire/production/Settings/Soong module 정의를 수정하지 않았다.
truncate+chunk write는 atomic replacement가 아니다.

`0029-macro-c-recovery-prebuild-hardening.patch`는 C 적용 후 dirty-tree 기준 delta다.
소스에 이미 적용되므로 다시 apply하지 않는다. `macro_c1-local/`에 pre/post SHA256,
images, forward/reverse patch와 scratch apply/reverse byte 비교 결과를 보관한다.
기존 Macro C patch 및 그 checksum 목록은 보존한다. integration README만 C.1로 갱신했으므로
기존 MACRO_C_SHA256SUMS의 README hash는 이전 문서 snapshot의 값이다. 이전 README 원본과
새 문서의 hash는 C.1 manifest에 별도로 보관한다.

Source-only rollback이 필요하면 후속 변경을 확인한 뒤 C.1 reverse patch만 검사/적용한다.
0026~0028이나 live journal을 되돌리는 작업이 아니다. 외부 integration README를 되돌릴 때도
C.1 doc-pre 사본과 현재 hash를 확인하며 수행한다. 자동 rollback/삭제 명령은 추가하지 않았다.
