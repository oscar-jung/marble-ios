# POCO F5 (marble) 하드웨어 인벤토리 — XNU 포팅용

작성: 2026-11-21. 출처는 LineageOS `lineage-24.0` 브랜치의 다운스트림 DT/디바이스 트리, 그리고 upstream Linux와 Mesa.
표시: ✅ 소스에서 직접 확인 / ⚠️ 실기기에서 확인 필요 / 📎 일반 사양(소스 미확인)

소스:
- DT: https://github.com/LineageOS/android_kernel_xiaomi_sm8450-devicetrees (`qcom/marble-*`, `ukee*`, `cape*`, `waipio-*`)
- Device: https://github.com/LineageOS/android_device_xiaomi_marble, https://github.com/LineageOS/android_device_xiaomi_sm8450-common
- Upstream: `torvalds/linux` `arch/arm64/boot/dts/qcom/sm8450.dtsi`, `drivers/gpu/drm/msm/adreno/a7xx_catalog.c`
- Mesa: `src/freedreno/common/freedreno_devices.py`

---

## 0. 핵심 결론

1. **SM7475의 Qualcomm 내부 코드명은 "ukee"** ✅. `ukee.dtsi`는 `#include "cape.dtsi"`(SM8475, 8+ Gen 1)이고, cape는 clock, QUP, SMMU를 `waipio`(SM8450, 8 Gen 1)에서 그대로 가져와. 그래서 **upstream Linux의 `sm8450.dtsi`가 거의 그대로 레퍼런스로 쓸 수 있음.** SM7475 자체는 upstream에 없지만 형제 SoC 지원이 풍부함.
2. **GPU는 Adreno725v1** ✅. Mesa에서는 `FD725`(chip_id `0x07030002`)로 지원되고, `FD730`(SM8450, `0x07030001`)과 같은 **a7xx gen1 패밀리**임 ✅. upstream Linux msm 드라이버에는 A730은 있고 A725 엔트리는 없음 ✅. A730 엔트리를 복제하면 될 가능성이 높음 ⚠️.
3. **터치는 SPI(QUP SE4)** ✅. ios-sweet은 GENI I2C 터치(GT9896)라서 그 코드를 그대로 쓸 수 없고, **GENI SPI 드라이버를 새로 써야 함.**
4. **센서와 충전은 DSP를 거침.** 센서는 SLPI, 충전과 배터리는 ADSP의 `pmic_glink`로 처리돼 ✅. 그래서 **배터리 잔량조차 원격 프로세서 부팅과 GLINK가 있어야 읽을 수 있음.** 초기 bring-up에서는 스텁으로 처리해야 함.
5. **부트 이미지는 header v4, A/B, GKI, DTB는 vendor_boot에 있음** ✅. 이 기기의 vendor_boot는 LineageOS 24.0 nightly(2026-11-03) 빌드임 (bootargs의 `mtdoops.fingerprint`로 확인, M1). ios-sweet(SM7150) 방식의 `fastboot boot` shim이 v4에서 어떻게 DTB를 넘기는지는 확인이 필요함 ⚠️.

---

## 1. SoC / CPU / 인터럽트

