#ifndef CYXV_CONFIG_H
#define CYXV_CONFIG_H

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
 */

typedef struct {
    int  xvcompat;          /* bool: present as "XVideo"      */
    char display[64];       /* render thread display string   */
    int  ring_drop_log;     /* bool: log ring-full drops      */
} CyxvConfig;

/* Load config; populates *cfg with defaults then overrides from file. */
void cyxv_config_load(CyxvConfig *cfg);

/* Return the extension name to pass to AddExtension() */
const char *cyxv_extension_name(const CyxvConfig *cfg);

#endif /* CYXV_CONFIG_H */
