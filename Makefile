.PHONY: all clean

ifndef PS5_PAYLOAD_SDK
    PS5_PAYLOAD_SDK = /opt/ps5-payload-sdk/
endif

# make cannot handle spaces in include paths, so transparently use a
# no-space symlink when the SDK lives in a directory with spaces.
PS5_PAYLOAD_SDK := $(shell p='$(PS5_PAYLOAD_SDK)';   if printf '%s' "$$p" | grep -q ' '; then     mkdir -p /tmp/ps5sdk_link; ln -sfn "$$p" /tmp/ps5sdk_link/sdk;     printf '%s' /tmp/ps5sdk_link/sdk;   else printf '%s' "$$p"; fi)

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

BIN := bin/ps5-linux-loader.elf
SRC := $(wildcard source/*.c)
OBJS := $(SRC:.c=.o)

# Build-time render of tools/images.json into a C header the payload embeds.
# The payload WebUI lists the official .img download URLs so the user can
# download the image (R2) and transfer it to the PS5 (USB or M.2 via FTP).
GEN_MANIFEST := include/images_manifest.h

CFLAGS  := -std=c23 -Wall -Iinclude
LDFLAGS := -L$(PS5_PAYLOAD_SDK)/target/lib -lSceHttp2 -lSceHttp -lSceSsl -lSceNet -lSceNetCtl -lpthread

all: $(GEN_MANIFEST) $(BIN)

$(GEN_MANIFEST): tools/images.json tools/gen_manifest.py
	python3 tools/gen_manifest.py

$(OBJS): %.o: %.c $(GEN_MANIFEST)
	"$(CC)" $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJS)
	@mkdir -p "$(dir $@)"
	"$(CC)" $(OBJS) $(LDFLAGS) -o $@

clean:
	rm -f $(BIN) $(OBJS) $(GEN_MANIFEST)
