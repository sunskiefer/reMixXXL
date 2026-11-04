/* reMixXXL's live screen: a whole-page overlay plane on the Force's display, drawn by the plugin itself.
 *
 * Built on VADER's overlay program (sunskiefer/vader src/overlay.c, 0.5.x, proven on the Force): the plugin runs inside
 * MPC's process, and MPC owns the display through DRM/KMS (the Force: an RK3288, whose display controller has four
 * hardware windows with alpha blending; MPC uses one). This file finds MPC's own DRM handle among the process's open
 * files, picks a spare overlay plane that takes ARGB (or XRGB), gives it a dumb buffer the size of the whole plugin
 * page (1280 x 628) and copies a freshly drawn frame into it up to 30 times a second from a thread of its own, right
 * after the vertical blank, only the rows that changed. Where the page is on the display, and WHICH page shows, is
 * read from the picture MPC is showing: the skin (tools/remix_skin.py) has a 2 x 2 marker at each page's top-left
 * (RX_PAGE_MARK_TL_X/Y from the page origin) and bottom-right (RX_PAGE_MARK_BR_X/Y), in colours that carry the page
 * id (boxes.h RX_MARK_TL / RX_MARK_BR). While both are visible the plane is shown there, else it is hidden (another
 * tab, a popup over it). What to draw is published by the engine every block (overlay_publish: a seqlock copy of
 * rx_view_t); screen.c draws the page's live boxes into the page buffer (everything else transparent).
 *
 * Kept from VADER: the DRM code (find_card_fds, drm_setup, dumb buffers, set_plane), the marker locate (fast path on
 * XRGB scanouts, mapped for one look and released at once), the owner rule (one instance draws), the pacing (30 fps,
 * adaptive when a frame is slow, never under 10/s), the vblank wait and the changed-rows copy.
 * New here: per-page marker pairs, a whole-page plane, the XRGB fallback (no alpha: the plane shows only the page's
 * drawn rectangle, screen_used_rect), the platter frames, the 10 s stats line, and the view is rx_view_t.
 *
 * Everything it finds and does goes to "<plugin folder>/remixxxl_overlay.log" (fresh each start).
 * A file "<plugin folder>/overlay.off" keeps it off altogether. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include "remix.h"
#include "boxes.h"
#include "overlay_priv.h"

#define OVL_FPS 30
#define OVL_W RX_SKIN_W
#define OVL_H RX_SKIN_H
#define OVL_PITCH (OVL_W * 4)
#define MARK_TOL 5                     /* per channel: the page ids are 12 apart */
#define PLATTER_KEEP 4                 /* retired platter strips kept (never freed while the thread may read them) */

typedef struct { uint32_t handle, pitch, fb_id; uint64_t size; uint8_t *map; } ovl_buf_t;

struct rx_overlay {
    char log_path[RX_PATH];
    int started, stop, live, waited_logged, off;
    pthread_t thread;
    /* DRM */
    int fd, crtc_index;
    uint32_t crtc_id, plane_id, format, disp_w, disp_h;
    uint64_t zpos_max;
    int has_zpos;
    ovl_buf_t buf;
    /* the page on the display */
    int page_found, page_logged, page_x, page_y, page_id, on_screen, fmt_unknown, fb_logged, fb_err_logged, no_vblank;
    double ms_avg;
    /* the last log lines, for the on-screen diagnosis (overlay_status) */
    char ring[8][96]; int ring_n;
    /* the snapshot (seqlock) */
    unsigned seq;
    rx_view_t view;
};

/* ------------------------------------------------------------------------------------------------ the log */
static void logf_(rx_overlay_t *o, const char *fmt, ...) {
    if (!o->log_path[0]) return;
    FILE *f = fopen(o->log_path, "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "[%6ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    va_start(ap, fmt);
    vsnprintf(o->ring[o->ring_n % 8], sizeof o->ring[0], fmt, ap);
    va_end(ap);
    o->ring_n++;
}

/* ------------------------------------------------------------------------------------------------ DRM (VADER's) */
static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* the DRM cards this process has open (MPC's own handles): up to 8 candidates; the one that lets us set a plane is
 * MPC's master handle */
static int find_card_fds(rx_overlay_t *o, int *fds, int max) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) { logf_(o, "no /proc/self/fd: %s", strerror(errno)); return 0; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < max) {
        char link[64], target[256];
        int fd = atoi(e->d_name);
        if (fd < 3) continue;
        snprintf(link, sizeof link, "/proc/self/fd/%s", e->d_name);
        ssize_t ln = readlink(link, target, sizeof target - 1);
        if (ln <= 0) continue;
        target[ln] = 0;
        if (strncmp(target, "/dev/dri/card", 13)) continue;
        struct drm_mode_card_res r;
        memset(&r, 0, sizeof r);
        if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) { logf_(o, "fd %d %s: GETRESOURCES: %s", fd, target, strerror(errno)); continue; }
        logf_(o, "fd %d is %s: %u crtcs, %u connectors, %u fbs", fd, target, r.count_crtcs, r.count_connectors, r.count_fbs);
        fds[n++] = fd;
    }
    closedir(d);
    return n;
}

