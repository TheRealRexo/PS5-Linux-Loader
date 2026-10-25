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
CFLAGS  = -Os -fomit-frame-pointer -fno-stack-protector -ffreestanding \
	-nostdlib -fPIE -fno-plt -fcf-protection=none -mno-sse -mno-sse2 -m64 \
	-ffunction-sections -fdata-sections -DFW_1160=1 -I. -I../shellcode_kernel
LDFLAGS = -pie -T ../shellcode_kernel/linker.ld \
	-Wl,--gc-sections,--defsym,SHELLCODE_LIMIT=0x5000
TARGET  = shellcode_kernel_1160.elf
TEXT_BIN = shellcode_kernel_1160.bin
dump = shellcode_kernel_1160.h

SRC = main.c boot_linux.c utils.c
OBJ = $(SRC:.c=.o) display_ops_1160.o
HEADERS = ../shellcode_kernel/main.h ../shellcode_kernel/boot_linux.h \
	../shellcode_kernel/utils.h ../shellcode_kernel/shellcode_kernel_args.h \
	../include/config.h ../include/linux.h ../shellcode_hv/shellcode_hv.h \
	display_ops_1160.h shellcode_kernel_args_validate.h

all: $(dump)

# The blob is linked at 0x1000 but runs from the kernel code cave, so it has
# to be position independent with no relocations left for objcopy to drop.
# It copies the HV into the 0x2000 bytes in front of linux_info.
$(TARGET): $(OBJ) ../shellcode_kernel/linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) -o $(TARGET)
	$(READELF) -h $(TARGET) | grep -q 'Type:[[:space:]]*DYN'
	$(READELF) -r $(TARGET) | grep -q 'There are no relocations in this file'
	test "$$(wc -c < ../shellcode_hv/shellcode_hv.bin)" -le 8192

$(SRC:.c=.o): %.o: ../shellcode_kernel/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

display_ops_1160.o: display_ops_1160.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# update_sck_args_ptr only looks for the args marker at offsets below 0x40, so
# it has to be in the first 71 bytes, and be the only one in the blob.
MARKER = aa 11 aa 11 aa 11 aa 11
count_marker = xxd -p -c1 | tr '\n' ' ' | grep -o '$(MARKER)' | grep -c ''

$(TEXT_BIN): $(TARGET)
	$(OBJCOPY) -O binary -j .shell_code $(TARGET) $(TEXT_BIN)
	test "$$(cat $(TEXT_BIN) | $(count_marker))" -eq 1
	test "$$(head -c 71 $(TEXT_BIN) | $(count_marker))" -eq 1

clean:
	rm -f $(OBJ) $(TARGET) $(TEXT_BIN) $(dump)

$(dump): $(TEXT_BIN)
	xxd -i $(TEXT_BIN) > $(dump)

.PHONY: all clean
.DELETE_ON_ERROR:
