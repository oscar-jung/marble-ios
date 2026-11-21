# marble-shim (M0-M3 bring-up pages, M5 XNU loader)
#
#   make                      phone image, header v4: out/marble/marble-shim-v4.img
#                             (ABL takes the DTB from the phone's vendor_boot)
#   make HDR=2 DTB=<dtb>      phone image, header v2 with our own DTB: out/marble/marble-shim-v2.img
#   make BOARD=qemu           QEMU build:  out/qemu/Image
#   make qemu-run PAGE=N      boot the QEMU build on page N, dump the framebuffer to out/qemu/page-N.png
#   make CMDLINE="shim.hold=1"  extra cmdline for the phone image (shim.hold=1, shim.page=N, shim.mdp=1)

BOARD     ?= marble
LLVM      ?= $(shell brew --prefix llvm 2>/dev/null || echo /usr)/bin
LLD       ?= $(shell brew --prefix lld 2>/dev/null || echo /usr)/bin
CC        := $(LLVM)/clang
OBJCOPY   := $(LLVM)/llvm-objcopy
READELF   := $(LLVM)/llvm-readelf
NM        := $(LLVM)/llvm-nm
LD        := $(LLD)/ld.lld
PYTHON    ?= python3

OUT       := out/$(BOARD)
QEMU_FB_BASE ?= 0x9c000000
BOARD_DEF := $(if $(filter qemu,$(BOARD)),-DBOARD_QEMU -DQEMU_FB=$(QEMU_FB_BASE)UL,-DBOARD_MARBLE)

CFLAGS := --target=aarch64-none-elf -march=armv8-a -O2 -std=gnu11 \
	-ffreestanding -fno-builtin -nostdlib -fno-pic -fno-pie -fno-jump-tables -fno-strict-aliasing \
	-mgeneral-regs-only -mstrict-align -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Wall -Wextra -Werror $(BOARD_DEF)

SRCS := start.S main.c console.c fdt.c smp.c gic.c loader.c libc.c
OBJS := $(patsubst %,$(OUT)/%.o,$(basename $(SRCS)))

# M5a test kernel (tk.c): a stand-in for XNU, packed as a payload in the ramdisk.
TK_OBJS := $(OUT)/start.o $(OUT)/tk.o $(OUT)/console.o $(OUT)/libc.o
ifeq ($(BOARD),qemu)
TK_PHYS   ?= 0x100000000
TK_MEM    ?= 0x100000000
TK_PREFIX ?= 13
else
TK_PHYS   ?= 0x840000000
TK_MEM    ?= 0x140000000
TK_PREFIX ?= 0
endif
TK_CMDLINE ?= tk.hold=1

# Phone boot image. v4: DTB from the phone's vendor_boot (design plan B, needs
# no extraction). v2: DTB inside the image (plan A, ios-sweet layout).
HDR       ?= 4
DTB       ?=
CMDLINE   ?=
OS_VER    ?= 15.0.0
OS_PATCH  ?= 2035-06

.PHONY: all clean qemu-run qemu-tk tk check FORCE

ifeq ($(BOARD),qemu)
all: $(OUT)/Image
else
all: $(OUT)/marble-shim-v$(HDR).img
endif

$(OUT)/%.o: %.c board.h console.h fdt.h shim.h payload.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/%.o: %.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT):
	mkdir -p $@

# No absolute relocations outside the Image header: the code must run at any address.
CHECK_OBJS = $(filter-out $(OUT)/start.o,$(sort $(OBJS) $(TK_OBJS)))
check: $(OBJS) $(TK_OBJS)
	@n=$$($(READELF) -r $(CHECK_OBJS) | grep -c R_AARCH64_ABS64 || true); \
	if [ "$$n" != 0 ]; then echo "error: $$n absolute relocations in C objects" >&2; \
		$(READELF) -r $(CHECK_OBJS) | grep ABS64 >&2; exit 1; fi

$(OUT)/shim.elf: $(OBJS) shim.ld check
	$(LD) -T shim.ld -nostdlib -static --no-dynamic-linker --build-id=none -o $@ $(OBJS)

$(OUT)/tk.elf: $(TK_OBJS) shim.ld check
	$(LD) -T shim.ld -nostdlib -static --no-dynamic-linker --build-id=none -o $@ $(TK_OBJS)

$(OUT)/tk.Image: $(OUT)/tk.elf
	$(OBJCOPY) -O binary $< $@
	@size=$$($(NM) $< | awk '$$3=="_end"{print $$1}'); \
	$(PYTHON) -c "import sys;f=open(sys.argv[1],'r+b');f.truncate(int(sys.argv[2],16))" $@ $$size

$(OUT)/tk-payload.bin: $(OUT)/tk.Image ../tools/mkpayload_tk.py FORCE
	$(PYTHON) -I ../tools/mkpayload_tk.py $< $@ --phys-base $(TK_PHYS) --mem-size $(TK_MEM) \
		--prefix $(TK_PREFIX) --cmdline "$(TK_CMDLINE)"