static int get_prop_u64(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *val, uint32_t *prop_id,
                        uint64_t *vmin, uint64_t *vmax) {
    struct drm_mode_obj_get_properties gp;
    memset(&gp, 0, sizeof gp);
    gp.obj_id = obj;
    gp.obj_type = type;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return -1;
    uint32_t n = gp.count_props;
    if (!n || n > 64) return -1;
    uint32_t ids[64];
    uint64_t vals[64];
    gp.props_ptr = (uintptr_t)ids;
    gp.prop_values_ptr = (uintptr_t)vals;
    gp.count_props = n;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return -1;
    for (uint32_t i = 0; i < n && i < gp.count_props; i++) {
        struct drm_mode_get_property p;
        memset(&p, 0, sizeof p);
        p.prop_id = ids[i];
        if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p)) continue;
        if (strcmp(p.name, name)) continue;
        if (val) *val = vals[i];
        if (prop_id) *prop_id = ids[i];
        if ((vmin || vmax) && p.count_values >= 2 && (p.flags & DRM_MODE_PROP_RANGE)) {
            uint64_t range[2];
            p.values_ptr = (uintptr_t)range;
            p.count_values = 2;
            p.count_enum_blobs = 0;
            if (!xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p)) { if (vmin) *vmin = range[0]; if (vmax) *vmax = range[1]; }
        }
        return (int)(p.flags & DRM_MODE_PROP_IMMUTABLE ? 2 : 1);
    }
    return 0;
}

static void log_props(rx_overlay_t *o, int fd, uint32_t obj, uint32_t type, const char *what) {
    struct drm_mode_obj_get_properties gp;
    memset(&gp, 0, sizeof gp);
    gp.obj_id = obj;
    gp.obj_type = type;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return;
    uint32_t n = gp.count_props;
    if (!n || n > 64) return;
    uint32_t ids[64];
    uint64_t vals[64];
    gp.props_ptr = (uintptr_t)ids;
    gp.prop_values_ptr = (uintptr_t)vals;
    gp.count_props = n;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp)) return;
    char line[512];
    int ln = snprintf(line, sizeof line, "  %s %u props:", what, obj);
    for (uint32_t i = 0; i < n && i < gp.count_props && ln < 480; i++) {
        struct drm_mode_get_property p;
        memset(&p, 0, sizeof p);
        p.prop_id = ids[i];
        if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p)) continue;
        ln += snprintf(line + ln, sizeof line - (size_t)ln, " %s=%llu%s", p.name, (unsigned long long)vals[i],
                       (p.flags & DRM_MODE_PROP_IMMUTABLE) ? "(ro)" : "");
    }
    logf_(o, "%s", line);
}

