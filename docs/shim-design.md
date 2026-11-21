# marble-shim 설계 — ABL에서 XNU로 가는 부트 shim

작성: 2026-11-21. 상태: 설계 단계이고, 실기기에서 검증한 것은 아직 없음.
선행 문서: [hardware-inventory.md](hardware-inventory.md)

참고 소스 (scratchpad에서 직접 읽은 것):
- ios-sweet `shim/` (MIT, 2026-10-23 `45263be`). SM7150 대상. ABL → shim → XNU 경로를 실제로 성공시킨 구현.
- open-mapple `mapple_loader/shim/qcom/` (MIT). ios-sweet shim의 일반화 버전으로, 메모리 홀 목록, 시리얼/시드, MDP 레지스터 기반 splash 탐지가 추가됨.
- Qualcomm ABL `LA.VENDOR.1.0.r2-17000-WAIPIO.QISI15.0`: `QcomModulePkg/Library/BootLib/BootLinux.c`, `FastbootLib/FastbootCmds.c`

---

## 1. 목표 / 비목표

**목표**
- 아무것도 플래시하지 않고 `fastboot boot marble-shim.img` 한 번으로 XNU에 진입.
- 재부팅하면 Android로 그대로 돌아감.
- shim이 실패하면 화면에 원인을 표시하고, 자동으로 fastboot 모드로 리셋.

**비목표 (shim 범위 밖)**
- 디스플레이 재초기화, 클럭/레귤레이터 설정. ABL이 남긴 상태를 그대로 사용함.
- 듀얼부트, 영구 설치.
- iOS 17+ 대응 (PAC 키, SPTM 등). 1단계는 iOS 16.7.x와 iPhone X ADT만 다룸.

## 2. 부트 체인

```
PBL → XBL → (Gunyah @EL2, TZ @EL3) → ABL (언락)
  └─ fastboot boot marble-shim.img   ← boot.img header v2
       kernel  = shim Image (arm64 Linux 헤더를 흉내 냄)
       ramdisk = XNU payload ("MARBLXNU": kernelcache 세그먼트 + ADT + cmdline)
       dtb     = stock vendor_boot에서 뽑은 base DTB (ukee/cape)
  └─ ABL: DTB 선택 (msm-id 591, board-id 0x10008) + flash dtbo 오버레이 적용
          + /chosen(initrd, bootargs, kaslr-seed), memory 노드 채움
          → shim(x0 = DTB), EL1, MMU/D-cache off
  └─ shim: FDT 파싱 → splash FB 탐지 → 메모리 홀 계산
          → XNU 세그먼트 복사, ADT 패치, boot_args 작성
          → XNU _start(x0 = boot_args PA), MMU off
```

EL1에서 시작함: SM8450 계열은 Gunyah 하이퍼바이저가 EL2를 점유하고 있음 (`gh_*` 모듈, `hyp_mem`, `qheebsp_reserved_mem` 근거). iOS 16 XNU는 EL1 커널이라 문제없음.

## 3. 핵심 결정: boot image header v2를 사용

marble의 stock은 header v4 + vendor_boot 구성임. 하지만 **ABL은 플래시된 이미지가 아니라 `fastboot boot`으로 받은 이미지의 헤더 버전을 보고 부팅 경로를 정함** ✅:

- `FastbootCmds.c:2789 CmdBoot`: 받은 이미지를 `Info.Images[0]`로 넘김. 헤더 버전 제한은 없음.
- `BootLinux.c:945`: `HeaderVersion < 3`이면 vendor_boot를 아예 읽지 않음.
- `BootLinux.c:487-495`: v2이면 DTB를 **부트 이미지 자체의 dtb 섹션**에서 찾음.
- `BootLinux.c:512 LoadAndValidateDtboImg`: dtbo는 버전과 관계없이 flash의 dtbo 파티션에서 적용함. marble 오버레이(터치, PMIC 등)가 그대로 들어오므로 오히려 좋음.

그래서 ios-sweet과 같은 **v2 이미지 + stock base DTB 동봉** 방식을 1안으로 함.

⚠️ 위험: Xiaomi가 ABL을 수정해서 v2를 거부할 수 있음. 이 경우 2안으로 감.

**2안 (fallback): v4 이미지 + flash vendor_boot 사용**
- ABL이 램디스크를 `vendor_ramdisk ‖ 우리 ramdisk ‖ bootconfig` 순서로 이어 붙이고 (`BootLinux.c:848-876`), `/chosen` initrd 범위는 그 전체를 가리킴.
- 따라서 shim은 initrd 범위 안에서 페이지 단위로 payload magic을 스캔해야 함. vendor_ramdisk 크기만큼 오프셋이 생기기 때문.
- 스캔 코드는 1안에서도 넣어 둠 (비용이 거의 없음).

## 4. 메모리 맵

DRAM은 `0x80000000`부터 시작함. 8 GB 모델은 대략 `0x80000000–0x100000000`(2 GB)와 `0x880000000~`(6 GB)일 것 ⚠️ (`/proc/iomem`으로 확인 필요).

`cape-reserved-memory.dtsi` 기준 carveout ✅:

| 범위 | 내용 |
|---|---|
| `0x80000000–0x80600000` | hyp_mem (Gunyah) |
| `0x80600000–0x85e00000` | XBL log, AOP, smem, cpucp, cdsp heap, video |
| `0x88000000–0xa2e00000` | SLPI, CDSP, IPA, GPU microcode, SPSS, **MPSS 306 MiB**, CVP, camera, ADSP |
| `0xa6b80000–0xa7000000` | xbl_ramdump, xbl_sc, global_sync |
| `0xa7000000–0xa7400000` | **ramoops** (Xiaomi, console 2 MiB + pmsg 2 MiB) |
| `0xb8000000–0xbab00000` | **cont_splash** (43 MiB) |
| `0xe0000000–0xea000000` | qheebsp, cpusys_vm, hyp_reserved, **trust_ui_vm 74 MiB**, tz_stat, tags, qtee |
| 동적 | sp_mem 16 MiB 등 (`alloc-ranges`라서 FDT에 위치가 안 나옴) |

ABL이 이미지를 놓는 위치 ✅ (`BootLinux.c:157-215`): UEFI `QueryBootParams`가 돌려준 `[KernelLoadAddr, +KernelSizeReserved)` 창 안에 커널(shim)을 앞쪽에 놓고, 램디스크(payload)를 창의 끝에 놓고, DTB를 램디스크 아래 2 MiB 자리에 놓음. 창의 실제 크기는 ⚠️. 이 크기가 payload 크기의 상한이 됨.