| 항목 | 값 | 상태 | 소스 |
|---|---|---|---|
| SoC | SM7475-AB, 코드명 `ukee`, `qcom,msm-id = <591 0x10000>` | ✅ | `ukee.dtsi` |
| 부모 SoC | cape(SM8475), waipio(SM8450) 파생 | ✅ | `ukee.dtsi`, `cape.dtsi` includes |
| CPU | 8코어, `cpu@0`~`cpu@700` (MPIDR `0x000`~`0x700`), PSCI `smc` | ✅ | `cape.dtsi:64-213, 463` |
| 클러스터 | 1× Cortex-X2 + 3× A710 + 4× A510 (ARMv9.0) | 📎 | 부트 CPU = A510 r0p3 (`MIDR 0x410FD463`) ✅ M0 |
| PAC / 16K | **8코어 모두** APA=5, GPA=1 (PAC 있음), **TGran16=1**, PARange 40-bit, SVE 숨김(0), MPIDR MT=1 | ✅ M2 실측 | CPU0–3 A510 r0p3, 4–6 A710 r2p0, 7 X2 r2p0 |
| capacity | CPU0-3 = 1024(silver), CPU4~ = 2253(gold) | ✅ | `cape.dtsi` |
| GIC | GICv3, GICD `0x17100000`, GICR `0x17180000` (stride `0x40000`, 8개), ITS `0x17140000` | ✅ `cape.dtsi:584-601`, M3 실측 | HLOS가 보는 것은 Gunyah 가상 GIC (IIDR `0x47000070`, SecurityExtn=0, SPI 992개, LPI 있음) |
| Arch timer | 19.2 MHz, PPI 13/14/11/12 | ✅ `cape.dtsi:616-622` | **vtimer(INTID 27) 동작, EL1 물리 타이머(`CNTP_*`)는 Gunyah가 트랩해서 UNDEF** (M3 실측) |
| TLMM | `0x0F000000`, **reserved GPIO 28–31, 36–39 (secure)** | ✅ | `cape.dtsi` tlmm. 이 GPIO를 건드리면 크래시 |
| PDC | `0xb220000` (wakeup interrupt parent) | ✅ | `cape.dtsi:1115` |
| LLCC | `0x19200000`, `qcom,ukee-llcc` | ✅ | `ukee.dtsi` |
| Debug UART | QUP SE7 `0x99c000` (`serial0`) | ✅ | `waipio-qupv3.dtsi:77`. 양산 기기에 패드가 노출됐는지 ⚠️ → ramoops/pstore 권장 |
| RAM | 8 GB (이 기기) | ✅ M1 | `/memory`: `0x80000000–0xEA000000`, `0xF4600000–0x100000000`, `0x800000000–0x839700000`, `0x839D00000–0x83BB00000`, **`0x840000000–0x980000000`(5 GiB, reserved 없음)**. reserved 54개(고정 603 MiB, 동적 452 MiB)는 모두 하위 4 GB 안 |

## 2. 부트

| 항목 | 값 | 상태 |
|---|---|---|
| 부트로더 | XBL → ABL (`abl.img`), Xiaomi 언락 | ✅ `proprietary-firmware.txt` |
| boot image | header **v4**, pagesize 4096, base 0, `Image` (GKI) | ✅ `BoardConfigCommon.mk:72-86` |
| 파티션 | A/B, boot 192 MiB, vendor_boot 96 MiB, dtbo 24 MiB | ✅ |
| DTB | vendor_boot 안에 있고 dtbo 오버레이 적용 (`marble-sm7475-pm8008-overlay`) | ✅ |
| board-id | `qcom,board-id = <0x10008 0>`, `xiaomi,miboard-id = <0xf 0>` | ✅ |
| bootconfig | `androidboot.hardware=qcom`, USB `a600000.dwc3` | ✅ |
| `fastboot boot` 경로 | header v4 이미지를 RAM 부팅하면 ABL이 flash의 vendor_boot DTB를 x0로 넘김. 커널 로드 `0xA8000000`, FDT `0xB7205000`, initrd는 `0xB8000000` 직전까지 | ✅ M0 (2026-11-21) |

## 3. PMIC / 전원

SPMI arbiter가 **2개 버스**임 (`spmi0` `0xc42d000`, `spmi1` `0xc432000`) ✅. hackiosx 문서가 "최신 칩은 PMIC 버스가 다름"이라고 한 부분이 이것으로 보임. ios-sweet(SM7150)의 SPMI 코드는 PMIC arbiter v7과 다중 버스에 맞게 고쳐야 함.

| SID | PMIC | 역할 (marble) | 상태 |
|---|---|---|---|
| 0 | PMK8350 | PON: **전원 키(KPDPWR), 볼륨 다운(RESIN)**, RTC, ADC5/VADC 서미스터 | ✅ `pmk8350.dtsi:30-44` |
| 1 | PM8350 | **볼륨 업 = `pm8350_gpios 6`** (active low) | ✅ `xiaomi-sm7475-common.dtsi` |
| 2 | PM8350C | 플래시 LED, 스위치 | ✅ |
| 3 | PM8350B | LRA 햅틱 (`qcom-hv-haptics`) | ✅ |
| 4/5 | PMR735A / PMR735B | RF/레귤레이터 | ✅ overlay include (B 실장 여부 ⚠️) |
| 7 | PM8450 | | include만 확인, 실장 여부 ⚠️ |
| I2C | PM8008 | 카메라용 LDO `L1J`~`L7J` | ✅ `marble-sm7475-pm8008-overlay.dts` |
| — | RPMh | `apps_rsc @ 0x17a00000`. CX/MX/GFX 레일과 LDO 대부분이 RPMh VRM 경유 | ✅ |

