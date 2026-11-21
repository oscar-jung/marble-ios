# marble XNU patches

These are applied after the ios-sweet patch set (`third_party/ios-sweet/xnu/patches/`). Number them from 1001 so they always sort after the upstream ones (0001–0081).

Planned work (M5b-2, see `docs/shim-design.md` §12):
- MARBLE machine config: vtimer INTID 27, GICR frame `0x40000`, single security state GIC (Gunyah)
- CPU IDs: MPIDR `0x000–0x700` with MT=1, cores A510×4 + A710×3 + X2
- Framebuffer `0xb8000000`, DRAM `0x840000000` + 5 GiB (these go in the ADT, through a marble `mkadt`)
- Never touch `CNTP_*`: Gunyah traps it with UNDEF

Empty for now. M5b-1 builds the unmodified SWEET config, to check that the build pipeline works.
