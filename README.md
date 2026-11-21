# marble-ios

Experiment: run XNU and the iOS userland on the POCO F5 (`marble`, Snapdragon 7+ Gen 2 / SM7475). Not affiliated with Apple, Xiaomi or Qualcomm. No Apple binaries are kept in this repo.

| Path | Contents |
|---|---|
| `docs/hardware-inventory.md` | marble hardware inventory: DT sources plus values measured on the phone |
| `docs/shim-design.md` | Boot shim design, milestones M0–M5, phone results (§11), XNU plan (§12) |
| `shim/` | The ABL → XNU boot shim: diagnostic pages, XNU loader, tk test kernel |
| `tools/` | Boot image packer (v2/v4), payload tools, framebuffer → PNG |
| `third_party/ios-sweet/` | ios-sweet XNU patch set (MIT), vendored unchanged |
| `xnu/marble-patches/` | marble XNU patches (applied after ios-sweet's) |
| `.github/workflows/xnu.yml` | xnu-8796 build (macOS runner, Xcode 15) |

Status (2026-11-21): M0–M3 and M5a are verified on the phone. The ios-sweet XNU boots through the shim in QEMU (M5b-1). Details are in `docs/shim-design.md` §11.
