# CyXV-D3D — Makefile
#
# Targets:
#   cyxv.dll          — XV extension DLL injected into XWin.exe via LD_PRELOAD
#   phoenix_receiver  — Standalone H.264 stream receiver with XShm display
#
# Prerequisites (Cygwin packages):
#   libX11-devel  libXext-devel  libavcodec-devel  libswscale-devel
#   libavutil-devel  pkg-config  gcc

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
LDFLAGS =

# ── cyxv.dll ─────────────────────────────────────────────────────────────────

CYXV_SRC = src/cyxv_init.c src/cyxv_dispatch.c src/cyxv_config.c
CYXV_OBJ = $(CYXV_SRC:.c=.o)
CYXV_HDR = src/cyxv_xvproto.h src/cyxv_dispatch.h src/cyxv_config.h

cyxv.dll: $(CYXV_OBJ)
	$(CC) -shared -o $@ $^ \
	    -lX11 -lXext -lpthread \
	    $(LDFLAGS)
	@echo ""
	@echo "Built $@"
	@echo "Usage:"
	@echo "  LD_PRELOAD=\$$PWD/cyxv.dll XWin :0 -multiwindow -listen tcp &"
	@echo "  DISPLAY=:0 xdpyinfo | grep XVideo"
	@echo ""
	@echo "Config: ~/.config/cyxv.conf"
	@echo "  xvcompat = true   # present as XVideo (default)"
	@echo "  xvcompat = false  # present as CyXV-D3D"

src/%.o: src/%.c $(CYXV_HDR)
	$(CC) $(CFLAGS) -fPIC -Isrc -c $< -o $@

# ── phoenix_receiver ─────────────────────────────────────────────────────────

RECV_SRC = phoenix/phoenix_receiver.c
RECV_BIN = phoenix_receiver

FFMPEG_CFLAGS  := $(shell pkg-config --cflags libavcodec libavutil libswscale 2>/dev/null \
                   || echo "-I/usr/include/ffmpeg")
FFMPEG_LIBS    := $(shell pkg-config --libs   libavcodec libavutil libswscale 2>/dev/null \
                   || echo "-lavcodec -lavutil -lswscale")

$(RECV_BIN): $(RECV_SRC)
	$(CC) $(CFLAGS) $(FFMPEG_CFLAGS) $< -o $@ \
	    -lX11 -lXext \
	    $(FFMPEG_LIBS) \
	    $(LDFLAGS)
	@echo "Built $@"

# ── Phony targets ─────────────────────────────────────────────────────────────

.PHONY: all clean help

all: cyxv.dll $(RECV_BIN)

clean:
	rm -f $(CYXV_OBJ) cyxv.dll $(RECV_BIN)

help:
	@echo "Targets:"
	@echo "  make cyxv.dll         Build XV extension DLL"
	@echo "  make phoenix_receiver Build H.264 stream receiver"
	@echo "  make all              Build both"
	@echo ""
	@echo "Config: ~/.config/cyxv.conf"
	@echo "  xvcompat = true       Register as XVideo (mplayer/mpv compatible)"
	@echo "  xvcompat = false      Register as CyXV-D3D (native name)"
	@echo "  display  = :0         Render thread display"
	@echo ""
	@echo "Test:"
	@echo "  LD_PRELOAD=\$$PWD/cyxv.dll XWin :0 -multiwindow -listen tcp &"
	@echo "  sleep 3 && DISPLAY=:0 xdpyinfo | grep -E 'XVideo|CyXV'"
	@echo "  DISPLAY=:0 mplayer -vo xv your_video.mp4"
