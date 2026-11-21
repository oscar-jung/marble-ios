# marble-shim — M0~M3 bring-up 페이지

[docs/shim-design.md](../docs/shim-design.md) §8의 M0~M3을 한 이미지에 담았습니다. ABL에서 `fastboot boot`으로 RAM에 올려 실행하며, 폰에는 아무것도 쓰지 않습니다. 각 페이지는 화면을 지우고, 점검을 실행하면서 결과를 바로 그리고, `PAGE_S`(폰 25초) 동안 유지합니다. 그 사이에 사진을 찍으면 됩니다.

| 페이지 | 마일스톤 | 내용 |
|---|---|---|
| 0 | M0 | EL, FDT, 부트 CPU의 MIDR/MPIDR, PAC, 16K, PSCI, SYSTEM_RESET2 |
| 1 | M1 | FDT 헤더, **`/memory`의 모든 뱅크**, initrd, kaslr/rng seed, **bootargs 원문** |
| 2 | M1 | **`/reserved-memory`의 모든 자식 노드**: 이름, base, size, no-map(N), 다중 reg(+), 동적(DYNAMIC) |
| 3 | M2 | `/cpus/cpu@*` 각각에 PSCI CPU_ON을 보내고, 코어별 MIDR, PAC(APA/GPA), TGran16, PARange, SVE, EL을 표시. CPU_OFF 후 AFFINITY_INFO |
| 4 | M3 | GICD ID, ICC_SRE, 재분배기(redistributor) 목록과 내 것(`<=`), vtimer(INTID 27)·SGI 1 인터럽트 수. 페이지가 유지된 **뒤에** EL1 물리 타이머(INTID 30)를 시험 (Gunyah가 막을 수 있어 가장 마지막에 둠) |

| 5 | M5 | **XNU 핸드오프.** initrd에 payload(`MARBLXNU`/`SWEETXNU`)가 있으면 검증하고, `phys_base`로 복사하고, ADT와 boot_args를 놓은 뒤 점프함. 점프 전에 `LOADER_WAIT_S`(폰 15초) 동안 화면에 계획을 표시함 |

payload가 있으면 shim은 진단 페이지를 건너뛰고 바로 페이지 5로 갑니다. `shim.diag=1`을 주면 0~4를 먼저 실행합니다. 마지막 페이지가 끝나면 PSCI로 리셋합니다. **리셋되는 동안 볼륨 다운을 누르고 있으면** fastboot로 들어갑니다. 예외가 나면 진한 빨강 PANIC 화면에 `STAGE`, `ESR`, `ELR`, `FAR`, `MPIDR`가 표시됩니다.

### 녹색과 빨간색의 의미
- 3페이지: PAC와 16K가 모두 있는 코어는 녹색, 하나라도 없으면 빨간색. `NO REPORT`는 CPU_ON 후 0.5초 안에 응답이 없었다는 뜻
- 4페이지: vtimer는 20~40회(100 Hz로 300 ms, 기대값 약 30), SGI는 정확히 10회면 녹색

## cmdline 옵션 (`make CMDLINE="..."`)

| 옵션 | 동작 |
|---|---|
| `shim.hold=1` | 리셋하지 않고 마지막 페이지(또는 PANIC 화면)에서 멈춤 |
| `shim.page=N` | 페이지 N(0~4)만 실행하고 멈춤. 같은 페이지를 다시 찍을 때 사용 |
| `shim.diag=1` | payload가 있어도 진단 페이지 0~4를 먼저 실행 |
| `shim.mdp=1` | 0페이지에서 MDP SSPP 레지스터를 읽어 실제 스캔아웃 표면을 찾음. MDP 클럭이 꺼져 있으면 버스가 멈출 수 있음 |
| `shim.fault=1` | 0페이지 뒤에 일부러 데이터 abort를 일으킴 (PANIC 화면 시험) |

예전 `m0.*` 이름도 그대로 받습니다.

## 빌드

Homebrew의 `llvm`, `lld`가 필요합니다. QEMU 테스트에는 `qemu`, `dtc`도 씁니다.

```bash
brew install llvm lld qemu dtc
```

