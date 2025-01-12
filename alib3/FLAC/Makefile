# libFLAC

include ../makeconf

VER := 1.3.3
URL := http://downloads.xiph.org/releases/flac/flac-$(VER).tar.xz
PKG := $(notdir $(URL))
DIR := flac-$(VER)
LIB := libFLAC-ff.$(SO)

all: $(DIR)
	cd $(DIR) && ./configure --disable-ogg
	cd $(DIR) && cat ../*.patch | patch -p1
	$(MAKE) -rR $(LIB)

# download
$(PKG):
	$(CURL) -o $(PKG) $(URL)

# unpack
$(DIR): $(PKG)
	$(UNTAR_XZ) $(PKG)
	touch $(DIR)

# build
CFLAGS += -funroll-loops \
	-I. -I$(DIR) -I$(DIR)/src/libFLAC/include -I$(DIR)/include \
	-DHAVE_CONFIG_H -DFLAC_EXP -D_FORTIFY_SOURCE=2 \
	-Wall -Wextra -Wstrict-prototypes -Waggregate-return \
	-Wcast-align -Wnested-externs -Wshadow -Wundef -Wunreachable-code -Winline -Wdeclaration-after-statement \
	-Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-implicit-fallthrough

SRC := \
	$(DIR)/src/libFLAC/bitmath.c \
	$(DIR)/src/libFLAC/bitreader.c \
	$(DIR)/src/libFLAC/bitwriter.c \
	$(DIR)/src/libFLAC/cpu.c \
	$(DIR)/src/libFLAC/crc.c \
	$(DIR)/src/libFLAC/fixed.c \
	$(DIR)/src/libFLAC/fixed_intrin_sse2.c \
	$(DIR)/src/libFLAC/fixed_intrin_ssse3.c \
	$(DIR)/src/libFLAC/float.c \
	$(DIR)/src/libFLAC/format.c \
	$(DIR)/src/libFLAC/lpc.c \
	$(DIR)/src/libFLAC/lpc_intrin_sse.c \
	$(DIR)/src/libFLAC/lpc_intrin_sse2.c \
	$(DIR)/src/libFLAC/lpc_intrin_sse41.c \
	$(DIR)/src/libFLAC/lpc_intrin_avx2.c \
	$(DIR)/src/libFLAC/md5.c \
	$(DIR)/src/libFLAC/memory.c \
	$(DIR)/src/libFLAC/stream_decoder.c \
	$(DIR)/src/libFLAC/stream_encoder.c \
	$(DIR)/src/libFLAC/stream_encoder_intrin_sse2.c \
	$(DIR)/src/libFLAC/stream_encoder_intrin_ssse3.c \
	$(DIR)/src/libFLAC/stream_encoder_intrin_avx2.c \
	$(DIR)/src/libFLAC/stream_encoder_framing.c \
	$(DIR)/src/libFLAC/window.c
OBJ := FLAC-ff.o $(SRC:.c=.o)

FLAC-ff.o: FLAC-ff.c
	$(C) $(CFLAGS) $< -o $@

%.o: %.c
	$(C) $(CFLAGS) $< -o $@

$(LIB): $(OBJ)
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

clean:
	$(RM) $(OBJ) $(DIR)