static int create_buffer(rx_overlay_t *o, ovl_buf_t *b) {
    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof cd);
    cd.width = OVL_W;
    cd.height = OVL_H;
    cd.bpp = 32;
    if (xioctl(o->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { logf_(o, "CREATE_DUMB: %s", strerror(errno)); return -1; }
    b->handle = cd.handle;
    b->pitch = cd.pitch;
    b->size = cd.size;
    struct drm_mode_fb_cmd2 fb;
    memset(&fb, 0, sizeof fb);
    fb.width = OVL_W;
    fb.height = OVL_H;
    fb.pixel_format = o->format;
    fb.handles[0] = cd.handle;
    fb.pitches[0] = cd.pitch;
    if (xioctl(o->fd, DRM_IOCTL_MODE_ADDFB2, &fb)) { logf_(o, "ADDFB2 (%.4s): %s", (char *)&o->format, strerror(errno)); return -1; }
    b->fb_id = fb.fb_id;
    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (xioctl(o->fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { logf_(o, "MAP_DUMB: %s", strerror(errno)); return -1; }
    b->map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, o->fd, (off_t)md.offset);
    if (b->map == MAP_FAILED) { b->map = NULL; logf_(o, "mmap dumb: %s", strerror(errno)); return -1; }
    memset(b->map, 0, cd.size);
    logf_(o, "buffer: handle %u fb %u pitch %u size %llu (%dx%d)", b->handle, b->fb_id, b->pitch, (unsigned long long)b->size, OVL_W, OVL_H);
    return 0;
}

static void destroy_buffer(rx_overlay_t *o, ovl_buf_t *b) {
    if (b->map) munmap(b->map, b->size);
    if (b->fb_id) { uint32_t id = b->fb_id; xioctl(o->fd, DRM_IOCTL_MODE_RMFB, &id); }
    if (b->handle) { struct drm_mode_destroy_dumb dd = {.handle = b->handle}; xioctl(o->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd); }
    memset(b, 0, sizeof *b);
}

/* show the plane: the buffer's rectangle r (skin pixels) at the page origin (x, y) + r; fb_id 0 hides it. With an
 * ARGB plane r is the whole page (the undrawn part is transparent); with XRGB only the drawn rectangle of the page
 * is shown, so the skin stays visible around it. */
static int set_plane(rx_overlay_t *o, uint32_t fb_id, int x, int y, const rx_box_t *r) {
    struct drm_mode_set_plane sp;
    memset(&sp, 0, sizeof sp);
    sp.plane_id = o->plane_id;
    sp.crtc_id = o->crtc_id;
    sp.fb_id = fb_id;
    if (fb_id) {
        /* clipped to the display: on the Force the page's bottom can lie below the display's last row (1280 x 720 with MPC's
         * header above the 628 px page), and a plane reaching past the CRTC is refused by the driver */
        int cx = x + r->x, cy = y + r->y, w = r->w, h = r->h, sx = r->x, sy = r->y;
        if (cx < 0) { w += cx; sx -= cx; cx = 0; }
        if (cy < 0) { h += cy; sy -= cy; cy = 0; }
        if (o->disp_w && cx + w > (int)o->disp_w) w = (int)o->disp_w - cx;
        if (o->disp_h && cy + h > (int)o->disp_h) h = (int)o->disp_h - cy;
        if (w <= 0 || h <= 0) { errno = ERANGE; return -1; }
        sp.crtc_x = cx;
        sp.crtc_y = cy;
        sp.crtc_w = (uint32_t)w;
        sp.crtc_h = (uint32_t)h;
        sp.src_x = (uint32_t)sx << 16;
        sp.src_y = (uint32_t)sy << 16;
        sp.src_w = (uint32_t)w << 16;
        sp.src_h = (uint32_t)h << 16;
    }
    return xioctl(o->fd, DRM_IOCTL_MODE_SETPLANE, &sp);
}

/* the display: MPC's CRTC and a spare plane on it that takes ARGB (XRGB as a fallback). 0: ready. */
static int drm_setup_fd(rx_overlay_t *o);
static int drm_setup(rx_overlay_t *o) {
    int fds[8], n = find_card_fds(o, fds, 8);
    if (!n) { logf_(o, "no DRM card among this process's files"); return -1; }
    for (int i = 0; i < n; i++) {
        o->fd = fds[i];
        logf_(o, "trying fd %d", o->fd);
        if (drm_setup_fd(o) == 0) {
            /* can we set a plane on it? (only the display's master can: a disable of our unused plane is harmless) */
            if (set_plane(o, 0, 0, 0, NULL) == 0) return 0;
            logf_(o, "fd %d: SETPLANE: %s (not the master handle?)", o->fd, strerror(errno));
        }
        destroy_buffer(o, &o->buf);
    }
    o->fd = -1;
    return -1;
}
static int drm_setup_fd(rx_overlay_t *o) {
    uint64_t cap = 1;
    struct drm_set_client_cap cc = {.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = cap};
    if (xioctl(o->fd, DRM_IOCTL_SET_CLIENT_CAP, &cc)) logf_(o, "universal planes: %s", strerror(errno));
    /* the active CRTC */
    struct drm_mode_card_res r;
    memset(&r, 0, sizeof r);
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) { logf_(o, "GETRESOURCES: %s", strerror(errno)); return -1; }
    uint32_t crtcs[16], ncrtc = r.count_crtcs > 16 ? 16 : r.count_crtcs;
    memset(&r, 0, sizeof r);
    r.crtc_id_ptr = (uintptr_t)crtcs;
    r.count_crtcs = ncrtc;
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) { logf_(o, "GETRESOURCES 2: %s", strerror(errno)); return -1; }
    o->crtc_id = 0;
    for (uint32_t i = 0; i < ncrtc; i++) {
        struct drm_mode_crtc c;
        memset(&c, 0, sizeof c);
        c.crtc_id = crtcs[i];
        if (xioctl(o->fd, DRM_IOCTL_MODE_GETCRTC, &c)) continue;
        logf_(o, "crtc %u: fb %u, %ux%u at %u,%u, mode %s %ux%u", c.crtc_id, c.fb_id, c.mode.hdisplay, c.mode.vdisplay, c.x, c.y,
              c.mode.name, c.mode.hdisplay, c.mode.vdisplay);
        if (c.fb_id && !o->crtc_id) { o->crtc_id = c.crtc_id; o->crtc_index = (int)i; o->disp_w = c.mode.hdisplay; o->disp_h = c.mode.vdisplay; }
    }
    if (!o->crtc_id) { logf_(o, "no active crtc"); return -1; }
    /* the planes */
    struct drm_mode_get_plane_res pr;
    memset(&pr, 0, sizeof pr);
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) { logf_(o, "GETPLANERESOURCES: %s", strerror(errno)); return -1; }
    uint32_t planes[32], np = pr.count_planes > 32 ? 32 : pr.count_planes;
    pr.plane_id_ptr = (uintptr_t)planes;
    pr.count_planes = np;
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) { logf_(o, "GETPLANERESOURCES 2: %s", strerror(errno)); return -1; }
    logf_(o, "%u planes", np);
    o->plane_id = 0;
    int best_rank = -1;
    for (uint32_t i = 0; i < np; i++) {
        struct drm_mode_get_plane gp;
        memset(&gp, 0, sizeof gp);
        gp.plane_id = planes[i];
        uint32_t fmts[64];
        gp.format_type_ptr = (uintptr_t)fmts;
        gp.count_format_types = 64;
        if (xioctl(o->fd, DRM_IOCTL_MODE_GETPLANE, &gp)) { logf_(o, "plane %u: GETPLANE: %s", planes[i], strerror(errno)); continue; }
        uint64_t type = 99, zpos = 0, zmin = 0, zmax = 0;
        get_prop_u64(o->fd, planes[i], DRM_MODE_OBJECT_PLANE, "type", &type, NULL, NULL, NULL);
        int hasz = get_prop_u64(o->fd, planes[i], DRM_MODE_OBJECT_PLANE, "zpos", &zpos, NULL, &zmin, &zmax);
        int ar24 = 0, xr24 = 0;
        char fl[256];
        int fn = 0;
        for (uint32_t k = 0; k < gp.count_format_types && k < 64; k++) {
            if (fmts[k] == DRM_FORMAT_ARGB8888) ar24 = 1;
            if (fmts[k] == DRM_FORMAT_XRGB8888) xr24 = 1;
            if (fn < 240) fn += snprintf(fl + fn, sizeof fl - (size_t)fn, " %.4s", (char *)&fmts[k]);
        }
        logf_(o, "plane %u: type %s, crtcs 0x%x, in use: crtc %u fb %u, zpos %llu [%llu..%llu]%s, formats:%s",
              planes[i], type == 1 ? "primary" : type == 2 ? "cursor" : type == 0 ? "overlay" : "?", gp.possible_crtcs,
              gp.crtc_id, gp.fb_id, (unsigned long long)zpos, (unsigned long long)zmin, (unsigned long long)zmax,
              hasz == 2 ? "(ro)" : hasz == 1 ? "" : "(none)", fl);
        log_props(o, o->fd, planes[i], DRM_MODE_OBJECT_PLANE, "plane");
        if (!(gp.possible_crtcs & (1u << o->crtc_index))) continue;
        if (gp.fb_id) continue;              /* MPC's own */
        if (type == 1) continue;             /* a primary plane */
        if (!ar24 && !xr24) continue;
        int rank = (type == 0 ? 2 : 1) + (ar24 ? 4 : 0);
        if (rank > best_rank) { best_rank = rank; o->plane_id = planes[i]; o->format = ar24 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888; o->zpos_max = zmax; o->has_zpos = hasz == 1; }
    }
    if (!o->plane_id) { logf_(o, "no spare plane on crtc %u takes ARGB/XRGB", o->crtc_id); return -1; }
    logf_(o, "using plane %u (%.4s) on crtc %u", o->plane_id, (char *)&o->format, o->crtc_id);
    if (o->format == DRM_FORMAT_XRGB8888)
        logf_(o, "XRGB only: no transparency; the plane will cover just the drawn rectangle of each page (the boxes), black behind them");
    if (o->has_zpos && o->zpos_max) {
        uint32_t pid = 0;
        get_prop_u64(o->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "zpos", NULL, &pid, NULL, NULL);
        struct drm_mode_obj_set_property sp = {.value = o->zpos_max, .prop_id = pid, .obj_id = o->plane_id, .obj_type = DRM_MODE_OBJECT_PLANE};
        if (xioctl(o->fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &sp)) logf_(o, "zpos %llu: %s", (unsigned long long)o->zpos_max, strerror(errno));
        else logf_(o, "zpos set to %llu", (unsigned long long)o->zpos_max);
    }
    if (create_buffer(o, &o->buf)) return -1;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ where the page is */
