# Smash&Clash for the PSP, built with the PSPDEV toolchain (https://pspdev.github.io).
#   make            -> EBOOT.PBP (copy it to ms0:/PSP/GAME/SmashAndClash/, or open it in PPSSPP)
# Needs PSPDEV (with its bundled mbedTLS) on the PATH, and Python 3 with Pillow and
# fontTools for the art (pip install pillow fonttools).

# The art comes from the real game (cached in ../tools/cache): the card table and fonts
# shared with the DS build, and the GPU's textures.
ifeq ($(wildcard build/assets/assets.h),)
    $(info Making the art: python3 ../tools/make_assets.py psp build/assets)
    $(shell python3 ../tools/make_assets.py psp build/assets 1>&2)
endif
ifeq ($(wildcard build/tex/textures.h),)
    $(info Making the textures: python3 ../tools/make_psp_textures.py build/tex)
    $(shell python3 ../tools/make_psp_textures.py build/tex 1>&2)
endif

TARGET = smashandclash
# the core, minus the software renderer (the GPU draws here)
CORE = $(patsubst %.c,%.o,$(filter-out ../core/snc_gfx.c ../core/snc_draw.c,$(wildcard ../core/*.c)))
OBJS = source/main.o source/net_psp.o source/gx.o source/view3d.o $(CORE) \
	build/assets/assets_data.o build/tex/textures_data.o build/tex/textures_bin.o

INCDIR = source ../core build/assets build/tex
CFLAGS = -O2 -G0 -Wall -DSNC_PIXEL_PSP
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)

LIBS = -lmbedtls -lmbedx509 -lmbedcrypto -lpspgu -lpspnet_apctl -lpspnet_resolver -lpspnet_inet -lpspnet -lpsputility -lpsprtc -lpsppower -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Smash&Clash
PSP_EBOOT_ICON = build/assets/ICON0.PNG
PSP_EBOOT_PIC1 = build/assets/PIC1.PNG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# the textures, pulled in with .incbin
build/tex/textures_bin.o: build/tex/textures_bin.s build/tex/textures.bin
	$(CC) -G0 -c -o $@ $<
