# gamescan.irx: experimental live games scan for the OSDMenu games submenu (games_live_scan)

IOP_SRC_DIR := src/
IOP_OBJS_DIR := obj/
IOP_INCS := -Isrc/include -I../../../common/include

IOP_BIN  = gamescan.irx
IOP_OBJS = gamescan.o imports.o

IOP_CFLAGS  += -Wall -fno-builtin -DIOMANX_OLD_NAME_COMPATIBILITY=0 -DIOMANX_OLD_NAME_ADDDELDRV=0
IOP_LDFLAGS += -s

all:: $(IOP_BIN)

clean::
	rm -f -r $(IOP_BIN) $(IOP_OBJS_DIR) *.elf

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
