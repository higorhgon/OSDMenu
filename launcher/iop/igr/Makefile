# igr.irx: in-game reset combos for the PS1 games launched via Ember, which has none (see common/include/eigr.h)

IOP_SRC_DIR := src/
IOP_OBJS_DIR := obj/
IOP_INCS := -Isrc/include -I../../../common/include

IOP_BIN  = igr.irx
IOP_OBJS = igr.o ioplib.o imports.o

IOP_CFLAGS  += -Wall -fno-builtin
IOP_LDFLAGS += -s

all:: $(IOP_BIN)

clean::
	rm -f -r $(IOP_BIN) $(IOP_OBJS_DIR) *.elf

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