**XNU 배치 방침**
- 1단계: `phys_base`를 상위 뱅크에 둠. **M1 실측 결과: `0x840000000`부터 5 GiB가 연속 (§11)**. (원래 추정은 `0x880000000`이었음.) carveout이 없는 연속 영역이고, Apple의 DRAM base(`0x800000000`)와도 대역이 가까움. `mem_size`는 그 뱅크 크기에서 여유분을 뺀 값으로 시작.
- 하위 2 GB는 XNU에 넘기지 않음. 그러면 carveout 문제를 통째로 피할 수 있음. 다만 FB(`0xb8000000`)와 ramoops(`0xa7000000`)는 XNU에서 디바이스 메모리로 매핑해서 사용.
- 2단계: open-mapple `holes.c` 방식으로 넘어감. `/reserved-memory` 전체, payload, DTB를 홀 목록으로 만들어 패치된 XNU pmap에 전달하고, 하위 뱅크도 사용.
- payload가 XNU 목적지와 겹치는지 확인하는 로직(ios-sweet `loader.c`)은 유지.

## 5. marble 상수 (ios-sweet 대비 바뀌는 값)

| 항목 | sweet (SM7150) | **marble (SM7475)** | 근거 |
|---|---|---|---|
| GICD | `0x17a00000` | **`0x17100000`** | `cape.dtsi:584` |
| GICR | `0x17a60000`, PE stride `0x20000` | **`0x17180000`, stride `0x40000`** | `redistributor-stride`. `find_gicr()` 루프의 보폭도 바꿔야 함 |
| 가상 타이머 INTID | 19 (비표준) | **27** (PPI 11) | `cape.dtsi:616` |
| 물리 타이머 INTID | 18 | **30** (PPI 14) | 〃 |
| 타이머 주파수 | 19.2 MHz | 19.2 MHz | 〃 |
| CPU MPIDR | `0x000–0x700` | `0x000–0x700` | `cape.dtsi` cpu@N00 |
| splash FB | `0x9c000000` | **`0xb8000000`** (최대 `0x2b00000`) | `cape-sde-display.dtsi:33` |
| FB 크기 | 1080×2400 | 1080×2400, **실제 stride와 포맷은 MDP에서 읽음** | §6 |
| MDP base | `0x0ae00000` | `0x0ae00000` | `cape-sde-common.dtsi:5` |
| SSPP offsets | VIG 0x5000.., DMA 0x25000.. | VIG `0x5000/7000/9000/b000`, DMA `0x25000/27000/29000/2b000` | `cape-sde-common.dtsi:109` |
| INTF (DSI) | | `0x36000` (INTF1) | `cape-sde-common.dtsi:62` |
| IMEM restart_reason | `0x146aa65c` | `0x146aa65c` (imem `0x146aa000` + `0x65c`) | `cape.dtsi:2201-2215` |
| ramoops | | `0xa7000000`, 4 MiB | `xiaomi-sm8475-common.dtsi:199` |
| UFS HC | `0x1d84000` | `0x1d84000` | `cape.dtsi:1916` |
| PS_HOLD | `0x0c264000` | ⚠️ DT에서 못 찾음. 우선 PSCI `SYSTEM_RESET`만 사용 | |
| 금지 GPIO | | TLMM 28–31, 36–39 | `cape.dtsi` tlmm |

## 6. 모듈 구성

open-mapple `shim/qcom`을 베이스로 fork하고, 차이점은 `marble.h` 한 곳에 모음.

```
shim/
  start.S      arm64 Image 헤더, EL1 진입, 스택, 벡터, PSCI secondary 진입점   (거의 그대로)
  shim.c       마일스톤별 진단 → loader 호출                                   (상수만 교체)
  marble.h     §5의 상수 전부                                                   (신규)
  fdt.c        /chosen(initrd, bootargs, kaslr-seed), /memory, /reserved-memory  (그대로)
  holes.c      reserved-memory + payload + DTB → 홀 목록                         (MTK 전용 #ifdef 제거)
  panel.c      SSPP 레지스터로 splash 주소/stride/포맷 탐지                      (SDE 8.x 오프셋, autorefresh 제거)
  console.c    FB 텍스트 콘솔 + ramoops 동시 기록                                (ramoops 미러 추가)
  loader.c     payload 탐색(magic 스캔), 세그먼트 복사, ADT 패치, boot_args, 점프 (스캔 추가)
  identity.c   androidboot.serialno → ADT serial, kaslr-seed → random-seed       (그대로)
  ufs.c        (진단용) UFS 읽기 테스트                                         (그대로)
tools/
  mkpayload.py kernelcache → MARBLXNU payload (fixup 해결, ADT 생성)
  mkadt.py     iPhone X(D22AP) ADT + marble 값
  mkshimimg.sh mkbootimg --header_version 2 … --dtb stock-base.dtb
```

**panel.c 차이점.** marble은 **video mode** 패널이라 MDP가 매 프레임 SSPP 주소에서 직접 스캔아웃함. 그래서 sweet의 command mode용 autorefresh/TE 처리는 필요 없음. 활성 SSPP(대개 DMA 파이프)에서 `SRC0_ADDR`, `YSTRIDE0`, `SRC_FORMAT`, `SRC_SIZE`를 읽어 FB 위치를 정하고, 읽은 값이 비정상이면 DT 기본값(`0xb8000000`, 1080×2400, stride 4320)으로 대체함.

**ramoops 미러.** 시리얼 포트가 없으므로 shim 콘솔 출력을 `0xa7000000`의 ramoops console 영역에 pstore 형식으로 같이 씀. 리셋 후 Android(또는 TWRP/OrangeFox)에서 `/sys/fs/pstore/console-ramoops*`로 회수함. 이건 ⚠️ 리셋 과정에서 DRAM 내용이 유지된다는 전제가 필요하고, Xiaomi의 warm reset이 이를 보장하는지 확인해야 함.

## 7. 빌드 / 이미지

```bash
# 1. stock vendor_boot에서 base DTB 추출 (한 번만)
python3 unpack_bootimg.py --boot_img vendor_boot.img --out vb && cp vb/dtb stock-base.dtb
```

```bash
# 2. shim + payload → v2 boot.img
mkbootimg --header_version 2 --kernel out/Image --ramdisk out/payload.bin --dtb stock-base.dtb --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 --os_version 15.0.0 --os_patch_level <stock과 같거나 그 이후> --cmdline "console=null" -o out/marble-shim.img
```

```bash
# 3. 테스트 (RAM 부팅, 아무것도 쓰지 않음)
fastboot boot out/marble-shim.img
```

- `vendor_boot.img`는 루팅된 기기에서 `dd`로 뽑거나 stock fastboot ROM에서 가져옴.
- `stock-base.dtb`에는 여러 DTB가 이어 붙어 있음 (ukee, cape, waipio 변종). ABL이 msm-id/board-id로 골라냄.
- ⚠️ `os_patch_level`: ios-sweet은 2035-06으로 설정했음. AVB가 unlocked 상태라 롤백 체크가 의미 없을 가능성이 높지만, ABL 동작을 보고 결정.

## 8. 마일스톤

각 단계는 "실패해도 자동으로 fastboot로 복귀"를 전제로 함 (IMEM restart_reason = `0x77665500` → PSCI reset).

