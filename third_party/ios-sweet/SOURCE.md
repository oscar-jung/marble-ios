# ios-sweet (vendored)

- Upstream: https://github.com/dipa-studio/ios-sweet
- Commit: `45263bee51df4a0f55cdf9c649af886399488ddb` (2026-10-23)
- License: MIT (`LICENSE`, Copyright (c) 2026 dipa-studio)
- Contents: `xnu/patches/` (patches 0001–0081 and the `stage-*.sh` helpers), the upstream `xnu/README.md` (saved as `xnu/README.upstream.md`), and `tools/adt.py`, `tools/mkadt.py`, `tools/mkpayload.py` (`tools/mkadt_marble.py` imports these).

These patches go on top of Apple's open-source xnu-8796 (macOS 13.5 / the iOS 16 generation), using [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build). They add the SWEET machine config (Redmi Note 10 Pro, SM7150). The marble-only changes live separately in `xnu/marble-patches/` and are applied after these.

Do not edit the files here. Put changes in `xnu/marble-patches/`, so the upstream set can be replaced as a whole when updating.
