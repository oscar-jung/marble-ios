# marble XNU patches

These are applied after the ios-sweet patch set (`third_party/ios-sweet/xnu/patches/`). Numbering starts at 1000, so they always sort after the upstream ones (0001–0081).

- `1000-xcode16-iig-kernelcontent.patch`: build compatibility. Adds `IORPC.kernelContent` (as xnu-11215 has it), which Xcode 16's iig emits, and fills it in IOUserServer. Needed because hosted runners no longer carry Xcode 15.
- marble patches start at 1001.

Planned work (M5b-2, see `docs/shim-design.md` §12):
- MARBLE machine config: vtimer INTID 27, GICR frame `0x40000`, single security state GIC (Gunyah)
- CPU IDs: MPIDR `0x000–0x700` with MT=1, cores A510×4 + A710×3 + X2
- Framebuffer `0xb8000000`, DRAM `0x840000000` + 5 GiB (these go in the ADT, through a marble `mkadt`)
- Never touch `CNTP_*`: Gunyah traps it with UNDEF

M5b-1 builds the SWEET config (with only 1000 added), to check that the build pipeline works.