레귤레이터 (marble 오버라이드) ✅:
- `L1C` 1.8 V: 지문
- `L2C` 1.8 V: **터치 IOVDD**
- `L6C` 3.3 V: 지문
- `L9C` 3.224 V: **터치 AVDD**

## 4. 디스플레이

| 항목 | 값 | 상태 |
|---|---|---|
| 컨트롤러 | MDSS/SDE (cape), **DSI0 단일** | ✅ |
| 패널 | AMOLED 1080×2400, **video mode (burst)**, 120 Hz, 30 bpp (10-bit) | ✅ `dsi-panel-m16t-36-*-dsc-vid.dtsi` |
| DSC | v1.2, slice 540×20 | ✅ |
| 변종 | `36 02 0a`, `36 0d 0b` 두 가지 (리셋 시퀀스가 다름) | ✅ 이 기기는 **36 02 0a** (M0, bootargs) |
| 전원 | vddio 1.8 V, vddd 1.2 V, vci 3.0 V | ✅ |
| 백라이트 | DCS, max 2047 | ✅ |
| splash FB | `cont_splash_region` **`0xb8000000`, 0x2b00000 (43 MiB)**, a8r8g8b8, stride 4320 | ✅ `cape-sde-display.dtsi:33`, M0 실측 |

포팅 관점: ios-sweet과 같은 "DSC video mode + ABL continuous splash" 조합이라서, **ABL이 켜둔 프레임버퍼에 CPU로 그리는 1단계는 같은 방식으로 가능함.** 실제 포맷(스트라이드, 픽셀 포맷)과 SSPP 주소는 실기기 레지스터 덤프로 확인 필요 ⚠️.

## 5. 입력

| 항목 | 값 | 상태 |
|---|---|---|
| 터치 IC | **Goodix GT9916S** (BRL 계열) | ✅ |
| 버스 | **SPI, QUP SE4**, 15 MHz, CS = gpio19 | ✅ |
| IRQ / RST | gpio21 (level low / edge falling), gpio20 | ✅ |
| 전원 | IOVDD `L2C` 1.8 V, AVDD `L9C` 3.224 V | ✅ |
| 패널 ID | gpio119 / gpio117 → `goodix_firmware_TM*.bin`, `goodix_cfg_group_TM*.bin` | ✅ 부팅 때 FW/config 업로드가 필수인지 ⚠️ |
| 해상도 | 1080×2400, 압력 4096 | ✅ |
| 전원 키 / 볼다운 | PMK8350 PON | ✅ |
| 볼업 | PM8350 GPIO6 | ✅ |

## 6. 스토리지 / USB

| 항목 | 값 | 상태 |
|---|---|---|
| UFS HC | `0x1d84000` (+ICE `0x1d88000`), SPI 265, PHY `0x1d80000` | ✅ `cape.dtsi:1893-1925` |
| UFS | 3.1, 256/512 GB | 📎 |
| marble 설정 | `qcom,iommu-dma = "bypass"` | ✅ SMMU를 우회하므로 초기 bring-up에 유리 |
| USB | DWC3 `0xa600000`, NXP eUSB2 repeater | ✅ |

## 7. GPU

| 항목 | 값 | 상태 |
|---|---|---|
| 모델 | `Adreno725v1`, speed-bin은 qfprom `0x221c8000+0x119` | ✅ `ukee.dtsi` |
| 클럭 | 220 ~ **580 MHz** (7단계), initial pwrlevel 5 (285 MHz) | ✅ |
| Mesa | `FD725` chip_id `0x07030002`, a7xx gen1 (FD730과 같은 계열) | ✅ freedreno/turnip 지원 |
| Linux msm | A730(`0x07030001`, `a730_sqe.fw`, `gmu_gen70000.bin`)만 있고 A725는 없음 | ✅. A725 FW 파일명은 ⚠️ |
| 레퍼런스 | UniXNU의 Adreno 740 드라이버(a7xx gen2), HackiOS의 a6xx 드라이버 | |

포팅 관점: GPU 세대가 SM8450(A730)과 같아서 **upstream msm의 a7xx gen1 경로와 GMU 코드가 직접 레퍼런스가 됨.** HackiOS(a6xx)보다 UniXNU(a7xx) 쪽이 훨씬 가까움.

## 8. 오디오 / 햅틱

