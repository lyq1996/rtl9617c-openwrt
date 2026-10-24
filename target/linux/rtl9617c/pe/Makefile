# Build the native PE program and inject pe-tx-code.h into the rtl_ax_ap
# driver. The packet engines run bare MIPS32 code; the driver loads the
# program at probe time.
#
# Requires host clang-18, ld.lld-18 and llvm-objcopy-18:
#   make -C target/linux/rtl9617c/pe
PE := $(CURDIR)
DRIVER := $(PE)/../files-6.18/drivers/net/wireless/realtek/rtl_ax_ap
CC := clang-18
LINKER := ld.lld-18
OBJCOPY := llvm-objcopy-18
OUT := $(PE)/build

.PHONY: all clean
all: $(DRIVER)/pe-tx-code.h
	@printf 'Native PE WD firmware: %s bytes\n' "$$(wc -c < $(OUT)/pe-tx.bin | tr -d ' ')"

$(OUT)/pe-wd.o: $(PE)/rtl9617c-radio-wd.c $(PE)/rtl9617c-radio-uplink.h
	@mkdir -p $(OUT)
	$(CC) --target=mipsel-none-elf -march=mips32 -msoft-float -mno-abicalls -fno-pic -G0 \
	    -ffreestanding -fno-builtin -fno-stack-protector -fomit-frame-pointer -O2 -DPE_UPLINK \
	    -Wall -Wextra -Werror \
	    -I $(DRIVER) -I $(PE)/../files-6.18/include \
	    -c $< -o $@

$(OUT)/pe-entry.o: $(PE)/rtl9617c-radio-wd-entry.S
	@mkdir -p $(OUT)
	$(CC) --target=mipsel-none-elf -march=mips32 -msoft-float -mno-abicalls -fno-pic -G0 \
	    -c $< -o $@

$(OUT)/pe-wd.elf: $(OUT)/pe-wd.o $(OUT)/pe-entry.o $(PE)/rtl9617c-radio-wd.ld
	$(LINKER) -m elf32ltsmip -T $(PE)/rtl9617c-radio-wd.ld $^ -o $@

$(OUT)/pe-tx.bin: $(OUT)/pe-wd.elf
	$(OBJCOPY) -O binary $< $@

$(DRIVER)/pe-tx-code.h: $(OUT)/pe-tx.bin
	python3 -c 'from pathlib import Path; import struct, sys; \
	code = Path("$(OUT)/pe-tx.bin").read_bytes(); \
	assert len(code) % 4 == 0 and len(code) <= 0x8000, "bad PE program size"; \
	Path("$(DRIVER)/pe-tx-code.h").write_text("static const u32 pe_tx_program[] = {\n" + \
	    "".join("0x%08x,\n" % w for w in struct.unpack("<%dI" % (len(code) // 4), code)) + "};\n")'

clean:
	rm -rf $(OUT)
