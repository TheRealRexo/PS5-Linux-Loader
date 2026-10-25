ifeq ($(shell uname -m),aarch64)
CC      = x86_64-linux-gnu-gcc
LD      = x86_64-linux-gnu-ld
OBJCOPY = x86_64-linux-gnu-objcopy
READELF = x86_64-linux-gnu-readelf
else
CC      = gcc
LD      = ld
OBJCOPY = objcopy
READELF = readelf
endif
CFLAGS  = -O2 -ffreestanding -nostdlib -fno-pic -fno-plt \
	-fno-stack-protector -fno-stack-clash-protection -fomit-frame-pointer \
	-fno-asynchronous-unwind-tables -fcf-protection=none -fno-jump-tables \
	-mgeneral-regs-only -mno-red-zone -Wall -Wextra
TARGET  = break_hv.elf
TEXT_BIN = break_hv.bin
dump = break_hv.h

OBJ = entry.o break_hv.o

all: $(dump)

entry.o: entry.S
	$(CC) -c $< -o $@

break_hv.o: break_hv.c break_hv_params.h
	$(CC) $(CFLAGS) -c $< -o $@

# The payload is linked at 0 but copied to and run from another address, so
# only PC-relative relocations are allowed.
$(TARGET): $(OBJ) link.ld
	$(READELF) -rW $(OBJ) >/dev/null
	! $(READELF) -rW $(OBJ) | grep R_X86_64_ | grep -v -E 'R_X86_64_(PC32|PLT32) '
	$(LD) -nostdlib -no-pie -T link.ld $(OBJ) -o $(TARGET)

$(TEXT_BIN): $(TARGET)
	$(OBJCOPY) -O binary $(TARGET) $(TEXT_BIN)

clean:
	rm -f $(OBJ) $(TARGET) $(TEXT_BIN) $(dump)

$(dump): $(TEXT_BIN)
	xxd -i $(TEXT_BIN) > $(dump)

.PHONY: all clean
.DELETE_ON_ERROR:
