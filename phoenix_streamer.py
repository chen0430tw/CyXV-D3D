import socket
import struct
import subprocess
import numpy as np
import win32gui
import win32ui
import ctypes
import os

# 設定伺服器資訊
HOST = "127.0.0.1"
PORT = 9999
MAGIC = 0x50484F58
FMT_H264 = 2

user32 = ctypes.windll.user32

def capture(hwnd):
    left, top, right, bottom = win32gui.GetClientRect(hwnd)
    w = right - left
    h = bottom - top

    # 強制寬高為偶數 (FFmpeg libx264 要求)
    w = w // 2 * 2
    h = h // 2 * 2

    hwndDC = win32gui.GetWindowDC(hwnd)
    mfcDC = win32ui.CreateDCFromHandle(hwndDC)
    saveDC = mfcDC.CreateCompatibleDC()
    saveBitMap = win32ui.CreateBitmap()
    saveBitMap.CreateCompatibleBitmap(mfcDC, w, h)
    saveDC.SelectObject(saveBitMap)

    # 使用 PrintWindow 抓取畫面
    user32.PrintWindow(hwnd, saveDC.GetSafeHdc(), 3)
    bmpstr = saveBitMap.GetBitmapBits(True)

    img = np.frombuffer(bmpstr, dtype=np.uint8)
    img.shape = (h, w, 4)
    frame = img[:, :, :3].copy() # 只要 BGR 三通道

    win32gui.DeleteObject(saveBitMap.GetHandle())
    saveDC.DeleteDC()
    mfcDC.DeleteDC()
    win32gui.ReleaseDC(hwnd, hwndDC)

    return frame, w, h

def list_windows():
    windows = []
    def cb(hwnd, _):
        title = win32gui.GetWindowText(hwnd)
        if title and win32gui.IsWindowVisible(hwnd):
            windows.append((hwnd, title))
    win32gui.EnumWindows(cb, None)
    for i, (_, title) in enumerate(windows):
        print(f"{i} {title}")
    return windows

def start_encoder(w, h):
    cmd = [
        "ffmpeg",
        "-loglevel", "error",
        "-f", "rawvideo",
        "-pix_fmt", "bgr24", # 輸入格式
        "-s", f"{w}x{h}",
        "-r", "60",
        "-i", "-",

        # 關鍵修正點：在這裡強制轉成 420p，對應 baseline profile
        "-pix_fmt", "yuv420p", 

        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-tune", "zerolatency",
        "-profile:v", "baseline", # 現在 420p 支援 baseline 了
        "-level", "5.1",

        "-x264-params", "threads=1:bframes=0:rc-lookahead=0:sync-lookahead=0:repeat-headers=1",
        
        "-fflags", "nobuffer+flush_packets",
        "-f", "h264",
        "-"
    ]
    return subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        bufsize=0
    )

def main():
    windows = list_windows()
    try:
        idx = int(input("選擇窗口序號: "))
        hwnd = windows[idx][0]
    except (IndexError, ValueError):
        print("序號錯誤")
        return

    sock = socket.socket()

    # 禁用 Nagle（低延迟）
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    try:
        sock.connect((HOST, PORT))
    except ConnectionRefusedError:
        print(f"無法連接到 {HOST}:{PORT}，請確認 Receiver 已啟動")
        return

    # 先抓一幀確認尺寸
    _, w, h = capture(hwnd)
    encoder = start_encoder(w, h)
    print(f"Encoder started: {w}x{h}")

    buffer = b""

    try:
        while True:
            # 1. 抓圖並餵給 FFmpeg
            frame, _, _ = capture(hwnd)
            try:
                encoder.stdin.write(memoryview(frame))
                encoder.stdin.flush()
            except OSError:
                break

            # 2. 讀取編碼後的 NAL 數據
            # 使用 read1 避免 AttributeError，並提高讀取效率
            chunk = encoder.stdout.read(65536)
            if not chunk:
                continue
            
            buffer += chunk

            # 3. 精準切割 NAL 單元
            while True:
                # 尋找下一個 NAL 起始碼 (從 index 4 開始找)
                pos = buffer.find(b"\x00\x00\x00\x01", 4)
                
                # 如果找不到下一個起始碼，代表這包資料還沒完，跳出等待更多資料
                if pos == -1:
                    break

                # 切出完整的 NAL (包含目前的 00 00 00 01，但不含下一個起始碼)
                nal = buffer[:pos]
                buffer = buffer[pos:] # 剩下的留到下次處理

                if len(nal) < 5:
                    continue

                # 封裝自定義協議頭並發送
                header = struct.pack(
                    "!IIIII",
                    MAGIC,
                    w,
                    h,
                    FMT_H264,
                    len(nal)
                )
                sock.sendall(header)
                sock.sendall(nal)

    except KeyboardInterrupt:
        print("停止推流")
    finally:
        encoder.terminate()
        sock.close()

if __name__ == "__main__":
    main()
