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

CFLAGS  := -std=c23 -Wall -Iinclude
LDFLAGS :=

all: $(BIN)

$(OBJS): %.o: %.c
	"$(CC)" $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJS)
	@mkdir -p "$(dir $@)"
	"$(CC)" $(OBJS) $(LDFLAGS) -o $@

clean:
	rm -f $(BIN) $(OBJS)
