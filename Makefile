# CyXV-D3D — Makefile
#
# Targets:
#   cyxv.dll          — XV shim injected into XWin.exe via LD_PRELOAD
#   phoenix_receiver  — Standalone H.264 stream receiver with XShm display
#
# Prerequisites (Cygwin packages):
#   libX11-devel  libXext-devel  libavcodec-devel  libswscale-devel
#   libavutil-devel  pkg-config  gcc

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
LDFLAGS =

# ── cyxv.dll (LD_PRELOAD shim) ───────────────────────────────────────────────

CYXV_SRC = cyxv_init.c cyxv_dispatch.c
CYXV_OBJ = $(CYXV_SRC:.c=.o)

cyxv.dll: $(CYXV_OBJ)
	$(CC) -shared -o $@ $^ \
	    -lX11 -lXext -lpthread \
	    $(LDFLAGS)
	@echo ""
	@echo "Built $@"
	@echo "Usage:"
	@echo "  LD_PRELOAD=\$$PWD/cyxv.dll XWin :0 -multiwindow -listen tcp &"
	@echo "  DISPLAY=:0 xdpyinfo | grep XVideo"

$(CYXV_OBJ): %.o: %.c cyxv_xvproto.h cyxv_dispatch.h
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# ── phoenix_receiver ─────────────────────────────────────────────────────────

RECV_SRC = phoenix_receiver.c
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
	@echo "  make cyxv.dll         Build XV injection shim"
	@echo "  make phoenix_receiver Build H.264 stream receiver"
	@echo "  make all              Build both"
	@echo ""
	@echo "Test XV registration:"
	@echo "  LD_PRELOAD=\$$PWD/cyxv.dll XWin :0 -multiwindow -listen tcp &"
	@echo "  sleep 3"
	@echo "  DISPLAY=:0 xdpyinfo | grep XVideo"
	@echo "  DISPLAY=:0 mplayer -vo xv your_video.mp4"
