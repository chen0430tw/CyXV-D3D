# CyXV-D3D

**The missing XVideo (XV) bridge.**

**Why?**

VcXsrv/Cygwin-X lacks hardware XV. GDI is slow. CPU is crying.

**How?**
Shim: Fake libXv to trick clients.
Transport: MIT-SHM (cygserver) for zero-copy.
Render: D3D12 Compute Shaders (YUV to RGB).
Fallback: Auto-bridge to WSLg if present.

**Usage**
export LD_PRELOAD=./libcyxv_shim.so
mplayer -vo xv video.mp4
