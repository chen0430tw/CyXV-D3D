"""
phoenix_streamer.py — Windows-side screen capture + H.264 encode + TCP send

Supports two capture backends:
  --backend printwindow   GDI PrintWindow (default, captures a specific HWND)
  --backend dxgi          DXGI Desktop Duplication via dxgi_capture.exe
                          (lower latency, full monitor, requires dxgi_capture.exe)

Usage:
  python phoenix_streamer.py                    # PrintWindow, picks HWND interactively
  python phoenix_streamer.py --backend dxgi     # DXGI full monitor #0
  python phoenix_streamer.py --backend dxgi --monitor 1  # second monitor
"""

import argparse
import socket
import struct
import subprocess
import sys
import threading
import numpy as np
import os

HOST = "127.0.0.1"
PORT = 9999
MAGIC = 0x50484F58
FMT_H264 = 2

# ── PrintWindow backend ───────────────────────────────────────────────────────

def _import_win32():
    import win32gui, win32ui, ctypes
    return win32gui, win32ui, ctypes

def list_windows():
    win32gui, _, _ = _import_win32()
    windows = []
    def cb(hwnd, _):
        title = win32gui.GetWindowText(hwnd)
        if title and win32gui.IsWindowVisible(hwnd):
            windows.append((hwnd, title))
    win32gui.EnumWindows(cb, None)
    for i, (_, title) in enumerate(windows):
        print(f"{i}  {title}")
    return windows

def capture_printwindow(hwnd):
    win32gui, win32ui, ctypes = _import_win32()
    user32 = ctypes.windll.user32
    left, top, right, bottom = win32gui.GetClientRect(hwnd)
    w = (right  - left) // 2 * 2   # libx264 needs even dimensions
    h = (bottom - top)  // 2 * 2

    hwndDC   = win32gui.GetWindowDC(hwnd)
    mfcDC    = win32ui.CreateDCFromHandle(hwndDC)
    saveDC   = mfcDC.CreateCompatibleDC()
    bmp      = win32ui.CreateBitmap()
    bmp.CreateCompatibleBitmap(mfcDC, w, h)
    saveDC.SelectObject(bmp)
    user32.PrintWindow(hwnd, saveDC.GetSafeHdc(), 3)
    raw  = bmp.GetBitmapBits(True)

    win32gui.DeleteObject(bmp.GetHandle())
    saveDC.DeleteDC()
    mfcDC.DeleteDC()
    win32gui.ReleaseDC(hwnd, hwndDC)

    img = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, 4)
    return img[:, :, :3].copy(), w, h   # BGR24

# ── DXGI backend ──────────────────────────────────────────────────────────────

def start_dxgi_capture(monitor=0):
    """Launch dxgi_capture.exe and return (process, width, height)."""
    exe = os.path.join(os.path.dirname(__file__), "dxgi_capture.exe")
    if not os.path.exists(exe):
        sys.exit(f"[streamer] dxgi_capture.exe not found at {exe}\n"
                 "Build it with: cl /EHsc /O2 dxgi_capture.cpp /link dxgi.lib d3d11.lib")
    proc = subprocess.Popen(
        [exe, str(monitor)],
        stdout=subprocess.PIPE,
        stderr=None,
        bufsize=0,
    )
    # Read the first frame header to get W/H
    hdr = proc.stdout.read(8)
    if len(hdr) < 8:
        sys.exit("[streamer] dxgi_capture.exe did not send a frame header")
    w, h = struct.unpack("<II", hdr)
    return proc, w, h

def read_dxgi_frame(proc, w, h):
    """Read one BGR24 frame from dxgi_capture.exe stdout."""
    size = w * h * 3
    buf = bytearray(size)
    view = memoryview(buf)
    total = 0
    while total < size:
        chunk = proc.stdout.read(size - total)
        if not chunk:
            return None
        view[total:total+len(chunk)] = chunk
        total += len(chunk)
    # Next frame starts with a new [w,h] header
    hdr = proc.stdout.read(8)
    if hdr and len(hdr) == 8:
        nw, nh = struct.unpack("<II", hdr)
        if nw != w or nh != h:
            return None   # resolution changed — caller should reinit
    return np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 3)

