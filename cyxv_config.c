/*
 * cyxv_config.c — Config file loader for CyXV-D3D
 */

#include "cyxv_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Defaults ─────────────────────────────────────────────────────────── */

static void set_defaults(CyxvConfig *cfg) {
    cfg->xvcompat     = 1;      /* present as "XVideo" by default */
    cfg->ring_drop_log = 0;
    strncpy(cfg->display, ":0", sizeof(cfg->display) - 1);
}

/* ── String helpers ───────────────────────────────────────────────────── */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e-1))) e--;
    *e = '\0';
    return s;
}

static int parse_bool(const char *val, int *out) {
    if (!strcasecmp(val, "true")  || !strcmp(val, "1") ||
        !strcasecmp(val, "yes")   || !strcasecmp(val, "on")) {
        *out = 1; return 1;
    }
    if (!strcasecmp(val, "false") || !strcmp(val, "0") ||
        !strcasecmp(val, "no")    || !strcasecmp(val, "off")) {
        *out = 0; return 1;
    }
    return 0;
}

/* ── Parse one key=value line ─────────────────────────────────────────── */

static void apply_kv(CyxvConfig *cfg, const char *key, const char *val) {
    if (!strcmp(key, "xvcompat")) {
        parse_bool(val, &cfg->xvcompat);
    } else if (!strcmp(key, "display")) {
        strncpy(cfg->display, val, sizeof(cfg->display) - 1);
    } else if (!strcmp(key, "ring_drop_log")) {
        parse_bool(val, &cfg->ring_drop_log);
    }
    /* Unknown keys are silently ignored for forward compatibility */
}

/* ── File parser ──────────────────────────────────────────────────────── */

static void parse_file(CyxvConfig *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    fprintf(stderr, "[CyXV] Config: %s\n", path);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Strip comments */
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *s = trim(line);
        if (!*s) continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (*key && *val)
            apply_kv(cfg, key, val);
    }
    fclose(f);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void cyxv_config_load(CyxvConfig *cfg) {
    set_defaults(cfg);

    /* 1. Environment variable override */
    const char *env = getenv("CYXV_CONFIG");
    if (env) { parse_file(cfg, env); return; }

    /* 2. User config */
    const char *home = getenv("HOME");
    if (home) {
        char path[512];
        snprintf(path, sizeof(path), "%s/.config/cyxv.conf", home);
        parse_file(cfg, path);
    }

    /* 3. System config (may supplement or override user config) */
    parse_file(cfg, "/etc/cyxv.conf");
}

const char *cyxv_extension_name(const CyxvConfig *cfg) {
    return cfg->xvcompat ? "XVideo" : "CyXV-D3D";
}