| 항목 | 값 | 상태 |
|---|---|---|
| 코덱 | LPASS CDC 매크로 (RX/TX/VA/WSA) + WCD938x (3.5 mm 잭) | ✅ 모듈 목록 (`wcd937x`/`wcd938x` 중 실제 어느 쪽인지 ⚠️) |
| 스피커 앰프 | **2× Awinic AW882xx**, I2C QUP SE15 @ `0x34`(TOP, IRQ gpio88, RST gpio1) / `0x35`(BOT, IRQ gpio63, RST gpio120) | ✅ |
| 햅틱 | PM8350B LRA, f0 170 Hz | ✅ |

오디오 경로가 ADSP(Q6) 펌웨어 의존이라서 iOS에서 소리를 내려면 remoteproc, GLINK, APR/GPR 계층이 필요함. 후순위.

## 9. 무선

| 항목 | 값 | 상태 |
|---|---|---|
| Wi-Fi | QCA6490 (WCN6855 계열), PCIe0, `cnss-qca6490@b0000000` | ✅ cape.dtsi 기준. marble 실장 칩은 ⚠️ (common에 qca6750 빌드도 있음) |
| PCIe1 | ukee에서 비활성 | ✅ |
| BT | QCA6490 (UART) | ✅ cape.dtsi |
| 모뎀 | 내장 (`modem.img`), IPA/rmnet | ✅. iOS 포트에서는 범위 밖 |
| NFC | NXP (`nfc-service.nxp`) | ✅ |
| IR 블래스터 | `ir-spi` | ✅ |

## 10. 센서 / 충전 / 기타

| 항목 | 값 | 상태 |
|---|---|---|
| 센서 | **전부 SLPI 경유** (`qcom,msm-ssc-sensors`, `slpi2.mdt`) | ✅. 가속도와 자이로도 DSP 없이는 못 읽음 |
| 충전/배터리 | `battery_charger` → **pmic_glink (ADSP)**, shutdown 3350 mV | ✅ |
| 서미스터 | PMK8350 VADC (cpu, flash, quiet, conn, pa0/1, wifi, charger) | ✅ thermal 기본 보호에 사용 가능 |
| 지문 | DT에 FPC1020과 Goodix 노드가 같이 있음 | ✅. iOS에서는 어차피 미지원 |
| 카메라 | `camera/marble-sm7475-camera-sensor.dts` | 범위 밖 |
| SPSS | 비활성 | ✅ |

---

## 11. 레퍼런스 프로젝트 대비 차이 요약

| 영역 | ios-sweet (SM7150) | HackiOS (SM7325) | **marble (SM7475)** |
|---|---|---|---|
| 부트 | ABL + header v2 계열 `fastboot boot` shim | UEFI (Mu-Silicium) | ABL **header v4** ⚠️ |
| CPU | A76/A55, PAC 없음 | A78/A55, PAC 없음 | **X2/A710/A510, PAC 있음** |
| PMIC 버스 | SPMI 단일 | | **SPMI arb v7, 2개 버스** |
| 터치 | GT9896 / **I2C** | | GT9916S / **SPI** |
| 패널 | DSC video, ABL splash | | DSC video, ABL splash (같음) |
| GPU | 없음 | A642L (a6xx) | **A725 (a7xx gen1)** = A730 형제 |
| Upstream Linux 레퍼런스 | sm7150 | sc7280 | **sm8450** (매우 풍부) |

## 12. 실기기에서 확인할 것 (루팅된 Android에서)

```sh
# 1. 라이브 DTB 추출 (병합된 최종본)
adb shell su -c 'cat /sys/firmware/fdt' > marble-live.dtb
dtc -I dtb -O dts marble-live.dtb > marble-live.dts

# 2. CPU, PAC 확인 (Features에 paca/pacg가 있어야 함)
adb shell cat /proc/cpuinfo | grep -E 'CPU part|Features' | sort -u

# 3. 패널 변종, ABL cmdline
adb shell su -c 'cat /proc/cmdline' | tr ' ' '\n' | grep -iE 'panel|dsi|mdss'

# 4. GPU chip id, FW 파일명
adb shell su -c 'dmesg' | grep -iE 'kgsl|adreno|gmu|sqe|zap'
adb shell ls /vendor/firmware | grep -iE 'a7|gen7|gmu|zap'

# 5. Wi-Fi 칩, 오디오 코덱, 터치 FW
adb shell su -c 'dmesg' | grep -iE 'cnss|qca6|wcd93|goodix|gt99'

# 6. 메모리 맵 (reserved region / hole 목록)
adb shell su -c 'cat /proc/iomem' > iomem.txt

# 7. 파티션 백업 대상 (쓰기 금지)
adb shell ls -l /dev/block/by-name | grep -E 'persist|modemst1|modemst2|fsg|fsc'
```