| # | 내용 | 완료 기준 |
|---|---|---|
| **M0** | v2 이미지를 ABL이 받아 shim이 실행됨 | `0xb8000000`에 단색이 칠해짐. 칠해지지 않으면 2안(v4)으로 전환 |
| **M1** | FDT 덤프 | 화면에 DTB 주소, magic, initrd 범위, `/memory` reg, reserved 개수, 패널 이름(bootargs)이 표시됨 |
| **M2** | CPU / PSCI / SMP | `ID_AA64ISAR1_EL1`의 **APA/API/GPA(PAC)**, `ID_AA64MMFR0` **TGran16 지원** 확인. PSCI 버전 확인. CPU_ON으로 7개 코어 MPIDR 응답 |
| **M3** | GIC / 타이머 | GICR(stride `0x40000`)를 찾고, vtimer INTID 27이 100 Hz로 들어옴, SGI 1 |
| **M4** | MMU + 캐시 on 성능, ramoops 미러 | FB fill 시간 비교. 리셋 후 pstore에서 shim 로그를 회수 |
| **M5** | XNU 핸드오프 | payload를 찾고 세그먼트를 복사한 뒤 점프. XNU 초기 fbcon 출력(패치 필요) 또는 panic 화면이 뜸 |

M2에서 **16K granule 미지원**이 나오면 큰 문제임. hackiosx 포팅 문서에 "일부 작은 코어에서 16 KB 페이지가 실패할 수 있다"는 언급이 있음. A510이 여기에 해당하는지는 확인하지 못했으므로, 8개 코어 모두에서 `TGran16`을 읽어 확인해야 함. 이 경우 대안은 두 가지: (a) 큰 코어(A710/X2)만 켜고 A510은 끄기, (b) 4K 페이지 XNU 빌드 (iOS 유저랜드와 호환되지 않을 위험).

## 9. 위험 요소와 대응

| 위험 | 영향 | 대응 |
|---|---|---|
| Xiaomi ABL이 v2를 거부 | M0 실패 | 2안 (v4 + 램디스크 magic 스캔) |
| ~~`KernelSizeReserved` 창이 작음~~ | ✅ 해소 (M0): 창 ≈ 256 MiB (`0xA8000000–0xB8000000`) | — |
| ~~A510의 16K granule~~ | ✅ 해소 (M0): A510 TGran16=1 | 나머지 코어는 M2에서 확인 |
| Gunyah 소유 메모리, 동적 carveout (`alloc-ranges`) | XNU가 그 메모리를 쓰면 즉시 abort | 1단계는 상위 뱅크만 사용. 동적 영역은 `/proc/iomem`과 FDT의 실제 값으로 홀 목록 작성 |
| EL1 물리 타이머 트랩 (M3에서 확인) | `CNTP_*` 접근 시 UNDEF | XNU는 가상 타이머(CNTV, INTID 27)만 사용. rtclock/idle 코드에서 CNTP를 쓰면 패치 |
| APSS watchdog | 몇 분 뒤 리셋 | M1~M4에서 실행 시간을 늘려 관찰. 필요하면 XNU에 watchdog pet 드라이버 추가 (ios-sweet은 `wdt=-1`로 유저랜드 쪽만 처리) |
| DSC video FB 포맷 오판 | 화면이 깨짐 | panel.c의 SSPP 값을 화면에 덤프해서 확인 |
| 대용량 `fastboot boot` 후 ABL 행 | 테스트 지연 | ios-sweet NOTES의 사례: 재진입 필요. `fastboot getvar` 반복 금지 |

## 10. 시작 전 체크리스트