/* MPC's picture, read back from the display (read-only), to find the skin's marker pixels. Mapped for one look and
 * released at once: a mapping kept would pin MPC's buffer, and MPC needs its buffers back when it rebuilds a page
 * (VADER 0.5.1 froze the display on leaving a page). */
typedef struct { uint32_t fb_id; uint8_t *map; uint8_t *base; size_t len; uint32_t pitch, w, h, fmt; } fbmap_t;

static int scanout_open(rx_overlay_t *o, fbmap_t *m) {
    memset(m, 0, sizeof *m);
    struct drm_mode_crtc c;
    memset(&c, 0, sizeof c);
    c.crtc_id = o->crtc_id;
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETCRTC, &c) || !c.fb_id) return -1;
    struct drm_mode_fb_cmd2 f;
    memset(&f, 0, sizeof f);
    f.fb_id = c.fb_id;
    if (xioctl(o->fd, DRM_IOCTL_MODE_GETFB2, &f)) {
        /* an older kernel: the legacy GETFB (a handle, the pitch, bpp and depth; the format follows) */
        struct drm_mode_fb_cmd f1;
        memset(&f1, 0, sizeof f1);
        f1.fb_id = c.fb_id;
        if (xioctl(o->fd, DRM_IOCTL_MODE_GETFB, &f1)) { if (!o->fb_err_logged++) logf_(o, "GETFB2 and GETFB %u: %s", c.fb_id, strerror(errno)); return -1; }
        memset(&f, 0, sizeof f);
        f.fb_id = f1.fb_id;
        f.width = f1.width;
        f.height = f1.height;
        f.pitches[0] = f1.pitch;
        f.handles[0] = f1.handle;
        f.pixel_format = f1.bpp == 32 ? DRM_FORMAT_XRGB8888 : f1.bpp == 24 ? DRM_FORMAT_RGB888 : f1.bpp == 16 ? DRM_FORMAT_RGB565 : 0;
    }
    if (!f.handles[0]) { if (!o->fb_err_logged++) logf_(o, "GETFB %u: no handle (not the master handle, and not root?)", c.fb_id); return -1; }
    struct drm_prime_handle p = {.handle = f.handles[0], .flags = DRM_CLOEXEC};
    int rc = xioctl(o->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p);
    struct drm_gem_close gc = {.handle = f.handles[0]};
    xioctl(o->fd, DRM_IOCTL_GEM_CLOSE, &gc);   /* GETFB gave us a handle of our own: drop it at once */
    if (rc) { if (!o->fb_err_logged++) logf_(o, "PRIME export of fb %u: %s", c.fb_id, strerror(errno)); return -1; }
    size_t len = (size_t)f.pitches[0] * f.height + f.offsets[0];
    uint8_t *mm = mmap(NULL, len, PROT_READ, MAP_SHARED, p.fd, 0);
    close(p.fd);
    if (mm == MAP_FAILED) { if (!o->fb_err_logged++) logf_(o, "mmap of fb %u: %s", c.fb_id, strerror(errno)); return -1; }
    m->fb_id = c.fb_id;
    m->base = mm;
    m->map = mm + f.offsets[0];
    m->len = len;
    m->pitch = f.pitches[0];
    m->w = f.width;
    m->h = f.height;
    m->fmt = f.pixel_format;
    if (!o->fb_logged) { o->fb_logged = 1; logf_(o, "MPC's picture: fb %u, %ux%u %.4s, pitch %u", c.fb_id, f.width, f.height, (char *)&f.pixel_format, f.pitches[0]); }
    return 0;
}

