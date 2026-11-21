# marble XNU patches

These are applied after the ios-sweet patch set (`third_party/ios-sweet/xnu/patches/`). Numbering starts at 1000, so they always sort after the upstream ones (0001–0081).

- `1000-xcode16-iig-kernelcontent.patch`: build compatibility. Adds `IORPC.kernelContent` (as xnu-11215 has it), which Xcode 16's iig emits, and fills it in IOUserServer. Needed because hosted runners no longer carry Xcode 15.
- `1001-marble-board-flag-fb-address.patch`: build flag. Lets the SWEET machine config take board defines from the environment (`SWEET_BOARD_FLAGS`). With `-DSWEET_BOARD_MARBLE` the boot framebuffer `SWEET_FB_PHYS` becomes `0xb8000000`; sweet keeps `0x9c000000`. Covers start.s stage-0 and the bootstrap block, sweet_early.c and the arm_vm_init alias. On marble, `0x9c000000` is inside the MPSS/ADSP carve-outs.
- `1002-marble-gicr-stride.patch`: steps through the redistributors frame by frame, by `GICR_TYPER.VLPIS` (0x20000 or 0x40000), or uses ADT `/arm-io/gic` `redistributor-stride` when present. marble's Gunyah vGIC uses 0x40000; QEMU's GICv3 stays at 0x20000.
- `1003-marble-no-cntp-access.patch`: drops the "precaution" `CNTP_CTL_EL0` write in `_enable_virtual_timer()`. Under Gunyah it is UNDEF (M3), and every core would panic as it starts its timer. SWEET only enables the vtimer PPI, so this is harmless on sweet too.

These need the marble ADT from `tools/mkadt_marble.py` and the cmdline `sweettouch=0 sweetkeys=0` (the sweet touch/keys drivers poke SM7150 addresses; battery and USB are off by default, UFS without `/sweet-ufs`).