- [ ] 부트로더 언락 (`fastboot flashing unlock` / Mi Unlock)
- [ ] `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `boot_a/b`, `vendor_boot_a/b`, `dtbo_a/b` 백업
- [ ] `/sys/firmware/fdt`, `/proc/iomem`, `/proc/cmdline`, `/proc/cpuinfo` 덤프 ([hardware-inventory.md §12](hardware-inventory.md))
- [ ] stock `vendor_boot`에서 `stock-base.dtb` 추출
- [ ] cross toolchain (`aarch64-linux-gnu-gcc` 또는 clang `--target=aarch64-none-elf`), `mkbootimg`
- [ ] M0 shim (FB 단색 칠하기 + 20초 후 fastboot 복귀)만으로 첫 테스트

---

## 11. 실기기 결과 기록

### 2026-11-21 — M0, header v4 (2안) ✅

- 기기: POCO F5 `marble`, 포팅 ROM(Android 17, S23 표시) + 순정 vendor `OS3.0.4.0.VMRMIXM`, slot `a`, unlocked (`orange`), 루트 없음
- 이미지: `shim/out/marble/marble-shim-m0-v4.img` (header v4, kernel 80 KiB, 빈 cpio, cmdline `m0.hold=1`)
- `fastboot boot`: `Sending OKAY`, `Booting OKAY`
- 결과: **남색 화면 + 글자 표시** → 아래 항목이 확인됨
  - Xiaomi ABL이 v4 이미지를 받아서, 폰의 vendor_boot DTB와 함께 shim을 실행함
  - splash 표면이 `0xb8000000`에 있고, stride 4320으로 글자가 정상 표시됨
  - 따라서 **2안만으로 DTB를 추출하지 않고도 bring-up을 진행할 수 있음.** 1안(v2)의 수용 여부는 별도로 확인 필요
- 참고: Xiaomi ABL에서 `fastboot getvar`(product, current-slot, unlocked)를 실행했지만 멈춤 현상은 없었음

**화면 값 (사진 판독)**

| 항목 | 값 | 해석 |
|---|---|---|
| 색 막대 | 빨강 · 초록 · 파랑 · 흰색 순서 정상 | splash 포맷 **a8r8g8b8 확인**. stride `0x10E0`(4320) 정상 |
| EL | 1 | 예상대로 EL1 (EL2는 Gunyah) |
| IMAGE | `0xA8000000` | **ABL 커널 로드 주소** (`KernelLoadAddr`). ramoops(`0xA7000000–0xA7400000`) 바로 위 |
| FDT | `0xB7205000`, `0x9ED52` (≈636 KiB) | magic 정상. 2 MiB 슬롯 안에 있음 |
| INITRD | `0xB7406000 – 0xB7FFE53C` (≈12 MiB) | vendor ramdisk + 우리 cpio + bootconfig. 끝이 `0xB8000000` 바로 아래 → **ABL 로드 창 ≈ `0xA8000000–0xB8000000` (256 MiB)** |
| MEM0 / SIZE0 | `0x80000000` / `0x6A000000` | 첫 뱅크 `0x80000000–0xEA000000`. qtee carveout 끝에서 잘림 |
| BANKS | 5 | `/memory` reg 엔트리 5개. 전체 목록은 M1에서 덤프 |
| RESV | `0x36` = 54 | reserved-memory 노드 54개 |
| ARGS | `0x3E3` = 995 B | bootargs 길이 |
| PANEL | M16T 36 02 0A | 이 기기의 패널 변종 |
| MIDR | `0x410FD463` | ARM **Cortex-A510 r0p3** (부트 CPU = 작은 코어) |
| MPIDR | `0x81000000` | **MT=1** (affinity 이동). 코어 번호는 Aff1 → DT의 `cpu@N00`과 일치 |
| ISAR1 | `0x0010011101211052` | **APA=5 (FEAT_FPACCOMBINE까지, QARMA5)**, API=0, **GPA=1**, LRCPC=2, I8MM=1 |
| ISAR2 | 0 | QARMA3 없음 |
| MMFR0 | `0x0000022200101122` | **PARange=2 (40-bit PA)**, ASID 16-bit, **TGran16=1 (16K 지원)**, TGran4/64 지원, stage-2 16K 지원 |
| CNTFRQ | `0x124F800` | 19.2 MHz ✅ |
| PSCI | V=`0x10001`, SMCCC=`0x10001`, RST2 FEATURES=0 | PSCI 1.1, SMCCC 1.1, **SYSTEM_RESET2 지원** |

**결론**
- **PAC: 있음.** 표준 QARMA5 PAuth에 FPAC가 결합된 수준이라, arm64e 유저랜드의 PAC 명령은 하드웨어로 실행 가능함. 단, 두 가지 차이가 있음 (⚠️ 추론):
  - Apple XNU의 키 관리는 Apple 전용 시스템 레지스터를 쓰므로 패치가 필요함.
  - FPAC 때문에 인증 실패가 즉시 트랩됨. Apple A12–A14는 독(poison)값을 남기는 방식이라 동작 의미가 다름.
- **16K 페이지: A510에서 지원.** §8 M2의 최대 위험이 해소됨. A710/X2 코어도 M2에서 코어별로 확인할 것.
- **payload 예산:** ABL 창 약 256 MiB에서 FDT·initrd 영역을 빼면 약 240 MiB. iOS 16 kernelcache(수십 MiB)와 ADT는 충분히 들어감. §9의 "창이 작음" 위험은 해소됨.
- **reboot-to-fastboot 후보:** PSCI `SYSTEM_RESET2` 지원. 웜 리셋이면 DRAM이 유지되므로 ramoops 회수에 유리함 (M4에서 확인).
- **다음:** M1에서 `/memory` 5개 뱅크 전체, reserved-memory 54개, bootargs 본문을 덤프함. M2에서 8코어 SMP와 코어별 ID 레지스터를 확인함.

### 2026-11-21 — M1~M3 코드 준비 (실기기 미실행)

- `shim/` 5페이지 구성: 0 M0 요약 / 1 메모리 뱅크·initrd·bootargs / 2 reserved-memory 전체 / 3 SMP (CPU_ON + 코어별 ID) / 4 GICv3·타이머·SGI. EL1 물리 타이머는 페이지 4를 띄운 **뒤에** 시험함
- QEMU(`virt,gic-version=3`, `-smp 8`, reserved-memory를 넣은 시험 DTB)에서 확인한 것:
  - 8코어 CPU_ON이 모두 OK, AFFINITY_INFO는 부트 코어만 ON이고 나머지는 OFF
  - vtimer 25회/300 ms, SGI 10/10, ptimer 25회
  - reserved-memory 표시: no-map, 다중 reg, 동적 영역, 상위 뱅크 주소 모두 정상
  - 전체 흐름이 PSCI 리셋까지 정상 종료
- 실기기에서 볼 것: 메모리 5뱅크와 reserved 54개의 실제 배치, X2/A710 코어의 PAC·16K, Gunyah 아래에서 GIC 재분배기 stride(`0x40000`)와 affinity(MT=1) 매칭, vtimer/ptimer 동작
- 이미지: `shim/out/marble/marble-shim-v4.img`

### 2026-11-21 — M1~M3 실기기 ✅ (EL1 물리 타이머만 ❌)

이미지는 `marble-shim-v4.img` (`shim.hold=1`). 페이지 0~4가 모두 정상으로 나왔고, 마지막 EL1 물리 타이머 시험에서 예상했던 PANIC이 났음.

**M1 — `/memory` (5개 reg 엔트리, 합계 7951 MiB = 8 GB 모델)**

| # | 범위 | 크기 |
|---|---|---|
| 0 | `0x0080000000 – 0x00EA000000` | 1696 MiB |
| 1 | `0x00F4600000 – 0x0100000000` | 186 MiB |
| 2 | `0x0800000000 – 0x0839700000` | 919 MiB |
| 4 | `0x0839D00000 – 0x083BB00000` | 30 MiB |
| 3 | `0x0840000000 – 0x0980000000` | **5120 MiB (연속)** |

- 빠진 영역: `0xEA000000–0xF4600000`(166 MiB), `0x839700000–0x839D00000`(6 MiB), `0x83BB00000–0x840000000`(69 MiB). 합계 약 241 MiB이고, Gunyah, TZ, 다른 VM 몫으로 보임 (⚠️ 추정)
- 상위 DRAM은 **`0x800000000`부터 시작**함. Apple SoC의 DRAM base와 같은 주소
- initrd `0xB7406000–0xB7FFE53C`(12257 KiB), kaslr-seed와 rng-seed 있음
- bootargs(997 B)에서 확인한 것:
  - `mtdoops.fingerprint=24.0-20261103-NIGHTLY-marble` → **flash의 vendor_boot는 LineageOS 24.0 nightly(2026-11-03) 빌드.** 따라서 DTB는 인벤토리에서 분석한 LineageOS `sm8450-devicetrees` 소스와 같은 계열임
  - `console=ttyMSM0,115200n8`, `msm_geni_serial.con_enabled=0`
  - `msm_drm.dsi_display0=qcom,mdss_dsi_m16t_36_02_0a_dsc_vid`
  - `block2mtd.block2mtd=/dev/block/sda15,2097152`, `mtdoops.*`: Lineage가 oops 로그를 `sda15`에 기록함. 나중에 로그 회수 경로로 쓸 수 있음
  - `bootinfo.pureason=0x40001`, `bootinfo.pdreason=0x2`, `hwid.project=15`

**M1 — `/reserved-memory` (54개 노드: 고정 603 MiB, 동적 452 MiB)**
- 고정 영역은 모두 하위 4 GB 안에 있음. `cape-reserved-memory.dtsi`와 일치 (hyp `0x80000000`, MPSS `0x8BC00000`+`0x13200000`, ADSP, CDSP, SLPI, IPA, SPSS, qheebsp/cpusys_vm/hyp_reserved/trust_ui `0xE0000000~`, tz_stat, tags, qtee `0xE9B00000`+`0x500000`)
- ramoops `0xA7000000`+`0x400000`, splash `0xB8000000`+`0x2B00000` (splash는 no-map 아님)
- `trust_ui_vm_region`, `ramoops_region`은 reg/size가 표시되지 않음. 다른 셀 형식이거나 비활성 노드로 추정
- 동적 16개: sp, linux,cma(32 MiB), va_md_mem, user_contig, qseecom(20 MiB), non_secure_display(164 MiB), demura_heap(40 MiB), qseecom_ta, cnss_wlan, audio_cma, adsp_heap, sdsp, cdsp_eva, mem_dump(48 MiB) 등. 모두 Linux가 하위 영역(`alloc-ranges`)에서 잡는 몫이라 XNU에는 해당 없음
- **상위 5 GiB 뱅크(`0x840000000–0x980000000`)에는 reserved 영역이 하나도 없음**

**M2 — SMP: 8코어 모두 ✅**

| CPU | MPIDR | 코어 | rev | PAC | 16K | PA |
|---|---|---|---|---|---|---|
| 0 (부트) | `0x81000000` | A510 | r0p3 | APA5 GPA1 | TG16=1 | 40-bit |
| 1–3 | `0x81000100–300` | A510 | r0p3 | APA5 GPA1 | TG16=1 | 40-bit |
| 4–6 | `0x81000400–600` | A710 | r2p0 | APA5 GPA1 | TG16=1 | 40-bit |
| 7 | `0x81000700` | X2 | r2p0 | APA5 GPA1 | TG16=1 | 40-bit |

- PSCI CPU_ON이 7개 모두 OK. CPU_OFF 후 AFFINITY_INFO는 `N F F F F F F F`(부트 코어만 ON)
- **모든 코어가 PAC와 16K를 지원.** 16K 때문에 코어를 꺼야 할 위험은 완전히 해소됨
- 모든 코어가 `SVE0`: SVE가 숨겨져 있음 (Gunyah 또는 펌웨어). Apple 코어도 SVE가 없으므로 iOS와는 무관
- MMFR2 `0x1221011110101011`: VARange 0(48-bit VA)

**M3 — GIC / 타이머**
- GICD `0x17100000`: TYPER `0x0168001F`(SPI 992개, LPI 있음, **SecurityExtn=0**), IIDR `0x47000070`(구현자 코드 `0x070`, ARM의 `0x43B`가 아님), PIDR2 ArchRev 3
  - → **Gunyah가 에뮬레이션하는 가상 GICv3로 판단** (⚠️ IIDR 해석은 추정)
- 재분배기 8개: `0x17180000 + n×0x40000`, affinity `0x000…0x700`(Aff1 = 코어 번호), 7번이 LAST. **stride `0x40000` 확인**
- GICD_CTLR `0xD0 → 0xD3`, WAKER `6 → 0`, IGROUPR0는 처음부터 `0xFFFFFFFF`, ICC_SRE_EL1 = 1 (sysreg 인터페이스 사용 가능)
- **vtimer(INTID 27): 300 ms에 정확히 30회 ✅**, **SGI 1: 10/10 ✅**
- **EL1 물리 타이머: ❌ UNDEF.** PANIC 내용: `STAGE gic ptimer`, `ESR 0x02000000`(EC=0 Unknown, IL=1), `ELR 0xA80051F8`(`cntp_*_el0` 쓰기), `X1=0x1E`(30)
  - → Gunyah가 `CNTP_*` 접근을 트랩하고 UNDEF를 주입함. `CNTPCT_EL0` 읽기는 문제없음 (지연 루프가 이 레지스터로 동작함)
  - → **XNU는 가상 타이머(CNTV, INTID 27)만 써야 함.** XNU의 rtclock이 물리 타이머를 쓰는 부분이 있으면 패치 대상 (ios-sweet도 SM7150에서 vtimer INTID 19를 사용)

**XNU 배치에 대한 결론 (§4 갱신)**
- 1단계: `phys_base = 0x840000000`, `mem_size = 5 GiB`(`0x140000000`). carveout이 없는 단일 연속 영역
- 2단계 (선택): `0x800000000`부터 시작하고, 홀 2개(`0x839700000–0x839D00000`, `0x83BB00000–0x840000000`)를 홀 목록으로 넘기면 약 6 GiB 사용 가능. 하위 4 GB는 계속 제외

### 2026-11-21 — M5a: XNU 로더 + 시험 커널(tk) (QEMU ✅, 실기기 대기)

- `shim/loader.c`: initrd 안에서 payload magic(`MARBLXNU`, ios-sweet의 `SWEETXNU`도 허용)을 **바이트 단위로 스캔**함. v4에서는 vendor ramdisk가 앞에 붙으므로 오프셋이 임의임
  - 헤더 검증 항목: 버전, 세그먼트 범위, entry, `[phys_base, mem_size)`가 하나의 `/memory` 뱅크 안에 있는지, 그리고 payload·FDT·FB·shim·static reserved와 겹치지 않는지
  - 세그먼트 복사는 정렬된 64비트 접근만 씀. 원본이 어긋나 있으면 두 워드를 합쳐서 복사함
  - ADT와 boot_args(Revision 2)를 놓고, GIC를 정지한 뒤 MMU off 상태로 `x0 = boot_args`를 넘기며 점프
- payload 헤더 형식은 ios-sweet `mkpayload.py`와 바이트 단위로 같음 → ios-sweet 도구로 만든 XNU payload를 그대로 받을 수 있음
- payload가 있으면 shim은 바로 페이지 5(핸드오프)로 감. `shim.diag=1`이면 진단 페이지 0~4를 먼저 돌림
- marble은 `PTIMER_TRAPS=1`: EL1 물리 타이머 시험을 건너뜀 (M3 결과)
- `shim/tk.c` + `tools/mkpayload_tk.py`: 가짜 XNU. boot_args 전체를 표시하고, ADT 복사본을 검증하고, `[physBase, physBase+memSize)` 전체에 64지점 쓰기/읽기 RAM 탐침을 함
- QEMU (`-m 8G`, 13바이트 접두부로 정렬 어긋남을 흉내):
  - payload를 initrd `+0xD`에서 찾음
  - tk가 physBase에 로드됨, boot_args REV 2 / VER 2, ADT magic OK·불일치 0바이트, RAM 탐침 63/63 OK ✅
- 개발 중 버그 2개를 수정함:
  - (a) 4바이트 정렬 스캔이 어긋난 payload를 놓침 → 바이트 단위 스캔으로 변경
  - (b) strict aliasing 때문에 `uint64_t` 복사 뒤의 `uint32_t` 헤더 필드 읽기 순서가 바뀜 → `-fno-strict-aliasing` 추가
- 폰 이미지: `shim/out/marble/marble-shim-tk-v4.img` (tk: phys `0x840000000`, 5 GiB, `tk.hold=1`)

## 12. M5b 계획 — 진짜 XNU

**기반:** ios-sweet XNU (MIT). 구성은 xnu-8796(iOS 16 세대)에 패치 0001–0081을 얹은 것이고, 빌드는 blacktop/darwin-xnu-build로 함.
- SWEET machine config는 VMAPPLE을 바탕으로 만든 것이라 GICv3, vtimer, PSCI SMP, framebuffer 콘솔, 내장 HFS/AMFI/IOSurface/IOMFB/HID 패치를 모두 포함함
- payload 형식이 같으므로 shim 쪽 변경은 필요 없음

**marble에 맞게 바꿀 것** (sweet → marble):

| 항목 | sweet 값 | marble 값 | 위치 |
|---|---|---|---|
| vtimer INTID | 19 | **27** | `SWEET_VTIMER_INTID` (0004), ADT 값 (0017) |
| GICR 프레임 크기 | `GICR_PE_SIZE 0x20000` 고정 | **`0x40000`** (Gunyah vGIC, M3) | 0004 `SWEET.h` → ADT나 `GICR_TYPER`에서 읽도록 |
| GIC 보안 상태 | 2개 (TZ가 Group 0 소유) | **1개** (`SecurityExtn=0`, Gunyah) | 0004의 CTLR 비트 처리. M3에서는 `0x13` 쓰기로 동작 확인 |
| GICD/GICR 주소 | `0x17a00000/0x17a60000` | `0x17100000/0x17180000` | `mkadt.py` |
| DRAM | `PHYS_BASE`, `HIGH_PHYS_BASE 0x100000000` | **`0x840000000`, 5 GiB** | `mkadt.py` |
| FB | `0x9c000000` | `0xb8000000`, stride 4320 | `mkadt.py` `sweet-fbcon`, 0003/0061 |
| CPU | A55×6 + A76×2, `phys_id ≤ 0x500` 검사 | A510×4 + A710×3 + X2, MPIDR `0x000–0x700`, MT=1 | 0065/0078 `SweetCPU.cpp` |
| `-march` | `armv8.2-a+crypto` | 그대로 (모든 코어가 v8.2 이상) | 0004 |
| 물리 타이머 | — | 사용 금지 (UNDEF) | rtclock/idle에서 `CNTP_*`를 쓰는 곳 점검 |
| 터치 / 키 / UFS / USB / 배터리 | GT9896 I2C, SPMI(PM6150), UFS 2.x, … | GT9916S **SPI**, SPMI arb v7 **2버스**(PMK8350 PON, PM8350 GPIO6), UFS 3.1, DWC3+eUSB2, glink | 기기별 드라이버 → 처음에는 비활성/스텁 |

**순서**
1. **M5b-1:** 이 Mac(Xcode 27)에서 darwin-xnu-build로 ios-sweet 커널을 그대로 빌드하고, 우리 shim의 QEMU 경로(`qemu-tk`와 같은 방식, `-initrd`에 payload)로 부팅함. 빌드 환경과 payload 호환성을 폰 없이 검증하는 단계. ⚠️ ios-sweet CI는 macOS 13.5 SDK와 당시 Xcode를 썼으므로, Xcode 27에서 빌드가 깨질 위험이 있음
2. **M5b-2:** MARBLE machine config와 `mkadt.py`의 marble 값(위 표)을 적용하고 폰에서 부팅함. 완료 기준: XNU 초기 fbcon(`kprintf`) 텍스트가 화면에 출력됨. 이 시점부터 화면 사진만으로 디버깅하기 어려워지므로 M4(로그 회수)가 필요해짐
3. **M5b-3:** SMP 8코어, vtimer, 루트 파일시스템(ramdisk `rd=md0`부터 시작), launchd까지

### 2026-11-21 — M5a 실기기 ✅ (shim → tk 핸드오프)

- 이미지: `marble-shim-tk-v4.img`. `fastboot boot` → shim 페이지 5(사진은 놓침) → 15초 뒤 점프 → **tk 초록 화면**
- tk가 표시한 값:
  - EL 1, `LOADED AT 0x840000000`, `X0 0x84002C000` (= boot_args PA), **`LOADED AT PHYSBASE: YES`**
  - boot_args REV 2 / VER 2, VIRTBASE `0xFFFFFFF007004000`, PHYSBASE `0x840000000`, MEMSIZE `0x140000000`, TOPOFKERNELDATA `0x840030000`
  - VIDEO `0xB8000000` 1080×2400 ROW 4320 DEPTH 32, DEVICETREEP `0xFFFFFFF007020000` LEN `0x10000`, MEMSIZEACTUAL `0x140000000`, cmdline `tk.hold=1`
  - **ADT `0x84001C000`: MAGIC OK, 0 BAD BYTES**
  - **RAM PROBE `0x840000000–0x980000000`: 63 OK 0 BAD**
- 결론:
  - 로더 검증을 통과함 (점프했다는 사실 자체가 증거)
  - vendor ramdisk 뒤 임의 오프셋의 payload를 찾아내고, 정렬 병합 복사가 정상 동작함
  - boot_args와 ADT 배치가 맞음
  - **Gunyah stage-2가 상위 5 GiB 전체에 대한 EL1 쓰기/읽기를 허용함** → XNU의 RAM으로 확정
- 다음: §12 M5b-1 (ios-sweet XNU 로컬 빌드 → QEMU에서 shim으로 부팅)

### 2026-11-21 — M5b-1: ios-sweet XNU를 QEMU에서 shim으로 부팅 ✅

- 로컬 빌드 불가: xnu-8796에는 Xcode 15.x iig가 필요함 (Xcode 16 이후는 IORPC ABI 불일치, ios-sweet CI 주석 참고). 그리고 KDK 13.5가 필요한데 이 Mac에는 없음. 이 Mac에는 Xcode 27 beta만 있음
  - → 빌드는 GitHub Actions(macos-14)로 하기로 결정. `.github/workflows/xnu.yml`과 `third_party/ios-sweet/`(MIT, `45263be`), `xnu/marble-patches/`를 준비함 (원격 저장소 생성과 push는 아직 안 함)
- 검증에는 ios-sweet CI 아티팩트를 사용함: `xnu-kernel` (run 39158169212, 커밋 `fdad0de`, 2026-10-18), `kernel.development.sweet` (Mach-O arm64, 17.2 MB, SHA256 일치)
- 같은 커밋의 `mkadt.py --qemu` + `mkpayload.py`로 만든 `SWEETXNU` payload(16.6 MB) → 우리 shim(`BOARD=qemu`, FB `0x9c000000`) → `make qemu-xnu`
- 결과 (60초 시점 화면, `docs/m5b1-qemu-xnu-060.png`):
  - `iBoot version: sweet-shim`
  - SWEET platform expert, AMFI, AKS, WDT, IOSurface, HID, AppleCLCD(1080×2400 stride 4320 at `0x9c000000`), `SWEET CPU: cpu=0 ... SGI1 OK`, HFS, BSD 네트워크 스택
  - 마지막 줄 `Waiting on <dict>... IOMedia ... Apple_HFS`: 루트 디바이스를 기다리며 멈춤. QEMU에는 UFS나 램디스크가 없으므로 예상된 동작
- 결론: **우리 shim의 payload 탐지, 복사, ADT, boot_args 핸드오프가 실제 XNU에서 동작함** (ios-sweet 형식과 호환 확인). 남은 일은 커널과 ADT를 marble에 맞추는 것 (§12 M5b-2)

### 2026-11-21 — M5b-1 빌드 파이프라인 (GitHub Actions)

- 공개 저장소 https://github.com/oscar-jung/marble-ios 를 만들고 push함
- 첫 run이 2초 만에 취소됨: `The configuration 'macos-14-arm64-us-default' is not supported` → GitHub가 macos-14 이미지를 종료함
- 조사용 워크플로 `probe-runners.yml` 결과:
  - macos-15 (arm64/intel): Xcode 16.0–16.4, 26.0.1–26.3
  - macos-26: Xcode 26.0.1–26.6
  - **Xcode 15가 들어 있는 이미지는 없음**
- 대응: macos-15 + Xcode 16.4 + 호환 패치 `xnu/marble-patches/1000-xcode16-iig-kernelcontent.patch`
  - 패치 내용: xnu-11215에 있는 `IORPC.kernelContent` 필드를 xnu-8796.141.3에 추가하고, `IOUserServer`가 IORPC를 만드는 두 곳에서 그 필드를 채움
  - xnu-8796.141.3에 `git apply --check` 통과 확인
- run 44869356182 진행 중 (`make -k`로 오류를 한 번에 수집)
- **run 44869356182 ✅ (8분 4초):** Xcode 16.4 (16F6), MacOSX15.5 SDK, xnu-8796.141.3
  - ios-sweet 패치 81개와 `1000` 패치가 모두 적용됨. 건너뛴 것은 blacktop의 다른 macOS 버전용 패치 4개뿐 (ios-sweet CI에서도 같음)
  - 실제 컴파일 오류 0건. 로그에서 `error` 문자열로 잡힌 53곳은 모두 소스 안의 printf 문구임
  - 산출물 `kernel.development.sweet` (Mach-O arm64, 16.7 MB, SHA256 일치)
- **우리가 빌드한 커널을 QEMU에서 shim으로 부팅 ✅** (`docs/m5b1-qemu-ours-060.png`): 도구는 `45263be`의 `mkadt.py --qemu`와 `mkpayload.py`. upstream 아티팩트와 같은 지점(`Waiting on ... Apple_HFS`)까지 진행함. `iBoot Stage 2 version: sweet-shim` 표시
- **M5b-1 완료:** Xcode 15 없이도 xnu-8796 + sweet 패치를 빌드하는 파이프라인이 동작함. 다음은 M5b-2 (MARBLE config 패치 `1001~`, marble ADT)

### 2026-11-21 — M5b-2 패치와 marble ADT (CI 빌드 대기)

ios-sweet 패치를 적용한 트리(`xnu-8796.141.3` + 0001–0081 + 1000)를 읽어서 SWEET 커널이 sweet 하드웨어를 가정하는 곳을 정리함:

| 가정 | 위치 | marble에서 생기는 문제 | 대응 |
|---|---|---|---|
| FB `0x9c000000` | start.s stage-0 표시와 부트스트랩 블록, sweet_early.c, arm_vm_init 별칭 | MPSS/ADSP carve-out에 쓰게 됨 → 첫 명령부터 abort 가능 | **1001** `SWEET_FB_PHYS` (marble `0xb8000000`) |
| `GICR_PE_SIZE` 0x20000 고정 | pe_fiq.c `find_gicr_pe_base` | Gunyah vGIC는 0x40000 간격 | **1002** VLPIS 또는 ADT `redistributor-stride` |
| `CNTP_CTL_EL0` 쓰기 | machine_routines.c `_enable_virtual_timer` | Gunyah UNDEF → 모든 코어가 패닉 | **1003** SWEET에서 생략 |
| SM7150 고정 MMIO | sweet_keys/touch/spmi/rpmh/usb.c | stage-2 fault 가능 | `sweettouch=0 sweetkeys=0`. 배터리와 USB는 기본으로 꺼져 있음 |
| UFS `/sweet-ufs` | sweet_ufs.c | (컨트롤러는 같은 주소, UFS 3.1) | ADT에서 노드를 빼서 비활성. 나중에 따로 시험 |
| GIC 2-security | SWEET.h 주석 | Gunyah는 1-security | CTLR `0x13`, IGROUPR0 all-1로 M3에서 동작 확인 → 변경 없음 |
| CPU MPIDR Aff1 | SweetCPU.cpp | marble도 `0x000–0x700` | 변경 없음 |

- 부트스트랩 매핑: 16K granule에서 L1 엔트리 하나는 64 GiB를 덮음. RAM(`0x840000000`)과 FB(`0xb8000000`)가 모두 L1 index 0이므로 start.s의 "같은 V=P L2 테이블" 가정이 그대로 성립함
- `tools/mkadt_marble.py`: ios-sweet `mkadt.py`의 상수를 marble 값으로 바꾸고 트리를 조정함 (GIC stride, compatible, `sweet-ufs` 제거). `--qemu`는 RAM `0x100000000`/4G, GICv3 기본값을 씀
- CI: matrix `sweet` / `marble` (`SWEET_BOARD_FLAGS=-DSWEET_BOARD_MARBLE`)
- **CI run 44870826985 ✅:** sweet 6분 19초, marble 7분 1초. 두 보드 모두 패치 1000–1003이 적용됐고 오류 0건. marble 빌드에 `-DSWEET_BOARD_MARBLE` 반영됨
  - 디스어셈블로 확인: `start_first_cpu` 부근의 FB 상수가 sweet는 `movk #0x9c00, lsl #16`, marble은 `movk #0xb800, lsl #16`
