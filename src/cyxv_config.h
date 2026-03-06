#ifndef CYXV_CONFIG_H
#define CYXV_CONFIG_H

#include <stdint.h>

/*
 * cyxv_config.h — Runtime configuration for CyXV-D3D
 *
 * Config file location (first match wins):
 *   $CYXV_CONFIG               environment variable override
 *   $HOME/.config/cyxv.conf    user config
 *   /etc/cyxv.conf             system config
 *
 * Format: plain key = value, # comments, blank lines ignored.
 *
 * Supported keys:
 *
 *   xvcompat = true | false
 *     true  → register extension as "XVideo"
 *             XV-aware apps (mplayer, mpv, xine, vlc) will detect it.
 *     false → register extension as "CyXV-D3D"
 *             No XV compatibility; extension is still fully functional
 *             but only apps that explicitly query "CyXV-D3D" will use it.
 *     Default: true
 *
 *   display = :0
 *     X display for the render thread's private connection.
 *     Default: :0
 *
 *   ring_drop_log = true | false
 *     Log a message when frames are dropped due to full ring buffer.
 *     Default: false (silent drop, avoids flooding stderr)
 *
 *   rva_add_extension = 0x48a4e0
 *     RVA (file offset from XWin.exe image base) of AddExtension().
 *     Used when dlsym fails (stripped binary).  Find the correct value:
 *
 *       # 1. Get PE preferred image base
 *       objdump -p /usr/bin/XWin.exe | grep ImageBase
 *       # e.g. "ImageBase               0000000100400000"
 *
 *       # 2. Run XWin with cyxv.dll and read the log — it prints:
 *       #    [CyXV] image_base = 0x<runtime_base>
 *
 *       # 3. Find AddExtension VA with GDB:
 *       gdb -p $(pgrep XWin) -ex "p &AddExtension" -batch 2>/dev/null
 *       # or via signature scan:
 *       strings -t x /usr/bin/XWin.exe | grep "^.\{1,8\} RANDR$"
 *       # pick an extension name VA, trace calls into AddExtension
 *
 *       # 4. RVA = VA - runtime_base   (or VA - PE_image_base for objdump VAs)
 *
 *     Default: 0 (disabled; falls back to dlsym only)
 *
 *   rva_write_to_client = 0x546de0
 *     RVA of WriteToClient().  Same method as above.
 *     Default: 0
 */

typedef struct {
    int      xvcompat;          /* bool: present as "XVideo"      */
    char     display[64];       /* render thread display string   */
    int      ring_drop_log;     /* bool: log ring-full drops      */
    uint64_t rva_add_extension;    /* 0 = disabled                 */
    uint64_t rva_write_to_client;
    uint64_t rva_init_extensions;  /* hook target; 0 = use thread  */
} CyxvConfig;

/* Load config; populates *cfg with defaults then overrides from file. */
void cyxv_config_load(CyxvConfig *cfg);

/* Return the extension name to pass to AddExtension() */
const char *cyxv_extension_name(const CyxvConfig *cfg);

#endif /* CYXV_CONFIG_H */