static void scanout_close(fbmap_t *m) {
    if (m->base) munmap(m->base, m->len);
    memset(m, 0, sizeof *m);
}

static inline int px_rgb(const fbmap_t *m, int x, int y, uint8_t *rgb) {
    if (x < 0 || y < 0 || x >= (int)m->w || y >= (int)m->h) return 0;
    const uint8_t *p = m->map + (size_t)y * m->pitch;
    switch (m->fmt) {
    case DRM_FORMAT_XRGB8888: case DRM_FORMAT_ARGB8888: p += x * 4; rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0]; return 1;
    case DRM_FORMAT_XBGR8888: case DRM_FORMAT_ABGR8888: p += x * 4; rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2]; return 1;
    case DRM_FORMAT_RGB888: p += x * 3; rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0]; return 1;
    case DRM_FORMAT_BGR888: p += x * 3; rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2]; return 1;
    case DRM_FORMAT_RGB565: { p += x * 2; uint16_t v = (uint16_t)(p[0] | p[1] << 8); rgb[0] = (uint8_t)((v >> 11) << 3); rgb[1] = (uint8_t)(((v >> 5) & 63) << 2); rgb[2] = (uint8_t)((v & 31) << 3); return 1; }
    default: return 0;
    }
}
static inline int near_(const uint8_t *a, const uint8_t *b) {
    return abs(a[0] - b[0]) <= MARK_TOL && abs(a[1] - b[1]) <= MARK_TOL && abs(a[2] - b[2]) <= MARK_TOL;
}
static int marker_at(const fbmap_t *m, int x, int y, const unsigned char mk[4][3]) {
    uint8_t c[3];
    return px_rgb(m, x, y, c) && near_(c, mk[0]) && px_rgb(m, x + 1, y, c) && near_(c, mk[1]) &&
           px_rgb(m, x, y + 1, c) && near_(c, mk[2]) && px_rgb(m, x + 1, y + 1, c) && near_(c, mk[3]);
}

/* both markers of page p with the page's origin at (ox, oy)? */
static int page_at(const fbmap_t *m, int ox, int oy, int p) {
    return marker_at(m, ox + RX_PAGE_MARK_TL_X, oy + RX_PAGE_MARK_TL_Y, RX_MARK_TL[p]) &&
           marker_at(m, ox + RX_PAGE_MARK_BR_X, oy + RX_PAGE_MARK_BR_Y, RX_MARK_BR[p]);
}

/* the whole-picture search: a pixel near any page's TL[0] colour (23, 7, 91 + 12 p) names the page candidate, then
 * both markers are checked. 1 found: the page origin and id. */
static int find_page(const fbmap_t *m, int *px, int *py, int *pid) {
    const int b0 = RX_MARK_TL[0][0][2];                     /* 91 */
    /* only the two markers must be inside the picture, NOT the whole page: on the Force (1280 x 720, MPC's header above the
     * 628 px page) the page's bottom can be off the display, and 0.1.0/0.1.1 required the whole page to fit, so this loop
     * never ran there (no waves, no touch) */
    const int span_x = RX_PAGE_MARK_BR_X - RX_PAGE_MARK_TL_X + 2, span_y = RX_PAGE_MARK_BR_Y - RX_PAGE_MARK_TL_Y + 2;
    for (int y = RX_PAGE_MARK_TL_Y; y + (span_y > 2 ? span_y : 2) <= (int)m->h; y++) {
        const uint8_t *row = m->map + (size_t)y * m->pitch;
        for (int x = RX_PAGE_MARK_TL_X; x + span_x <= (int)m->w; x++) {
            uint8_t c[3];
            if (m->fmt == DRM_FORMAT_XRGB8888 || m->fmt == DRM_FORMAT_ARGB8888) {   /* the fast path */
                const uint8_t *p = row + x * 4;
                if (abs(p[2] - RX_MARK_TL[0][0][0]) > MARK_TOL || abs(p[1] - RX_MARK_TL[0][0][1]) > MARK_TOL) continue;
                c[0] = p[2]; c[1] = p[1]; c[2] = p[0];
            } else if (!px_rgb(m, x, y, c) || abs(c[0] - RX_MARK_TL[0][0][0]) > MARK_TOL || abs(c[1] - RX_MARK_TL[0][0][1]) > MARK_TOL) continue;
            int p = (c[2] - b0 + 6) / 12;
            if (c[2] < b0 - MARK_TOL || p < 0 || p >= RX_PAGES || abs(c[2] - (b0 + 12 * p)) > MARK_TOL) continue;
            if (page_at(m, x - RX_PAGE_MARK_TL_X, y - RX_PAGE_MARK_TL_Y, p)) {
                *px = x - RX_PAGE_MARK_TL_X;
                *py = y - RX_PAGE_MARK_TL_Y;
                *pid = p;
                return 1;
            }
        }
    }
    return 0;
}