- **QEMU:**
  - sweet 커널(1002/1003 포함, sweet `--qemu` ADT): 예전과 같은 지점(루트 대기)까지 진행 → 회귀 없음
  - **marble 커널**(`mkadt_marble.py --qemu`, `QEMU_FB_BASE=0xb8000000`, `-m 8G`, RAM `0x100000000`): `SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0xb8000000`, CPU0 SGI1 OK, 루트 대기까지 진행 ✅ (`docs/m5b2-qemu-marble-045.png`)
- 폰 이미지: `make xnu XNU_KERNEL=<marble kernel>` → `shim/out/marble/marble-shim-xnu-v4.img` (16.8 MB)
  - payload `SWEETXNU`, 세그먼트 10개, `phys_base 0x840000000`, `mem 0x13ff00000`
  - cmdline `debug=0xc serial=3 cpus=1 keepsyms=1 sweettouch=0 sweetkeys=0`

### 2026-11-21 — M5b-2 실기기 #1: XNU 실행, GIC 접근에서 panic

- shim 페이지 5: payload `0xB70161F3`(initrd +0xC181F3) `SWEETXNU`, 세그먼트 10개를 `0x841004000~`에 복사 (987 ms). ADT `0x8424D8000`, boot_args `0x8424DC000`, `JUMP TO 0x841480488` ✅
- **XNU가 폰에서 실행됨:** MMU on, sweet_early 콘솔(`0xb8000000`)에 panic 화면 출력. 커널 진입, 부트스트랩 매핑, FB 별칭은 모두 동작함
- panic: `Kernel data abort at pc _pe_init_fiq+0x254`, `esr 0x96000010` (현재 EL 데이터 abort, DFSC 0x10 = synchronous external abort), `far 0xfffffe000d604080`. 이후 nested panic 5회 → stall
  - `find_gicr_pe_base`는 성공 (x19 = 재분배기 0, 1002의 stride 동작). WAKER 처리도 통과
  - 실패한 명령: `str w9, [x8, #0x80]!` = `GICR_IGROUPR0`(SGI 프레임 +0x10080)에 쓰는 **pre-index writeback store**
  - **원인 (추정, 근거 강함):** Gunyah는 GIC 접근을 stage-2에서 트랩해 에뮬레이션함. writeback/pair 형식은 `ESR_EL2.ISV=0`이라 하이퍼바이저가 해독하지 못하고 external abort를 주입함 (KVM과 같은 제약). M3의 shim은 같은 레지스터를 단순 store로 썼기 때문에 성공함