폰용 (header v4, DTB는 폰의 vendor_boot에서 읽음). 결과는 `out/marble/marble-shim-v4.img`:

```bash
make CMDLINE="shim.hold=1"
```

header v2 (stock DTB를 직접 넣음, 1안):

```bash
make HDR=2 DTB=/path/to/stock-base.dtb
```

QEMU에서 페이지 N 렌더링 (`-M virt,gic-version=3 -smp 8`). 결과는 `out/qemu/page-N.png`:

```bash
make qemu-run PAGE=3
```

### M5a 시험 커널 (tk)

`tk.c`는 진짜 XNU 자리에 들어가는 가짜 커널입니다. 넘겨받은 boot_args를 표시하고, ADT 복사본을 검증하고, 받은 RAM 전체(폰에서는 `0x840000000`부터 5 GiB)에 64지점 쓰기/읽기를 해서 Gunyah가 그 메모리 접근을 허용하는지 확인합니다. 배경이 **진한 초록**이면 tk가 실행 중인 것입니다.

폰용 shim + tk 이미지. 결과는 `out/marble/marble-shim-tk-v4.img`:

```bash
make tk
```

QEMU (13바이트 접두부로 정렬 어긋남을 흉내). 결과는 `out/qemu/tk.png`:

```bash
make qemu-tk
```

## 폰에서 실행

> ⚠️ 첫 시도 전 준비: 부트로더 언락, 그리고 `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`와 양쪽 슬롯의 `boot`, `vendor_boot`, `dtbo` 백업.

```bash
adb reboot bootloader
```

```bash
fastboot boot out/marble/marble-shim-v4.img
```

- `shim.hold` 없이 빌드하면 5페이지가 각각 25초씩 지나간 뒤 리셋됩니다. 이때 볼륨 다운을 누르고 있으면 fastboot로 돌아옵니다.
- 특정 페이지만 다시 보려면 `make CMDLINE="shim.page=2"` 후 같은 방법으로 부팅합니다.

## 실기기 기록
- 2026-11-21 M0 (v4): 동작함. 결과는 [shim-design.md §11](../docs/shim-design.md)에 기록.

## 파일

| 파일 | 역할 |
|---|---|
| `start.S` | arm64 Image 헤더, 진입, 스택, 예외 벡터 (IRQ는 eret으로 복귀), PSCI secondary 진입점 |
| `main.c` | 페이지 0~2, 페이지 순서, cmdline, PSCI, 리셋, PANIC 화면 |
| `smp.c` | 페이지 3: `/cpus`에서 대상을 읽어 CPU_ON, 코어별 ID 레지스터 보고 |
| `gic.c` | 페이지 4: GICv3 distributor/redistributor 설정, 타이머/SGI 인터럽트 카운트. 핸드오프 전 `gic_quiesce` |
| `loader.c` | 페이지 5: payload 스캔, 검증, 복사, ADT/boot_args, 점프 |
| `payload.h` | payload 헤더(ios-sweet과 호환)와 boot_args 구조체 |
| `tk.c` | M5a 시험 커널 (가짜 XNU) |
| `../tools/mkpayload_tk.py` | tk → payload |
| `console.c` | 프레임버퍼 텍스트 콘솔: 자체 5×7 폰트, 배율 변경, `con_printf`, 줄바꿈 출력 |
| `fdt.c` | FDT 읽기: 최상위 노드 속성, 자식 노드 순회(`fdt_foreach_child`) |
| `libc.c` | `memset`/`memcpy` |
| `board.h` | marble / QEMU 상수 (FB, MDP, GIC, PSCI 호출 방식, 페이지 유지 시간) |
| `shim.h` | 공용 매크로와 함수 선언 |
| `../tools/mkbootimg_shim.py` | boot image v2/v4 패커 (AOSP 헤더 정의를 보고 직접 작성) |
| `../tools/fb2png.py` | QEMU 프레임버퍼 덤프 → PNG |

위치 독립 코드입니다. C 오브젝트에 절대 재배치(`R_AARCH64_ABS64`)가 하나라도 있으면 빌드가 실패합니다 (`make check`).