/* 1: a page is on screen at (o->page_x, o->page_y) (display pixels), o->page_id says which; 0: none (another tab,
 * a list over it). full: look over the whole picture when it isn't where it was (a few times a second: it reads a
 * lot of display memory); else only the last place is checked, for every page id (a tab change keeps the origin). */
static int locate(rx_overlay_t *o, int full) {
    fbmap_t m;
    if (scanout_open(o, &m)) { o->fmt_unknown = o->fb_err_logged > 0; return 0; }
    if (!o->disp_w) { o->disp_w = m.w; o->disp_h = m.h; }
    int on = 0;
    if (o->page_found || o->page_x >= 0) {   /* still there? (same page first, then the others at the same origin) */
        if (page_at(&m, o->page_x, o->page_y, o->page_id)) on = 1;
        else for (int p = 0; p < RX_PAGES && !on; p++) if (p != o->page_id && page_at(&m, o->page_x, o->page_y, p)) { o->page_id = p; on = 1; }
        o->page_found = on;
    }
    if (!on && full) {
        int px, py, pid;
        if (find_page(&m, &px, &py, &pid)) {
            o->page_x = px; o->page_y = py; o->page_id = pid;
            o->page_found = on = 1;
            if (!o->page_logged) { o->page_logged = 1; logf_(o, "the page is at %d,%d on the display (%ux%u %.4s), page %d", px, py, m.w, m.h, (char *)&m.fmt, pid); }
        }
    }
    scanout_close(&m);
    return on;
}

/* ------------------------------------------------------------------------------------------------ the platter frames */
/* The strip is process-wide (screen_draw has no instance handle, and only the owner draws): the newest strip is the
 * one shown; older strips are kept (never freed while the drawing thread might still be reading one) up to
 * PLATTER_KEEP, then the oldest is dropped (a strip is set once per skin load, so that never happens in practice). */
static pthread_mutex_t g_platter_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_platter;
static int g_platter_w, g_platter_h, g_platter_n;
static uint8_t *g_platter_old[PLATTER_KEEP];
static int g_platter_nold;

void overlay_set_platter_frames(rx_overlay_t *o, const uint8_t *argb, int w, int h, int frames) {
    if (!argb || w <= 0 || h <= 0 || frames <= 0) return;
    size_t n = (size_t)w * h * 4 * frames;
    uint8_t *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, argb, n);
    pthread_mutex_lock(&g_platter_lock);
    if (g_platter) {
        if (g_platter_nold == PLATTER_KEEP) { free(g_platter_old[0]); memmove(g_platter_old, g_platter_old + 1, sizeof(uint8_t *) * (PLATTER_KEEP - 1)); g_platter_nold--; }
        g_platter_old[g_platter_nold++] = g_platter;
    }
    g_platter_w = w; g_platter_h = h; g_platter_n = frames;
    __atomic_store_n(&g_platter, copy, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_platter_lock);
    if (o) logf_(o, "platter frames: %d of %dx%d", frames, w, h);
}

const uint8_t *overlay_platter_frames(int *w, int *h, int *n) {
    const uint8_t *p = __atomic_load_n(&g_platter, __ATOMIC_ACQUIRE);
    if (!p) return NULL;
    *w = g_platter_w; *h = g_platter_h; *n = g_platter_n;
    return p;
}

/* ------------------------------------------------------------------------------------------------ the thread */
/* a seqlock read: copy, and again if the writer was in the middle of it */
static void read_locked(unsigned *seq, const void *src, void *dst, size_t n) {
    unsigned s0, s1;
    do {
        s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (s0 & 1) { usleep(50); continue; }
        memcpy(dst, src, n);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    } while (s0 != s1 || (s0 & 1));
}

static pthread_mutex_t g_owner_lock = PTHREAD_MUTEX_INITIALIZER;
static rx_overlay_t *g_owner;   /* one reMixXXL instance in this process draws the live screen */