- **수정 1004:** `pe_fiq.c`의 `_gic_read32/_gic_read64/_gic_write32`를 단순 `ldr`/`str` 인라인 asm으로 바꿈 (Linux `__raw_*`와 같은 방식)

### 2026-11-21 — 로컬 빌드 (Xcode 27 beta) ✅

- `tools/xnu-local.sh [marble|sweet] [--quick]`: CI와 같은 레시피를 `build-local/dxb-<board>/`(gitignore)에서 실행함
  - KDK 13.5(22G74)는 dortania 미러에서 받아 SHA256을 확인함 (659 MB). `pkgutil --expand-full`로 sudo 없이 `build-local/KDK_13.5_22G74.kdk`에 풀고 `KDKROOT`로 지정
- **Xcode 27.0 beta (27A5194q)로 전체 빌드 성공: 8분 14초, 컴파일 오류 0건**
  - 1000–1004 패치 모두 적용됨. 1000(iig 호환)이 Xcode 27에서도 충분함
  - 커널 16,687,352 B (CI의 Xcode 16.4 빌드는 16,687,776 B)
  - QEMU marble 레이아웃에서 CI 커널과 같은 지점(루트 대기)까지 부팅됨
- `--quick`: 그 사이 추가된 marble 패치만 적용하고 커널 make만 다시 실행함
  - build.sh의 "kernel.development가 이미 있으면 건너뜀" 조건을 끄도록 처리함
  - `pe_fiq.c` 하나를 바꾼 뒤 **12.2초**에 끝남 (CI는 약 10분)
