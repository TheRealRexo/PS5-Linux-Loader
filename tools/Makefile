CC ?= gcc
CFLAGS ?= -Os -static
LDFLAGS ?=

.PHONY: all clean

all: m2_init

m2_init: m2_init.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f m2_init