# ── FFmpeg encoder ────────────────────────────────────────────────────────────

def start_encoder(w, h):
    cmd = [
        "ffmpeg", "-loglevel", "error",
        "-f", "rawvideo", "-pix_fmt", "bgr24",
        "-s", f"{w}x{h}", "-r", "60", "-i", "-",
        "-pix_fmt", "yuv420p",
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-tune", "zerolatency",
        "-profile:v", "baseline", "-level", "5.1",
        "-x264-params",
        "threads=1:bframes=0:rc-lookahead=0:sync-lookahead=0:repeat-headers=1",
        "-fflags", "nobuffer+flush_packets",
        "-f", "h264", "-",
    ]
    return subprocess.Popen(cmd,
                            stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE,
                            bufsize=0)

# ── NAL sender (runs in its own thread to avoid read/write deadlock) ──────────

def nal_sender(encoder_stdout, sock, w, h):
    """Read H.264 output from FFmpeg and send framed NAL units over TCP."""
    buf = b""
    try:
        while True:
            chunk = encoder_stdout.read(65536)
            if not chunk:
                break
            buf += chunk

            # Flush complete NAL units (delimited by 00 00 00 01)
            while True:
                pos = buf.find(b"\x00\x00\x00\x01", 4)
                if pos == -1:
                    break
                nal = buf[:pos]
                buf = buf[pos:]
                if len(nal) < 5:
                    continue
                hdr = struct.pack("!IIIII", MAGIC, w, h, FMT_H264, len(nal))
                sock.sendall(hdr)
                sock.sendall(nal)

        # Flush the last NAL unit buffered after the loop
        if len(buf) >= 5:
            hdr = struct.pack("!IIIII", MAGIC, w, h, FMT_H264, len(buf))
            sock.sendall(hdr)
            sock.sendall(buf)
    except OSError:
        pass

# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=["printwindow", "dxgi"],
                        default="printwindow")
    parser.add_argument("--monitor", type=int, default=0,
                        help="Monitor index for DXGI backend")
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()

    # ── connect to receiver ──
    sock = socket.socket()
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        sock.connect((args.host, args.port))
    except ConnectionRefusedError:
        sys.exit(f"Cannot connect to {args.host}:{args.port} — is phoenix_receiver running?")

    # ── pick capture source ──
    if args.backend == "dxgi":
        dxgi_proc, w, h = start_dxgi_capture(args.monitor)
        print(f"[streamer] DXGI backend: monitor {args.monitor}, {w}x{h}")
        encoder = start_encoder(w, h)
        print(f"[streamer] Encoder started: {w}x{h}")

        # Sender thread decouples encoder stdout reads from stdin writes
        sender = threading.Thread(
            target=nal_sender, args=(encoder.stdout, sock, w, h), daemon=True)
        sender.start()

        try:
            while True:
                frame = read_dxgi_frame(dxgi_proc, w, h)
                if frame is None:
                    break
                try:
                    encoder.stdin.write(memoryview(frame))
                except OSError:
                    break
        except KeyboardInterrupt:
            print("\n[streamer] Stopped (DXGI)")
        finally:
            dxgi_proc.terminate()
            encoder.terminate()
            sock.close()

    else:   # printwindow
        windows = list_windows()
        try:
            idx  = int(input("Select window index: "))
            hwnd = windows[idx][0]
        except (IndexError, ValueError):
            sys.exit("Invalid index")

        _, w, h = capture_printwindow(hwnd)
        encoder = start_encoder(w, h)
        print(f"[streamer] Encoder started: {w}x{h}")

        sender = threading.Thread(
            target=nal_sender, args=(encoder.stdout, sock, w, h), daemon=True)
        sender.start()

        try:
            while True:
                frame, fw, fh = capture_printwindow(hwnd)
                if fw != w or fh != h:
                    break   # window resized — would need encoder reinit
                try:
                    encoder.stdin.write(memoryview(frame))
                except OSError:
                    break
        except KeyboardInterrupt:
            print("\n[streamer] Stopped (PrintWindow)")
        finally:
            encoder.terminate()
            sock.close()

if __name__ == "__main__":
    main()