- 앞으로의 흐름: 로컬에서 `--quick`으로 반복 → 폰 테스트. CI는 깨끗한 환경의 기준 빌드와 공개 아티팩트 용도로 유지

### 2026-11-21 — M5b-2 실기기 #2 (1004): **XNU가 marble에서 루트 마운트 대기까지 부팅** ✅

- 이미지: CI run 44873021550 marble 커널 + `marble-shim-xnu-v4.img`, cmdline `debug=0xc serial=3 cpus=1 keepsyms=1 sweettouch=0 sweetkeys=0`
- 화면 (sweet_early → fbcon, `0xb8000000`): QEMU와 같은 순서로 진행함
  - `iBoot version: sweet-shim`, platform expert, AMFI, AKS, WDT, IOSurface, HID 버튼/터치(드라이버 비활성 상태로 등록만), BATTERY static fallback
  - `SWEET FB: AppleCLCD 1080x2400 stride 4320 at 0xb8000000`
  - **`SWEET CPU: cpu=0 mpidr=0x0 came alive; SGI1 OK`**, `1/1 CPUs initialized, SGI1 verified` → Gunyah vGIC에서 SGI와 vtimer 인터럽트 경로가 동작함 (1002 stride, 1003 no-CNTP, 1004 ISV-safe MMIO가 함께 효과를 냄)
  - MAC/AMFI/Sandbox, HFS, mbuf(`96 MB total pool`, QEMU는 64 MB → 5 GiB RAM 인식), dlil/ifnet(lo0, gif0, stf0)
  - **`Waiting on <dict>… IOProviderClass IOMedia … Apple_HFS`**: 루트 디바이스 대기. UFS 노드를 빼 두었으므로 예상된 정지점
  - 새로 보인 줄: `Failed to write boot-picker-bringup-reason to NVRAM` (NVRAM이 없어서 생김, 무해)
