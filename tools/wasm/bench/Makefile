# SPDX-FileCopyrightText: 2026 TabStationToo contributors
# SPDX-License-Identifier: MIT
#
# Builds bench.elf with the ps2dev toolchain (README.md in this folder).

EE_BIN = bench.elf
EE_OBJS = bench_vu1.o bench.o
EE_LIBS = -ldraw -lgraph -lmath3d -lpacket2 -ldma -lm
EE_CFLAGS += -O2 -std=gnu99 -Wall
EE_DVP = dvp-as

all: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)

%.o: %.vsm
	$(EE_DVP) $< -o $@

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

# Keeps bench.elf, which is committed.
clean-objs:
	rm -f $(EE_OBJS)

ifneq ($(PS2SDK),)
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
endif