static void *ovl_thread(void *p) {
    rx_overlay_t *o = p;
    /* a low priority (VADER 0.5.1): MPC's own screen and touch come first on the core we share (the audio runs on
     * cores of its own); when the device is busy this thread drops frames instead of making MPC lag */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    logf_(o, "reMixXXL overlay starting (page %dx%d, %d pages)", OVL_W, OVL_H, RX_PAGES);
    /* the owner: the first instance; another waits for it to go (its page can't be told apart on the display) */
    for (;;) {
        pthread_mutex_lock(&g_owner_lock);
        if (!g_owner) g_owner = o;
        int mine = g_owner == o;
        pthread_mutex_unlock(&g_owner_lock);
        if (mine) break;
        if (o->stop) return NULL;
        if (!o->waited_logged) { o->waited_logged = 1; logf_(o, "another reMixXXL instance has the live screen: waiting"); }
        for (int i = 0; i < 20 && !o->stop; i++) usleep(100000);
    }
    int tries = 0;
    while (!o->stop && drm_setup(o)) {
        destroy_buffer(o, &o->buf);
        if (++tries >= 3) { logf_(o, "giving up: no live screen"); goto give_up; }
        logf_(o, "retry %d in 3 s", tries);
        for (int i = 0; i < 30 && !o->stop; i++) usleep(100000);
    }
    if (o->stop) goto give_up;
    uint8_t *frame_mem = calloc((size_t)OVL_H, OVL_PITCH), *prev_mem = calloc((size_t)OVL_H, OVL_PITCH);
    if (!frame_mem || !prev_mem) { logf_(o, "out of memory"); free(frame_mem); free(prev_mem); goto give_up; }
    __atomic_store_n(&o->live, 1, __ATOMIC_RELEASE);
    logf_(o, "live: drawing up to %d frames a second", OVL_FPS);
    struct timespec next, t_start;
    clock_gettime(CLOCK_MONOTONIC, &next);
    t_start = next;
    int frame = 0, shown = 0, fails = 0, shown_x = -1, shown_y = -1, shown_page = -1, stat_frames = 0, last_stat_s = 0;
    double busy = 0, stat_busy = 0;
    rx_box_t shown_rect = {0, 0, 0, 0};
    static rx_view_t v;
    while (!o->stop) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int on = 0;
        /* is a page on the display? 10 looks a second while it is (16 pixels to read); 2 of them whole-picture
         * searches while it isn't (a lot of display memory, so not every frame) */
        if (frame % 3 == 0) on = locate(o, frame % 15 == 0), o->on_screen = on;
        else on = o->on_screen;
        int page = o->page_id;
        if (on && !o->fmt_unknown) {
            read_locked(&o->seq, &o->view, &v, sizeof v);
            /* drawn in ordinary memory, then copied into the plane's buffer in one pass (display memory is slow to
             * read back, and a copy tears less than drawing in place); the plane is set once, when shown or when the
             * page moves or changes, so MPC's own screen updates never wait on a call of ours */
            double now = (t0.tv_sec - t_start.tv_sec) + (t0.tv_nsec - t_start.tv_nsec) * 1e-9;
            screen_draw(page, &v, frame_mem, OVL_W, OVL_H, OVL_PITCH, now);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            /* into the plane's buffer right after the display's vertical blank (a plain wait: MPC is not involved), so
             * the copy is done before the scan reaches the page: no tearing. Only the rows that changed are copied. */
            if (!o->no_vblank) {
                union drm_wait_vblank wv;
                memset(&wv, 0, sizeof wv);
                wv.request.type = _DRM_VBLANK_RELATIVE | (o->crtc_index ? (unsigned)(o->crtc_index << _DRM_VBLANK_HIGH_CRTC_SHIFT) & _DRM_VBLANK_HIGH_CRTC_MASK : 0);
                wv.request.sequence = 1;
                if (xioctl(o->fd, DRM_IOCTL_WAIT_VBLANK, &wv)) { o->no_vblank = 1; logf_(o, "WAIT_VBLANK: %s (copying without it)", strerror(errno)); }
            }
            ovl_buf_t *b = &o->buf;
            for (int y = 0; y < OVL_H; y++) {
                const uint8_t *src = frame_mem + (size_t)y * OVL_PITCH;
                uint8_t *prev = prev_mem + (size_t)y * OVL_PITCH;
                if (!memcmp(src, prev, OVL_PITCH)) continue;
                memcpy(b->map + (size_t)y * b->pitch, src, OVL_PITCH);
                memcpy(prev, src, OVL_PITCH);
            }
            /* the plane's rectangle: the whole page on ARGB; on XRGB the page's drawn rectangle only */
            rx_box_t rect = {0, 0, OVL_W, OVL_H};
            if (o->format == DRM_FORMAT_XRGB8888) screen_used_rect(page, &rect);
            int want = rect.w > 0 && rect.h > 0;
            if (!want) {
                if (shown) { set_plane(o, 0, 0, 0, NULL); shown = 0; logf_(o, "hidden (page %d has nothing live)", page); }
            } else if (!shown || o->page_x != shown_x || o->page_y != shown_y || page != shown_page || memcmp(&rect, &shown_rect, sizeof rect)) {
                if (set_plane(o, b->fb_id, o->page_x, o->page_y, &rect)) {
                    if (++fails == 1 || fails % 300 == 0) logf_(o, "SETPLANE: %s (%d so far)", strerror(errno), fails);
                    if (errno == EACCES || errno == EPERM) { logf_(o, "not the display's master: no live screen"); __atomic_store_n(&o->live, 0, __ATOMIC_RELEASE); break; }
                } else {
                    shown = 1; shown_x = o->page_x; shown_y = o->page_y; shown_page = page; shown_rect = rect;
                    logf_(o, "shown at %d,%d page %d rect %d,%d %dx%d", o->page_x, o->page_y, page, rect.x, rect.y, rect.w, rect.h);
                }
            }
        } else if (shown) {
            set_plane(o, 0, 0, 0, NULL);
            shown = 0;
            logf_(o, "hidden (%s)", o->fmt_unknown ? "the display's picture can't be read" : "no page of ours is on the display");
        }
        if (!(on && !o->fmt_unknown)) clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        busy += ms;
        stat_busy += ms;
        frame++;
        stat_frames++;
        /* every 10 s: frames, ms a frame, shown/hidden, page */
        int sec = (int)(t1.tv_sec - t_start.tv_sec);
        if (sec / 10 != last_stat_s / 10) {
            logf_(o, "stats: %d frames (%d total), %.2f ms a frame (lately %.2f), %s, page %d, pace %.1f/s",
                  stat_frames, frame, stat_frames ? stat_busy / stat_frames : 0.0, o->ms_avg, shown ? "shown" : "hidden", page,
                  stat_frames / 10.0);
            stat_frames = 0; stat_busy = 0;
        }
        last_stat_s = sec;
        /* the pace: 30 frames a second when a frame takes under half of that; slower when the device is slower, so
         * this thread never takes a whole core from MPC */
        o->ms_avg += (ms - o->ms_avg) * 0.05;
        long period = 1000000000L / OVL_FPS;
        if (o->ms_avg * 2.2 > 1000.0 / OVL_FPS) period = (long)(o->ms_avg * 2.2e6);
        if (period > 100000000L) period = 100000000L;   /* never under 10 a second */
        next.tv_nsec += period;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - next.tv_sec) * 1e9 + (now.tv_nsec - next.tv_nsec) > 2e8) next = now;   /* far behind: don't catch up */
    }
    set_plane(o, 0, 0, 0, NULL);
    destroy_buffer(o, &o->buf);
    free(frame_mem);
    free(prev_mem);
    __atomic_store_n(&o->live, 0, __ATOMIC_RELEASE);
    logf_(o, "stopped after %d frames (%.2f ms a frame)", frame, frame ? busy / frame : 0.0);