- 결론: **SM7475 + Gunyah 위에서 XNU 커널 초기화가 끝까지 됨.** 남은 일은 루트 파일시스템과 그 이후 (M5b-3)

### 2026-11-21 — SMP: **8코어 모두 XNU에서 동작** ✅

- 같은 1004 커널에서 cmdline만 `cpus=8`로 바꿈 (`shim/Makefile`의 `XNU_CMDLINE` 기본값도 `cpus=8`로 변경)
- `SWEET CPU: requested=8 selected=8 (limit=8)`
- cpu1–7: `CPU_ON target=0x100…0x700 entry_pa=0x841482000 context=N` → `PSCI CPU_ON rc=0` → `mpidr=0xN00 came alive; SGI1 OK`
- **`SWEET CPU: 8/8 CPUs initialized, SGI1 verified`**, `mcache: 8 CPU(s), 64 bytes CPU cache line size`
- 커널 쪽 추가 변경 없이 성공함. 이유는 다음과 같음:
  - XNU reset vector가 MPIDR `& 0xFFFF`로 CPU 데이터를 찾으므로 MT 비트(24)와 무관함
  - 각 코어의 재분배기는 1002, 타이머 초기화는 1003, GIC 접근은 1004로 처리됨
- 같은 지점(`Waiting on … Apple_HFS`)에서 정지. 이 커널은 A510/A710/X2를 하나의 동질(SMP) 클러스터로 다룸 (P/E 구분 없음)
- 그 외 로그: `Log queues configured: slot count: 64` (CPU 수에 비례), zone map (VM 19G, DATA 25G 등)

### 2026-11-21 — M5b-3a: 램디스크 루트 + PID 1 (QEMU ✅, 폰 실행 중)

- 방식: ios-sweet의 mockfs 경로. 램디스크 자체가 정적 Mach-O(`sweet-init`, 16.6 KB, ad-hoc 서명)이고, XNU가 이것을 `/sbin/launchd`로 실행함. cmdline에 `rd=md0`
  - `mkpayload.py --ramdisk`가 커널 뒤 세그먼트로 붙이고 ADT `/chosen/memory-map RAMDisk`를 추가함 (폰: `0x8424d8000`, `0x8000`)
- `shim/Makefile`: `XNU_RAMDISK=`, `out/sweet-init` (Apple clang `-target arm64-apple-ios16.0` + `mkinit.py`), `make xnu-init XNU_KERNEL=…`
- QEMU (marble 레이아웃, `docs/m5b3-qemu-init-050.png`):
  - `Added memory device md0/rmd0` → `BSD root: md0, major 1, minor 0` → `VM Swap Subsystem is ON`
  - `load_init_program: attempting to load /sbin/launchd` → **`*** SWEET userspace: init running as PID 1 ***`** → `SWEET init: still alive` (5초 간격)
- **폰 ✅:** 8코어(`8/8 CPUs initialized, SGI1 verified`) → `Added memory device md0/rmd0 (01000000/12000000) at 0xFFFFFE00084D8000` → `BSD root: md0, major 1, minor 0` → `VM Swap Subsystem is ON` → `load_init_program: … /sbin/launchd` → **`*** SWEET userspace: init running as PID 1 ***`** → `SWEET init: still alive`가 5초 간격으로 반복됨 (사진 시점에 5회). panic 없음
  - → **SM7475(POCO F5)에서 XNU 위의 첫 유저 공간 프로세스가 동작함.** 다음이 모두 실기기에서 확인됨: exec, ad-hoc 코드 서명 검사, syscall(svc #0x80), devfs `/dev/console`, `select` 타임아웃(= vtimer 기반 스케줄링)