# Shim + tk payload in the ramdisk (header v4): the shim goes straight to page 5 and jumps to tk.
tk: $(OUT)/marble-shim-tk-v4.img
$(OUT)/marble-shim-tk-v4.img: $(OUT)/Image $(OUT)/tk-payload.bin ../tools/mkbootimg_shim.py FORCE
	$(PYTHON) -I ../tools/mkbootimg_shim.py --header-version 4 --kernel $(OUT)/Image \
		--ramdisk $(OUT)/tk-payload.bin --cmdline "$(CMDLINE)" \
		--os-version $(OS_VER) --os-patch-level $(OS_PATCH) -o $@

# Flat binary padded to image_size (= _end) so .bss and the stack are real zero bytes.
$(OUT)/Image: $(OUT)/shim.elf
	$(OBJCOPY) -O binary $< $@
	@size=$$($(NM) $< | awk '$$3=="_end"{print $$1}'); \
	$(PYTHON) -c "import sys;f=open(sys.argv[1],'r+b');f.truncate(int(sys.argv[2],16))" $@ $$size; \
	ls -l $@

$(OUT)/marble-shim-v4.img: $(OUT)/Image ../tools/mkbootimg_shim.py FORCE
	$(PYTHON) -I ../tools/mkbootimg_shim.py --header-version 4 --kernel $(OUT)/Image \
		--cmdline "$(CMDLINE)" --os-version $(OS_VER) --os-patch-level $(OS_PATCH) -o $@

$(OUT)/marble-shim-v2.img: $(OUT)/Image ../tools/mkbootimg_shim.py FORCE
	@if [ -z "$(DTB)" ]; then echo "error: set DTB=<stock base dtb from vendor_boot> (see README)" >&2; exit 1; fi
	$(PYTHON) -I ../tools/mkbootimg_shim.py --header-version 2 --kernel $(OUT)/Image --dtb $(DTB) \
		--cmdline "$(CMDLINE)" --os-version $(OS_VER) --os-patch-level $(OS_PATCH) -o $@

# Repack every time: CMDLINE is not a file dependency.
FORCE:

QEMU_FB_SIZE := 10368000
PAGE         ?= 0

qemu-run:
	$(MAKE) BOARD=qemu
	rm -f out/qemu/fb.raw
	( printf 'pmemsave $(QEMU_FB_BASE) $(QEMU_FB_SIZE) out/qemu/fb.raw\nquit\n' | \
	  { sleep 4; cat; } | qemu-system-aarch64 -M virt,gic-version=3 -cpu max -smp 8 -m 4G \
	  -nographic -no-reboot -kernel out/qemu/Image -append "shim.page=$(PAGE)" \
	  -monitor stdio -serial none ) >/dev/null
	$(PYTHON) -I ../tools/fb2png.py out/qemu/fb.raw 1080 2400 out/qemu/page-$(PAGE).png

# QEMU: shim + tk payload as -initrd (with a junk prefix), framebuffer dumped after tk has run.
qemu-tk:
	$(MAKE) BOARD=qemu out/qemu/Image out/qemu/tk-payload.bin
	( printf 'pmemsave $(QEMU_FB_BASE) $(QEMU_FB_SIZE) out/qemu/fb.raw\nquit\n' | \
	  { sleep 6; cat; } | qemu-system-aarch64 -M virt,gic-version=3 -cpu max -smp 8 -m 8G \
	  -nographic -no-reboot -kernel out/qemu/Image -initrd out/qemu/tk-payload.bin \
	  -append "$(CMDLINE)" -monitor stdio -serial none ) >/dev/null
	$(PYTHON) -I ../tools/fb2png.py out/qemu/fb.raw 1080 2400 out/qemu/tk.png

# QEMU: shim + an XNU payload (e.g. ios-sweet's mkpayload.py output) as -initrd.
# Dumps the framebuffer every SNAP seconds for SECS seconds to out/qemu/xnu-<t>.png
# and logs guest exceptions to out/qemu/xnu-qemu.log.
XNU_PAYLOAD ?=
SECS        ?= 60
SNAP        ?= 15
QEMU_CPU    ?= cortex-a76
QEMU_MEM    ?= 4G
qemu-xnu:
	@if [ -z "$(XNU_PAYLOAD)" ]; then echo "error: set XNU_PAYLOAD=<payload>" >&2; exit 1; fi
	$(MAKE) -B BOARD=qemu QEMU_FB_BASE=$(QEMU_FB_BASE) out/qemu/Image
	( for t in $$(seq $(SNAP) $(SNAP) $(SECS)); do sleep $(SNAP); \
	    printf 'pmemsave $(QEMU_FB_BASE) $(QEMU_FB_SIZE) out/qemu/xnu-%03d.raw\n' $$t; done; printf 'quit\n' ) | \
	  qemu-system-aarch64 -M virt,gic-version=3 -cpu $(QEMU_CPU) -smp 1 -m $(QEMU_MEM) \
	  -nographic -no-reboot -kernel out/qemu/Image -initrd $(XNU_PAYLOAD) \
	  -append "$(CMDLINE)" -monitor stdio -serial none \
	  -d int,guest_errors -D out/qemu/xnu-qemu.log >/dev/null
	for f in out/qemu/xnu-*.raw; do $(PYTHON) -I ../tools/fb2png.py $$f 1080 2400 $${f%.raw}.png; done

clean:
	rm -rf out