give_up:
    pthread_mutex_lock(&g_owner_lock);
    if (g_owner == o) g_owner = NULL;
    pthread_mutex_unlock(&g_owner_lock);
    return NULL;
}

/* ------------------------------------------------------------------------------------------------ the engine's side */
static void write_locked(unsigned *seq, void *dst, const void *src, size_t n) {
    __atomic_store_n(seq, *seq + 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(dst, src, n);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(seq, *seq + 1, __ATOMIC_RELEASE);
}
void overlay_publish(rx_overlay_t *o, const rx_view_t *v) { if (o) write_locked(&o->seq, &o->view, v, sizeof *v); }
void overlay_read_view(rx_overlay_t *o, rx_view_t *v) { if (o) read_locked(&o->seq, &o->view, v, sizeof *v); else memset(v, 0, sizeof *v); }
/* the on-screen diagnosis: line i (0 = the newest) of the last log lines; 0 when there is none */
int overlay_status(rx_overlay_t *o, int i, char *buf, int n) {
    if (!o) { snprintf(buf, (size_t)n, "SCREEN: not started"); return i == 0; }
    int cnt = o->ring_n < 8 ? o->ring_n : 8;
    if (i < 0 || i >= cnt) return 0;
    snprintf(buf, (size_t)n, "%s", o->ring[(o->ring_n - 1 - i) % 8]);
    return 1;
}
int overlay_live(rx_overlay_t *o) { return o ? __atomic_load_n(&o->live, __ATOMIC_ACQUIRE) : 0; }

int overlay_page_shown(rx_overlay_t *o) {
    if (!o || !__atomic_load_n(&o->live, __ATOMIC_ACQUIRE) || !__atomic_load_n(&o->on_screen, __ATOMIC_ACQUIRE)) return -1;
    return __atomic_load_n(&o->page_id, __ATOMIC_ACQUIRE);
}

int overlay_box_origin(rx_overlay_t *o, int *x, int *y) {
    if (!o || !__atomic_load_n(&o->page_found, __ATOMIC_ACQUIRE)) return 0;
    if (x) *x = __atomic_load_n(&o->page_x, __ATOMIC_ACQUIRE);
    if (y) *y = __atomic_load_n(&o->page_y, __ATOMIC_ACQUIRE);
    return 1;
}

rx_overlay_t *overlay_start(const char *plugin_dir, int *ok) {
    if (ok) *ok = 0;
    rx_overlay_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->fd = -1;
    o->page_x = -1;
    o->page_y = -1;
    if (!plugin_dir || !plugin_dir[0]) return o;
    char path[RX_PATH];
    snprintf(path, sizeof path, "%s/overlay.off", plugin_dir);
    if (access(path, F_OK) == 0) { o->off = 1; return o; }      /* asked to stay off */
    snprintf(o->log_path, sizeof o->log_path, "%s/remixxxl_overlay.log", plugin_dir);
    FILE *f = fopen(o->log_path, "w");   /* a fresh log each start */
    if (f) fclose(f); else { o->log_path[0] = 0; return o; }   /* can't write there (tests): no overlay */
    if (access("/proc/self/fd", R_OK)) { logf_(o, "no /proc/self/fd: no live screen"); return o; }
    o->started = 1;
    if (pthread_create(&o->thread, NULL, ovl_thread, o)) { o->started = 0; logf_(o, "no thread: %s", strerror(errno)); return o; }
    if (ok) *ok = 1;   /* the thread is up; whether a plane was found is in the log and overlay_live() */
    return o;
}

void overlay_stop(rx_overlay_t *o) {
    if (!o) return;
    if (o->started) {
        o->stop = 1;
        pthread_join(o->thread, NULL);
        o->started = 0;
    }
    free(o);
}
