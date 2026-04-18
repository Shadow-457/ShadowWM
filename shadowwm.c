/* shadowwm v5  ─  Apple/GNOME-inspired Compositing Window Manager
 * ================================================================
 * 20 Advanced Features:
 *  01  macOS traffic-light buttons LEFT, title CENTERED
 *  02  6-px XShape rounded window corners
 *  03  Gaussian-approximation XRender soft shadows (8 layers)
 *  04  Pure OLED-black palette  (#0a0a0a / #111111)
 *  05  4 virtual workspaces  (Super+1-4)
 *  06  Tabbed window frames  (Super+T attach, Super+U detach)
 *  07  Picture-in-Picture   (Super+P — sticky, always-on-top)
 *  08  Per-window opacity   (right-click titlebar → Opacity)
 *  09  Quarter-tiling       (6 zones: halves + 4 corners + ghost preview)
 *  10  Thumbnail-grid switcher (Super+Tab — Exposé style)
 *  11  Spring-physics animations (250-350 ms, critically damped)
 *  12  Invisible 6-px 8-direction resize grab zones
 *  13  Dim unfocused windows (0.82 opacity via compositor)
 *  14  Always-on-top / always-on-bottom  (right-click menu)
 *  15  Window rules — save/restore x,y,w,h,desk per WM_CLASS
 *  16  Fullscreen (Super+F, hides titlebar)
 *  17  Fade-in on map, fade-out on close / minimise
 *  18  Screen-edge snap while moving (16 px zone)
 *  19  Full EWMH: _NET_CLIENT_LIST, _NET_ACTIVE_WINDOW,
 *               _NET_WM_STATE, _NET_WM_DESKTOP, _NET_CURRENT_DESKTOP
 *  20  JSON config: ~/.config/shadowwm/config.json
 *
 * Build:
 *   gcc -O2 -Wall -o shadowwm shadowwm.c \
 *     -lX11 -lXft -lXcomposite -lXdamage -lXrender -lXfixes -lXext -lm
 *
 * Keyboard shortcuts (Super = Windows/Meta key):
 *   Super+Return        Terminal
 *   Super+D / Super+F2  Launcher
 *   Super+Q             Close focused window
 *   Super+1-4           Switch workspace
 *   Super+Shift+1-4     Move window to workspace
 *   Super+Tab           Thumbnail switcher
 *   Super+Left/Right    Tile left/right half
 *   Super+Up            Maximize
 *   Super+Down          Un-maximize / minimize
 *   Super+Ctrl+Left     Tile top-left quarter
 *   Super+Ctrl+Right    Tile top-right quarter
 *   Super+Ctrl+Shift+L  Tile bottom-left quarter
 *   Super+Ctrl+Shift+R  Tile bottom-right quarter
 *   Super+F             Fullscreen toggle
 *   Super+P             Picture-in-Picture toggle
 *   Super+T             Tab: merge focused into window under cursor
 *   Super+U             Untab: detach focused from tab group
 *   Super+Shift+Q       Quit WM
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

/* ══════════════════════════════════════════
   CONSTANTS & LAYOUT
══════════════════════════════════════════ */
#define TITLEBAR_H    30
#define TAB_H         22
#define CORNER_R       7
#define BORDER_W       1
#define RESIZE_B       6
#define BTN_D         13
#define BTN_PAD_L     12
#define BTN_GAP        8
#define MAX_CLIENTS  512
#define MAX_RULES     64
#define MAX_TABS       8
#define NUM_DESKS      4
#define MOD          Mod4Mask
#define SNAP_PX       16
#define TILE_ZONE     32
#define DBL_MS       350
#define MIN_W         80
#define MIN_H         40
#define FADE_STEPS    16
#define FADE_US     8000
#define PIP_W        320
#define PIP_H        200
#define SHADOW_LAYERS  8
#define SHADOW_DX      8
#define SHADOW_DY     10
#define SHADOW_EXPAND  3
#define DIM_OPACITY  0.82f
#define FONT_NAME    "Sans:size=9:weight=medium"
#define TERMINAL     "xterm"
#define LAUNCHER     "dmenu_run"

/* Resize direction bits */
#define RD_N (1<<0)
#define RD_S (1<<1)
#define RD_W (1<<2)
#define RD_E (1<<3)

/* Frame dims — chrome_h stored per-client */
#define RB         RESIZE_B
#define FW(c)      ((c)->w + 2*RB)
#define FH(c)      ((c)->chrome_h + (c)->h)

/* ══════════════════════════════════════════
   OLED-BLACK PALETTE
══════════════════════════════════════════ */
#define C_BAR_FOC_T  "#111111"
#define C_BAR_FOC_B  "#0a0a0a"
#define C_BAR_UNF_T  "#0d0d0d"
#define C_BAR_UNF_B  "#080808"
#define C_TXT_FOC    "#e2e2e2"
#define C_TXT_UNF    "#303030"
#define C_SEP_FOC    "#222222"
#define C_SEP_UNF    "#111111"
#define C_BDR_FOC    "#2a2a2a"
#define C_BDR_UNF    "#141414"
#define C_TAB_ACT    "#1e1e1e"
#define C_TAB_INACT  "#0f0f0f"
#define C_TAB_TXT    "#c0c0c0"
/* macOS traffic-light colours */
#define C_CLOSE      "#FF5F56"
#define C_CLOSE_H    "#FF8080"
#define C_MIN        "#FFBD2E"
#define C_MIN_H      "#FFD060"
#define C_MAX        "#27C93F"
#define C_MAX_H      "#50E060"
/* accent */
#define C_ACCENT     "#5e81f4"

/* ══════════════════════════════════════════
   SPRING PHYSICS
══════════════════════════════════════════ */
typedef struct {
    float pos, vel, target;
    int   active;
} Spring;

static void __attribute__((unused)) spring_set(Spring *s, float target) {
    s->target = target;
    s->active = (fabsf(target - s->pos) > 0.3f || fabsf(s->vel) > 0.3f);
}
static void __attribute__((unused)) spring_snap(Spring *s, float val) {
    s->pos = s->target = val; s->vel = 0; s->active = 0;
}
/* Returns 1 if still animating */
static int __attribute__((unused)) spring_step(Spring *s, float dt) {
    if (!s->active) { s->pos = s->target; return 0; }
    float k = 280.0f, d = 22.0f;
    float acc = k * (s->target - s->pos) - d * s->vel;
    s->vel += acc * dt;
    s->pos += s->vel * dt;
    if (fabsf(s->target - s->pos) < 0.4f && fabsf(s->vel) < 0.4f) {
        s->pos = s->target; s->vel = 0; s->active = 0; return 0;
    }
    return 1;
}

/* ══════════════════════════════════════════
   WINDOW RULES
══════════════════════════════════════════ */
typedef struct {
    char  wm_class[128];
    int   x, y, w, h;
    int   desk;
    int   valid;
} WinRule;

/* ══════════════════════════════════════════
   TAB GROUP
══════════════════════════════════════════ */
typedef struct {
    int members[MAX_TABS];  /* indices into clients[] */
    int count;
    int active;             /* index of visible member in members[] */
} TabGroup;

/* ══════════════════════════════════════════
   CLIENT
══════════════════════════════════════════ */
typedef struct {
    Window  frame, client;
    int     x, y, w, h;
    int     sx, sy, sw, sh;    /* saved geometry */
    int     chrome_h;           /* TITLEBAR_H or TITLEBAR_H+TAB_H */
    int     maximized;          /* 1=full, 2=horz, 3=vert */
    int     minimized;
    int     fullscreen;
    int     above, below;
    int     focused;
    int     is_panel;           /* 0=normal 1=desktop 2=dock 3=other */
    int     pip;                /* picture-in-picture mode */
    int     pip_sx, pip_sy, pip_sw, pip_sh;
    int     desk;               /* 0-3, or -1=all desks (sticky) */
    int     tab_group;          /* index into tab_groups[], -1=none */
    float   opacity;
    char    title[256];
    char    wm_class[128];
    /* compositor */
    Pixmap  cpx;
    Picture cpic;
    Damage  cdmg;
    int     dirty;
    /* spring animations for open/close */
    Spring  sp_opacity;
} Client;

/* ══════════════════════════════════════════
   GLOBALS
══════════════════════════════════════════ */
static Display   *dpy;
static Window     root;
static int        screen, scr_w, scr_h;
static int        work_x, work_y, work_w, work_h;
static int        current_desk = 0;

static Client     clients[MAX_CLIENTS];
static int        nc = 0;
static GC         gc;
static XftFont   *xfont;

static TabGroup   tab_groups[MAX_CLIENTS / 2];
static int        ntg = 0;

static WinRule    rules[MAX_RULES];
static int        nr = 0;

/* atoms */
static Atom WM_DEL, WM_PROTO, WM_TAKE_FOCUS;
static Atom NET_WM_TYPE, NET_WM_TYPE_DESKTOP, NET_WM_TYPE_DOCK;
static Atom NET_WM_TYPE_DIALOG, NET_WM_TYPE_SPLASH, NET_WM_TYPE_TOOLTIP;
static Atom NET_WM_TYPE_UTILITY, NET_WM_TYPE_POPUP, NET_WM_TYPE_NOTIFICATION;
static Atom NET_WM_STATE, NET_WM_STATE_MAX_H, NET_WM_STATE_MAX_V;
static Atom NET_WM_STATE_HIDDEN, NET_WM_STATE_FULLSCREEN;
static Atom NET_WM_STATE_ABOVE, NET_WM_STATE_BELOW;
static Atom NET_WM_STRUT, NET_WM_STRUT_PARTIAL;
static Atom NET_ACTIVE_WIN, NET_CLIENT_LIST;
static Atom NET_SUPPORTED, NET_WM_NAME;
static Atom NET_WM_DESKTOP, NET_CURRENT_DESKTOP, NET_NUMBER_DESKTOPS;
static Atom UTF8_STRING;
static Atom WM_CLASS_ATOM;

/* cursors */
static Cursor cur_normal, cur_move, cur_resize[16];

/* drag state */
static Window drag_frame = None;
static int    drag_mode  = 0;
static int    drag_dir   = 0;
static int    dsx, dsy, dwx, dwy, dww, dwh;

/* hover */
static Window hover_frame = None;
static int    hover_btn   = 0;
static Window last_cur_frame = None;
static int    last_cur_zone  = -1;

/* double-click */
static Window dbl_win  = None;
static Time   dbl_time = 0;

/* tile ghost overlay */
static Window ghost_win   = None;
static int    ghost_zone  = 0;

/* compositor */
static int      comp_ok     = 0;
static int      dmg_base    = 0;
static Pixmap   buf_px      = None;
static Picture  buf_pic     = None;
static Picture  root_pic    = None;
static XRenderPictFormat *vis_fmt = NULL;

/* thumbnail switcher */
static Window   switcher_win  = None;
static int      switcher_sel  = 0;
static int      switcher_open = 0;

/* config */
static char cfg_terminal[256];
static char cfg_launcher[256];

/* ══════════════════════════════════════════
   FORWARD DECLARATIONS
══════════════════════════════════════════ */
static void draw_frame(Client *c);
static void focus_client(Client *c);
static void unframe_window(Window w);
static void toggle_maximize(Client *c);
static void comp_repaint(void);
static void comp_get_pixmap(Client *c);
static void switch_desk(int d);
static void tab_redraw_active(int tg);
static void switcher_close_fn(void);
static void set_client_opacity(Client *c, float op);

/* ══════════════════════════════════════════
   HELPERS
══════════════════════════════════════════ */
static Client *by_frame(Window w)  { for(int i=0;i<nc;i++) if(clients[i].frame ==w) return &clients[i]; return NULL; }
static Client *by_client(Window w) { for(int i=0;i<nc;i++) if(clients[i].client==w) return &clients[i]; return NULL; }
static int     cidx(Client *c)    { return (int)(c - clients); }

static unsigned long xcolor(const char *s) {
    XColor c; XParseColor(dpy,DefaultColormap(dpy,screen),s,&c);
    XAllocColor(dpy,DefaultColormap(dpy,screen),&c); return c.pixel;
}
static void hex2rgb(const char *h, int *r, int *g, int *b) {
    unsigned rv=0,gv=0,bv=0; sscanf(h+1,"%02x%02x%02x",&rv,&gv,&bv);
    *r=(int)rv; *g=(int)gv; *b=(int)bv;
}
static unsigned long rgb_px(int r, int g, int b) {
    Visual *v = DefaultVisual(dpy,screen);
    unsigned long rm=v->red_mask,gm=v->green_mask,bm=v->blue_mask,rs=0,gs=0,bs=0,t;
    for(t=rm;t&&!(t&1);t>>=1)rs++;
    for(t=gm;t&&!(t&1);t>>=1)gs++;
    for(t=bm;t&&!(t&1);t>>=1)bs++;
    return((unsigned long)r<<rs)|((unsigned long)g<<gs)|((unsigned long)b<<bs);
}
static void spawn(const char *cmd) {
    if(fork()==0){setsid();execlp("sh","sh","-c",cmd,NULL);exit(0);}
}

/* ══════════════════════════════════════════
   JSON CONFIG  (minimal parser)
══════════════════════════════════════════ */
static char *json_get_str(const char *json, const char *key, char *out, int maxlen) {
    char search[128]; snprintf(search,sizeof(search),"\"%s\"",key);
    const char *p = strstr(json,search);
    if(!p) return NULL;
    p += strlen(search);
    while(*p==':'||*p==' '||*p=='\t') p++;
    if(*p=='"'){ p++; int i=0; while(*p&&*p!='"'&&i<maxlen-1) out[i++]=*p++; out[i]=0; return out; }
    return NULL;
}
static int json_get_int(const char *json, const char *key, int def) {
    char search[128]; snprintf(search,sizeof(search),"\"%s\"",key);
    const char *p = strstr(json,search);
    if(!p) return def;
    p += strlen(search);
    while(*p==':'||*p==' '||*p=='\t') p++;
    if(*p=='-'||((*p>='0')&&(*p<='9'))) return atoi(p);
    return def;
}

static void config_dir(char *buf, int sz) {
    const char *h = getenv("HOME"); if(!h) h="/root";
    snprintf(buf,sz,"%s/.config/shadowwm",h);
}
static void config_path(char *buf, int sz) {
    char d[496]; config_dir(d,sizeof(d));
    snprintf(buf,sz,"%s/config.json",d);
}

static void load_config(void) {
    /* defaults */
    strncpy(cfg_terminal,TERMINAL,sizeof(cfg_terminal)-1);
    strncpy(cfg_launcher,LAUNCHER,sizeof(cfg_launcher)-1);

    char path[512]; config_path(path,sizeof(path));
    FILE *f = fopen(path,"r");
    if(!f) return;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0){fclose(f);return;}
    char *json = malloc(sz+1); if(!json){fclose(f);return;}
    fread(json,1,sz,f); json[sz]=0; fclose(f);

    json_get_str(json,"terminal",cfg_terminal,sizeof(cfg_terminal));
    json_get_str(json,"launcher",cfg_launcher,sizeof(cfg_launcher));

    /* parse rules array  — look for {"class":... "x":... ...} objects */
    nr = 0;
    const char *p = json;
    while(nr < MAX_RULES && (p = strstr(p,"\"class\"")) != NULL) {
        WinRule *rule = &rules[nr];
        const char *blk = p;
        /* scan back to find opening brace */
        const char *ob = p;
        while(ob > json && *ob != '{') ob--;

        /* extract class */
        if(!json_get_str(p,"class",rule->wm_class,sizeof(rule->wm_class))){p++;continue;}
        /* extract geometry and desk */
        rule->x    = json_get_int(blk,"x",    -1);
        rule->y    = json_get_int(blk,"y",    -1);
        rule->w    = json_get_int(blk,"w",    -1);
        rule->h    = json_get_int(blk,"h",    -1);
        rule->desk = json_get_int(blk,"desk",  0);
        rule->valid = 1;
        nr++;
        p++;
    }
    free(json);
}

static void save_config(void) {
    char dir[512]; config_dir(dir,sizeof(dir));
    mkdir(dir,0755);
    char path[512]; config_path(path,sizeof(path));
    FILE *f = fopen(path,"w"); if(!f) return;
    fprintf(f,"{\n  \"terminal\": \"%s\",\n  \"launcher\": \"%s\",\n",
            cfg_terminal, cfg_launcher);
    fprintf(f,"  \"rules\": [\n");
    for(int i=0;i<nr;i++) {
        WinRule *r = &rules[i]; if(!r->valid) continue;
        fprintf(f,"    {\"class\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"desk\":%d}%s\n",
                r->wm_class, r->x, r->y, r->w, r->h, r->desk,
                (i<nr-1)?",":"");
    }
    fprintf(f,"  ]\n}\n");
    fclose(f);
}

static void apply_rule(Client *c) {
    for(int i=0;i<nr;i++) {
        if(!rules[i].valid) continue;
        if(strcmp(rules[i].wm_class, c->wm_class)==0) {
            WinRule *r = &rules[i];
            if(r->x>=0) c->x=r->x;
            if(r->y>=0) c->y=r->y;
            if(r->w>0)  c->w=r->w;
            if(r->h>0)  c->h=r->h;
            c->desk = r->desk;
            XMoveResizeWindow(dpy,c->frame,c->x,c->y,FW(c),FH(c));
            XResizeWindow(dpy,c->client,c->w,c->h);
            break;
        }
    }
}

static void save_rule_for(Client *c) {
    /* find existing rule or create new */
    WinRule *r = NULL;
    for(int i=0;i<nr;i++) if(strcmp(rules[i].wm_class,c->wm_class)==0){r=&rules[i];break;}
    if(!r && nr<MAX_RULES) { r=&rules[nr++]; }
    if(!r) return;
    strncpy(r->wm_class,c->wm_class,sizeof(r->wm_class)-1);
    r->x = c->x; r->y = c->y; r->w = c->w; r->h = c->h;
    r->desk  = c->desk;
    r->valid = 1;
    save_config();
}

/* ══════════════════════════════════════════
   GRADIENT FILL
══════════════════════════════════════════ */
static void fill_grad(Drawable d, int x, int y, int w, int h,
                      const char *top, const char *bot) {
    if(w<=0||h<=0) return;
    int r1,g1,b1,r2,g2,b2;
    hex2rgb(top,&r1,&g1,&b1); hex2rgb(bot,&r2,&g2,&b2);
    for(int i=0;i<h;i++) {
        float t=(h>1)?(float)i/(h-1):0.f;
        XSetForeground(dpy,gc,rgb_px(r1+(int)((r2-r1)*t),g1+(int)((g2-g1)*t),b1+(int)((b2-b1)*t)));
        XDrawLine(dpy,d,gc,x,y+i,x+w-1,y+i);
    }
}

/* ══════════════════════════════════════════
   XSHAPE ROUNDED CORNERS
══════════════════════════════════════════ */
static void apply_rounded_shape(Window w, int width, int height) {
    Pixmap mask = XCreatePixmap(dpy,root,width,height,1);
    GC gm = XCreateGC(dpy,mask,0,NULL);
    int r = CORNER_R, d = r*2;

    XSetForeground(dpy,gm,0);
    XFillRectangle(dpy,mask,gm,0,0,width,height);
    XSetForeground(dpy,gm,1);

    /* four corner arcs */
    XFillArc(dpy,mask,gm, 0,       0,        d,d, 90*64, 90*64);
    XFillArc(dpy,mask,gm, width-d, 0,        d,d, 0*64,  90*64);
    XFillArc(dpy,mask,gm, 0,       height-d, d,d, 180*64,90*64);
    XFillArc(dpy,mask,gm, width-d, height-d, d,d, 270*64,90*64);
    /* three fill rects to cover interior */
    XFillRectangle(dpy,mask,gm, r,      0,      width-2*r, height);
    XFillRectangle(dpy,mask,gm, 0,      r,      r,         height-2*r);
    XFillRectangle(dpy,mask,gm, width-r,r,      r,         height-2*r);

    XShapeCombineMask(dpy,w,ShapeBounding,0,0,mask,ShapeSet);
    XFreePixmap(dpy,mask);
    XFreeGC(dpy,gm);
}

/* ══════════════════════════════════════════
   EWMH
══════════════════════════════════════════ */
static void ewmh_update_client_list(void) {
    Window wins[MAX_CLIENTS]; int cnt=0;
    for(int i=0;i<nc;i++) if(!clients[i].is_panel) wins[cnt++]=clients[i].client;
    XChangeProperty(dpy,root,NET_CLIENT_LIST,XA_WINDOW,32,PropModeReplace,(unsigned char*)wins,cnt);
}
static void ewmh_set_active(Window w) {
    XChangeProperty(dpy,root,NET_ACTIVE_WIN,XA_WINDOW,32,PropModeReplace,(unsigned char*)&w,1);
}
static void ewmh_set_desktop(Client *c) {
    long d = c->desk;
    XChangeProperty(dpy,c->client,NET_WM_DESKTOP,XA_CARDINAL,32,PropModeReplace,(unsigned char*)&d,1);
}
static void ewmh_update_current_desk(void) {
    long d = current_desk;
    XChangeProperty(dpy,root,NET_CURRENT_DESKTOP,XA_CARDINAL,32,PropModeReplace,(unsigned char*)&d,1);
}
static void ewmh_publish_wm_state(Client *c) {
    Atom state[8]; int n=0;
    if(c->maximized==1){state[n++]=NET_WM_STATE_MAX_H;state[n++]=NET_WM_STATE_MAX_V;}
    if(c->minimized)    state[n++]=NET_WM_STATE_HIDDEN;
    if(c->fullscreen)   state[n++]=NET_WM_STATE_FULLSCREEN;
    if(c->above)        state[n++]=NET_WM_STATE_ABOVE;
    if(c->below)        state[n++]=NET_WM_STATE_BELOW;
    XChangeProperty(dpy,c->client,NET_WM_STATE,XA_ATOM,32,PropModeReplace,(unsigned char*)state,n);
}

/* ══════════════════════════════════════════
   COMPOSITOR
══════════════════════════════════════════ */
static Picture __attribute__((unused)) comp_solid(int r, int g, int b, int a) {
    XRenderColor c={(unsigned short)(r*257),(unsigned short)(g*257),
                    (unsigned short)(b*257),(unsigned short)(a*257)};
    return XRenderCreateSolidFill(dpy,&c);
}

static void comp_init(void) {
    int ev,er,de;
    if(!XCompositeQueryExtension(dpy,&ev,&er)||
       !XDamageQueryExtension(dpy,&dmg_base,&de)) {
        fputs("shadowwm: no Composite/Damage — running without compositor\n",stderr);
        return;
    }
    XCompositeRedirectSubwindows(dpy,root,CompositeRedirectManual);
    vis_fmt  = XRenderFindVisualFormat(dpy,DefaultVisual(dpy,screen));
    buf_px   = XCreatePixmap(dpy,root,scr_w,scr_h,DefaultDepth(dpy,screen));
    buf_pic  = XRenderCreatePicture(dpy,buf_px,vis_fmt,0,NULL);
    root_pic = XRenderCreatePicture(dpy,root,  vis_fmt,0,NULL);
    comp_ok  = 1;
    fputs("shadowwm v5: compositor active\n",stderr);
}

static void comp_get_pixmap(Client *c) {
    if(!comp_ok) return;
    if(c->cpic){XRenderFreePicture(dpy,c->cpic);c->cpic=None;}
    if(c->cpx) {XFreePixmap(dpy,c->cpx);        c->cpx=None;}
    Window src = c->is_panel ? c->client : c->frame;
    c->cpx  = XCompositeNameWindowPixmap(dpy,src);
    if(!c->cpx) return;
    c->cpic = XRenderCreatePicture(dpy,c->cpx,vis_fmt,0,NULL);
    c->dirty = 0;
}

static void comp_add(Client *c) {
    if(!comp_ok) return;
    c->cpx=None; c->cpic=None; c->dirty=1;
    Window src = c->is_panel ? c->client : c->frame;
    c->cdmg = XDamageCreate(dpy,src,XDamageReportNonEmpty);
}
static void comp_remove(Client *c) {
    if(!comp_ok) return;
    if(c->cdmg){XDamageDestroy(dpy,c->cdmg);c->cdmg=0;}
    if(c->cpic){XRenderFreePicture(dpy,c->cpic);c->cpic=None;}
    if(c->cpx) {XFreePixmap(dpy,c->cpx);        c->cpx=None;}
}

/* Gaussian-approximation shadow: 8 layered semi-transparent rectangles */
static void draw_soft_shadow(int x, int y, int w, int h) {
    for(int i=SHADOW_LAYERS; i>=1; i--) {
        int expand = i * SHADOW_EXPAND;
        unsigned short a = (unsigned short)(65535.0 * 0.055 * (SHADOW_LAYERS-i+1) / SHADOW_LAYERS);
        XRenderColor rc = {0,0,0,a};
        XRenderFillRectangle(dpy,PictOpOver,buf_pic,&rc,
            x - expand + SHADOW_DX,
            y - expand + SHADOW_DY,
            w + 2*expand, h + 2*expand);
    }
}

static void comp_repaint(void) {
    if(!comp_ok || !buf_pic || !root_pic) return;

    /* Background fill */
    static XRenderColor bg = {0x0a00,0x0a00,0x0a00,0xffff};
    XRenderFillRectangle(dpy,PictOpSrc,buf_pic,&bg,0,0,scr_w,scr_h);

    /* Pass 1: desktop windows */
    for(int i=0;i<nc;i++) {
        Client *c=&clients[i];
        if(c->is_panel!=1) continue;
        if(c->dirty) comp_get_pixmap(c);
        if(!c->cpic) continue;
        XRenderComposite(dpy,PictOpOver,c->cpic,None,buf_pic,
                         0,0,0,0,c->x,c->y,c->w,c->h);
    }

    /* Pass 2: normal windows, below-tagged first */
    for(int pass=0; pass<3; pass++) {
        for(int i=0;i<nc;i++) {
            Client *c=&clients[i];
            if(c->minimized || c->is_panel) continue;
            /* workspace visibility */
            if(c->desk >= 0 && c->desk != current_desk && !c->pip) continue;
            int is_below  = (c->below && !c->focused);
            int is_above  = (c->above || c->pip);
            int is_normal = (!is_below && !is_above);
            if(pass==0 && !is_below)  continue;
            if(pass==1 && !is_normal) continue;
            if(pass==2 && !is_above)  continue;

            if(c->dirty) comp_get_pixmap(c);
            if(!c->cpic) continue;

            /* soft shadow for focused normal windows */
            if(!c->is_panel && !c->fullscreen && (c->focused || pass==2))
                draw_soft_shadow(c->x, c->y, FW(c), FH(c));

            /* per-window opacity via mask picture */
            unsigned short oa = (unsigned short)(c->opacity * 65535.0f);
            XRenderColor ac = {oa,oa,oa,oa};
            Picture alpha_mask = XRenderCreateSolidFill(dpy,&ac);

            int fw = c->is_panel ? c->w : FW(c);
            int fh = c->is_panel ? c->h : FH(c);
            XRenderComposite(dpy,PictOpOver,c->cpic,alpha_mask,buf_pic,
                             0,0,0,0,c->x,c->y,fw,fh);
            if(alpha_mask) XRenderFreePicture(dpy,alpha_mask);
        }
    }

    /* Pass 3: dock windows — top layer */
    for(int i=0;i<nc;i++) {
        Client *c=&clients[i];
        if(c->is_panel!=2) continue;
        if(c->dirty) comp_get_pixmap(c);
        if(!c->cpic) continue;
        XRenderComposite(dpy,PictOpOver,c->cpic,None,buf_pic,
                         0,0,0,0,c->x,c->y,c->w,c->h);
    }

    XRenderComposite(dpy,PictOpSrc,buf_pic,None,root_pic,0,0,0,0,0,0,scr_w,scr_h);
    XFlush(dpy);
}

/* ══════════════════════════════════════════
   FRAME DRAWING  (macOS style)
══════════════════════════════════════════ */
static void update_title(Client *c) {
    char tmp[256]=""; unsigned char *data=NULL;
    Atom actual; int fmt; unsigned long n,extra;
    if(XGetWindowProperty(dpy,c->client,NET_WM_NAME,0,255,False,UTF8_STRING,
                          &actual,&fmt,&n,&extra,&data)==Success && data && n>0) {
        snprintf(tmp,sizeof(tmp),"%.*s",(int)n,(char*)data);
    } else {
        if(XFetchName(dpy,c->client,(char**)&data) && data)
            snprintf(tmp,sizeof(tmp),"%s",(char*)data);
    }
    if(data) XFree(data);
    snprintf(c->title, sizeof(c->title), "%s", tmp);
}

static void draw_macos_btn(Drawable d, int cx, int cy, int r,
                           const char *col, const char *hcol,
                           int hovered, const char *sym) {
    int bx = cx-r, by = cy-r, bd = r*2;
    /* fill circle */
    XSetForeground(dpy,gc,xcolor(hovered ? hcol : col));
    XFillArc(dpy,d,gc, bx,by,bd,bd, 0,360*64);
    /* subtle border */
    XSetForeground(dpy,gc,xcolor("#00000040"));
    XDrawArc(dpy,d,gc, bx,by,bd,bd, 0,360*64);
    /* symbol on hover */
    if(hovered && sym && xfont) {
        XftDraw *xd = XftDrawCreate(dpy,d,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen));
        XftColor fc; XRenderColor rc={0,0,0,0x9000};
        XftColorAllocValue(dpy,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen),&rc,&fc);
        XftDrawStringUtf8(xd,&fc,xfont,cx-3,cy+4,(const FcChar8*)sym,1);
        XftColorFree(dpy,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen),&fc);
        XftDrawDestroy(xd);
    }
}

static void draw_tab_strip(Client *c, Drawable d) {
    if(c->tab_group < 0 || c->tab_group >= ntg) return;
    TabGroup *tg = &tab_groups[c->tab_group];
    if(tg->count < 2) return;

    int y0 = TITLEBAR_H;
    int tab_w = FW(c) / tg->count;

    for(int i=0;i<tg->count;i++) {
        int xi = RB + i*tab_w;
        int is_active = (i == tg->active);
        /* tab background */
        XSetForeground(dpy,gc,xcolor(is_active ? C_TAB_ACT : C_TAB_INACT));
        XFillRectangle(dpy,d,gc, xi, y0, tab_w, TAB_H);
        /* tab border */
        XSetForeground(dpy,gc,xcolor(C_SEP_FOC));
        XDrawLine(dpy,d,gc, xi+tab_w-1, y0, xi+tab_w-1, y0+TAB_H-1);
        /* tab title */
        if(tg->members[i] >= 0 && tg->members[i] < nc) {
            Client *tc = &clients[tg->members[i]];
            if(xfont) {
                XftDraw *xd = XftDrawCreate(dpy,d,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen));
                XftColor fc; XRenderColor rc;
                rc.red=0xb000;rc.green=0xb000;rc.blue=0xb000;rc.alpha=0xffff;
                XftColorAllocValue(dpy,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen),&rc,&fc);
                /* clip text to tab width */
                XRectangle clip={xi+4,y0+2,tab_w-8,TAB_H-4};
                XftDrawSetClipRectangles(xd,0,0,&clip,1);
                XftDrawStringUtf8(xd,&fc,xfont,xi+6,y0+TAB_H-6,
                                  (const FcChar8*)tc->title,strlen(tc->title));
                XftColorFree(dpy,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen),&fc);
                XftDrawDestroy(xd);
            }
        }
    }
    /* separator line */
    XSetForeground(dpy,gc,xcolor(C_SEP_FOC));
    XDrawLine(dpy,d,gc, 0, y0+TAB_H-1, FW(c), y0+TAB_H-1);
}

static void draw_frame(Client *c) {
    if(!c || c->is_panel) return;
    int fw = FW(c), fh = FH(c);
    int focused = c->focused;

    /* gradient titlebar */
    fill_grad(c->frame, 0, 0, fw, TITLEBAR_H,
              focused ? C_BAR_FOC_T : C_BAR_UNF_T,
              focused ? C_BAR_FOC_B : C_BAR_UNF_B);

    /* fill rest of frame (resize area + tab strip) */
    XSetForeground(dpy,gc,xcolor(focused ? C_BDR_FOC : C_BDR_UNF));
    XFillRectangle(dpy,c->frame,gc, 0, TITLEBAR_H, fw, fh-TITLEBAR_H);

    /* separator below titlebar */
    XSetForeground(dpy,gc,xcolor(focused ? C_SEP_FOC : C_SEP_UNF));
    XDrawLine(dpy,c->frame,gc, 0, TITLEBAR_H-1, fw, TITLEBAR_H-1);

    /* border */
    XSetForeground(dpy,gc,xcolor(focused ? C_BDR_FOC : C_BDR_UNF));
    XDrawRectangle(dpy,c->frame,gc, 0, 0, fw-1, fh-1);

    /* re-apply rounded shape */
    apply_rounded_shape(c->frame, fw, fh);

    /* macOS traffic-light buttons */
    if(!c->fullscreen) {
        int r = BTN_D/2;
        int by = TITLEBAR_H/2;
        int bx1 = BTN_PAD_L + r;
        int bx2 = bx1 + BTN_D + BTN_GAP;
        int bx3 = bx2 + BTN_D + BTN_GAP;
        int hb = (hover_frame==c->frame) ? hover_btn : 0;
        draw_macos_btn(c->frame, bx1, by, r, C_CLOSE, C_CLOSE_H, hb==1, "\xc3\x97");
        draw_macos_btn(c->frame, bx2, by, r, C_MIN,   C_MIN_H,   hb==2, "\xe2\x88\x92");
        draw_macos_btn(c->frame, bx3, by, r, C_MAX,   C_MAX_H,   hb==3, "+");
    }

    /* centered title */
    if(c->title[0] && xfont && !c->fullscreen) {
        XGlyphInfo ext;
        XftTextExtentsUtf8(dpy,xfont,(const FcChar8*)c->title,strlen(c->title),&ext);
        int tx = (fw - ext.width) / 2;
        int ty = (TITLEBAR_H + xfont->ascent - xfont->descent) / 2;
        if(tx < BTN_PAD_L + 3*(BTN_D+BTN_GAP)+4) tx = BTN_PAD_L + 3*(BTN_D+BTN_GAP)+4;
        XftDraw *xd = XftDrawCreate(dpy,c->frame,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen));
        XftColor fc; XRenderColor rc;
        if(focused){ rc.red=0xe200;rc.green=0xe200;rc.blue=0xe200;rc.alpha=0xffff; }
        else        { rc.red=0x3000;rc.green=0x3000;rc.blue=0x3000;rc.alpha=0xffff; }
        XftColorAllocValue(dpy,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen),&rc,&fc);
        XftDrawStringUtf8(xd,&fc,xfont,tx,ty,(const FcChar8*)c->title,strlen(c->title));
        XftColorFree(dpy,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen),&fc);
        XftDrawDestroy(xd);
    }

    /* tab strip if in group */
    if(c->tab_group >= 0) draw_tab_strip(c, c->frame);

    if(comp_ok) { c->dirty=1; comp_repaint(); }
}

/* ══════════════════════════════════════════
   FOCUS
══════════════════════════════════════════ */
static void focus_client(Client *c) {
    /* unfocus all */
    for(int i=0;i<nc;i++) {
        if(clients[i].focused && &clients[i]!=c) {
            clients[i].focused=0;
            /* dim unfocused via opacity */
            clients[i].opacity = DIM_OPACITY;
            draw_frame(&clients[i]);
        }
    }
    if(!c) { ewmh_set_active(None); return; }
    c->focused  = 1;
    c->opacity  = 1.0f;
    XRaiseWindow(dpy,c->frame);
    XSetInputFocus(dpy,c->client,RevertToPointerRoot,CurrentTime);
    ewmh_set_active(c->client);
    draw_frame(c);
    if(comp_ok) comp_repaint();
}

/* ══════════════════════════════════════════
   WORKSPACES
══════════════════════════════════════════ */
static void switch_desk(int d) {
    if(d<0||d>=NUM_DESKS||d==current_desk) return;
    /* hide current desk windows */
    for(int i=0;i<nc;i++) {
        Client *c=&clients[i];
        if(c->is_panel||c->pip||c->desk<0) continue;
        if(c->desk==current_desk && !c->minimized)
            XUnmapWindow(dpy,c->frame);
    }
    current_desk = d;
    ewmh_update_current_desk();
    /* show new desk windows */
    for(int i=0;i<nc;i++) {
        Client *c=&clients[i];
        if(c->is_panel||c->pip||c->desk<0) continue;
        if(c->desk==current_desk && !c->minimized)
            XMapWindow(dpy,c->frame);
    }
    /* focus first visible */
    for(int i=nc-1;i>=0;i--) {
        Client *c=&clients[i];
        if(!c->is_panel && !c->minimized &&
           (c->desk==current_desk||c->desk<0)) {
            focus_client(c); break;
        }
    }
    comp_repaint();
}

static void move_to_desk(Client *c, int d) {
    if(d<0||d>=NUM_DESKS) return;
    if(c->desk != current_desk) return; /* only move visible windows */
    c->desk = d;
    ewmh_set_desktop(c);
    XUnmapWindow(dpy,c->frame);
    comp_repaint();
}

/* ══════════════════════════════════════════
   TAB GROUPS
══════════════════════════════════════════ */
static int tg_new(void) {
    if(ntg >= (int)(sizeof(tab_groups)/sizeof(tab_groups[0]))) return -1;
    memset(&tab_groups[ntg],0,sizeof(TabGroup));
    return ntg++;
}

/* Add client idx to group tg_idx */
static void tg_add(int tg_idx, int cidx_val) {
    TabGroup *tg = &tab_groups[tg_idx];
    if(tg->count >= MAX_TABS) return;
    tg->members[tg->count++] = cidx_val;
    clients[cidx_val].tab_group = tg_idx;
    clients[cidx_val].chrome_h  = TITLEBAR_H + TAB_H + RB;
}

/* Activate tab i in group tg_idx */
static void tg_activate(int tg_idx, int i) {
    TabGroup *tg = &tab_groups[tg_idx];
    if(i<0||i>=tg->count) return;
    tg->active = i;
    /* hide all, show active */
    for(int j=0;j<tg->count;j++) {
        Client *m = &clients[tg->members[j]];
        if(j==i) {
            XMapWindow(dpy,m->frame);
            focus_client(m);
        } else {
            XUnmapWindow(dpy,m->frame);
        }
    }
    /* reposition all to same geometry as active */
    Client *act = &clients[tg->members[i]];
    for(int j=0;j<tg->count;j++) {
        if(j==i) continue;
        Client *m = &clients[tg->members[j]];
        m->x=act->x; m->y=act->y; m->w=act->w; m->h=act->h;
    }
    draw_frame(act);
}

static void tg_remove(int tg_idx, int cidx_val) {
    TabGroup *tg = &tab_groups[tg_idx];
    int pos=-1;
    for(int i=0;i<tg->count;i++) if(tg->members[i]==cidx_val){pos=i;break;}
    if(pos<0) return;
    clients[cidx_val].tab_group=-1;
    clients[cidx_val].chrome_h=TITLEBAR_H+RB;
    /* remove from array */
    for(int i=pos;i<tg->count-1;i++) tg->members[i]=tg->members[i+1];
    tg->count--;
    if(tg->active >= tg->count) tg->active = tg->count-1;
    if(tg->count > 0) tg_activate(tg_idx, tg->active);
}

static void __attribute__((unused)) tab_redraw_active(int tg_idx) {
    TabGroup *tg = &tab_groups[tg_idx];
    if(tg->active>=0 && tg->active<tg->count) {
        draw_frame(&clients[tg->members[tg->active]]);
    }
}

/* Merge focused window with window at pointer */
static void tab_merge_focused(void) {
    Window fw; int rev; XGetInputFocus(dpy,&fw,&rev);
    Client *src = by_client(fw); if(!src||src->is_panel) return;
    /* find window under pointer */
    Window root_ret,child; int rx,ry,wx,wy; unsigned int mask;
    XQueryPointer(dpy,root,&root_ret,&child,&rx,&ry,&wx,&wy,&mask);
    Client *dst = by_frame(child);
    if(!dst||dst==src||dst->is_panel) return;

    int tg_idx;
    if(dst->tab_group >= 0) {
        tg_idx = dst->tab_group;
    } else {
        tg_idx = tg_new(); if(tg_idx<0) return;
        tg_add(tg_idx, cidx(dst));
    }
    if(src->tab_group != tg_idx)
        tg_add(tg_idx, cidx(src));
    tg_activate(tg_idx, tab_groups[tg_idx].count-1);
}

static void tab_detach_focused(void) {
    Window fw; int rev; XGetInputFocus(dpy,&fw,&rev);
    Client *c = by_client(fw); if(!c||c->is_panel||c->tab_group<0) return;
    tg_remove(c->tab_group, cidx(c));
    c->chrome_h = TITLEBAR_H + RB;
    XMapWindow(dpy,c->frame);
    draw_frame(c);
}

/* ══════════════════════════════════════════
   PICTURE-IN-PICTURE
══════════════════════════════════════════ */
static void pip_enable(Client *c) {
    if(c->pip) return;
    c->pip=1;
    c->pip_sx=c->x; c->pip_sy=c->y; c->pip_sw=c->w; c->pip_sh=c->h;
    c->above=1; c->desk=-1; /* sticky */
    /* position bottom-right corner */
    c->x = work_x + work_w - PIP_W - 20;
    c->y = work_y + work_h - PIP_H - 20;
    c->w = PIP_W; c->h = PIP_H;
    XMoveResizeWindow(dpy,c->frame,c->x,c->y,FW(c),FH(c));
    XResizeWindow(dpy,c->client,c->w,c->h);
    XRaiseWindow(dpy,c->frame);
    XMapWindow(dpy,c->frame);
    draw_frame(c);
    ewmh_publish_wm_state(c);
    comp_repaint();
}
static void pip_disable(Client *c) {
    if(!c->pip) return;
    c->pip=0; c->above=0; c->desk=current_desk;
    c->x=c->pip_sx; c->y=c->pip_sy; c->w=c->pip_sw; c->h=c->pip_sh;
    XMoveResizeWindow(dpy,c->frame,c->x,c->y,FW(c),FH(c));
    XResizeWindow(dpy,c->client,c->w,c->h);
    draw_frame(c);
    ewmh_publish_wm_state(c);
    comp_repaint();
}

/* ══════════════════════════════════════════
   PER-WINDOW OPACITY
══════════════════════════════════════════ */
static void set_client_opacity(Client *c, float op) {
    c->opacity = op;
    if(comp_ok) comp_repaint();
}

/* ══════════════════════════════════════════
   TILING  (6 zones + ghost preview)
══════════════════════════════════════════ */
#define ZONE_NONE    0
#define ZONE_LEFT    1
#define ZONE_RIGHT   2
#define ZONE_TOP     3
#define ZONE_TL      4
#define ZONE_TR      5
#define ZONE_BL      6
#define ZONE_BR      7

static void ghost_show(int zone) {
    if(zone == ghost_zone) return;
    ghost_zone = zone;
    if(zone == ZONE_NONE) {
        if(ghost_win) { XUnmapWindow(dpy,ghost_win); }
        return;
    }
    int gx,gy,gw,gh;
    int hw=work_w/2, hh=work_h/2;
    switch(zone) {
        case ZONE_LEFT:  gx=work_x;       gy=work_y;       gw=hw;  gh=work_h; break;
        case ZONE_RIGHT: gx=work_x+hw;    gy=work_y;       gw=hw;  gh=work_h; break;
        case ZONE_TOP:   gx=work_x;       gy=work_y;       gw=work_w;gh=hh;   break;
        case ZONE_TL:    gx=work_x;       gy=work_y;       gw=hw;  gh=hh;     break;
        case ZONE_TR:    gx=work_x+hw;    gy=work_y;       gw=hw;  gh=hh;     break;
        case ZONE_BL:    gx=work_x;       gy=work_y+hh;    gw=hw;  gh=hh;     break;
        case ZONE_BR:    gx=work_x+hw;    gy=work_y+hh;    gw=hw;  gh=hh;     break;
        default: return;
    }
    if(!ghost_win) {
        XSetWindowAttributes swa;
        swa.override_redirect=True;
        swa.background_pixel=xcolor(C_ACCENT);
        ghost_win = XCreateWindow(dpy,root,gx,gy,gw,gh,0,
                                  CopyFromParent,InputOutput,CopyFromParent,
                                  CWOverrideRedirect|CWBackPixel,&swa);
    }
    XMoveResizeWindow(dpy,ghost_win,gx,gy,gw,gh);
    /* semi-transparent via XRender */
    XMapRaised(dpy,ghost_win);
    if(comp_ok && vis_fmt) {
        Picture gp = XRenderCreatePicture(dpy,ghost_win,vis_fmt,0,NULL);
        XRenderColor gc2 = {0x5e00,0x8100,0xf400,0x4000};
        XRenderFillRectangle(dpy,PictOpSrc,gp,&gc2,0,0,gw,gh);
        XRenderFreePicture(dpy,gp);
    }
}

static int detect_tile_zone(int mx, int my) {
    int ex=work_x+work_w, ey=work_y+work_h;
    int inT = (my <= work_y+TILE_ZONE);
    int inB = (my >= ey-TILE_ZONE);
    int inL = (mx <= work_x+TILE_ZONE);
    int inR = (mx >= ex-TILE_ZONE);
    if(inT && inL) return ZONE_TL;
    if(inT && inR) return ZONE_TR;
    if(inB && inL) return ZONE_BL;
    if(inB && inR) return ZONE_BR;
    if(inL) return ZONE_LEFT;
    if(inR) return ZONE_RIGHT;
    if(inT) return ZONE_TOP;
    return ZONE_NONE;
}

static void tile_to_zone(Client *c, int zone) {
    if(!c) return;
    int hw=work_w/2, hh=work_h/2;
    int ch = c->chrome_h;   /* total chrome height (titlebar + RB) */
    /* save geometry */
    if(!c->maximized && !c->fullscreen) {
        c->sx=c->x;c->sy=c->y;c->sw=c->w;c->sh=c->h;
    }
    int nx,ny,nw,nh;
    switch(zone) {
        case ZONE_LEFT:  nx=work_x;       ny=work_y;       nw=hw-2*RB;        nh=work_h-ch;  break;
        case ZONE_RIGHT: nx=work_x+hw;    ny=work_y;       nw=work_w-hw-2*RB; nh=work_h-ch;  break;
        case ZONE_TOP:   nx=work_x;       ny=work_y;       nw=work_w-2*RB;    nh=work_h-ch; c->maximized=1; goto do_max;
        case ZONE_TL:    nx=work_x;       ny=work_y;       nw=hw-2*RB;        nh=hh-ch;      break;
        case ZONE_TR:    nx=work_x+hw;    ny=work_y;       nw=work_w-hw-2*RB; nh=hh-ch;      break;
        case ZONE_BL:    nx=work_x;       ny=work_y+hh;    nw=hw-2*RB;        nh=work_h-hh-ch; break;
        case ZONE_BR:    nx=work_x+hw;    ny=work_y+hh;    nw=work_w-hw-2*RB; nh=work_h-hh-ch; break;
        default: return;
    }
    c->x=nx; c->y=ny; c->w=nw; c->h=nh;
    c->maximized=1;
    XMoveResizeWindow(dpy,c->frame,c->x,c->y,FW(c),FH(c));
    XResizeWindow(dpy,c->client,c->w,c->h);
    if(comp_ok){c->dirty=1;comp_repaint();}
    draw_frame(c);
    return;
do_max:
    c->x=nx; c->y=ny; c->w=nw; c->h=nh;
    XMoveResizeWindow(dpy,c->frame,c->x,c->y,FW(c),FH(c));
    XResizeWindow(dpy,c->client,c->w,c->h);
    if(comp_ok){c->dirty=1;comp_repaint();}
    draw_frame(c);
}

/* ══════════════════════════════════════════
   THUMBNAIL SWITCHER (Exposé-style)
══════════════════════════════════════════ */
#define THUMB_COLS   4
#define THUMB_PAD   20
#define THUMB_GAP   16

static void switcher_draw(void) {
    if(!switcher_win) return;
    /* dark background */
    XSetForeground(dpy,gc,xcolor("#0a0a0a"));
    XFillRectangle(dpy,switcher_win,gc,0,0,scr_w,scr_h);

    /* collect visible windows */
    int vis[MAX_CLIENTS], nvis=0;
    for(int i=0;i<nc;i++) {
        Client *c=&clients[i];
        if(c->minimized||c->is_panel) continue;
        if(c->desk>=0 && c->desk!=current_desk && !c->pip) continue;
        vis[nvis++]=i;
    }
    if(!nvis) return;

    int cols = nvis < THUMB_COLS ? nvis : THUMB_COLS;
    int rows = (nvis + cols - 1) / cols;
    int cell_w = (scr_w - 2*THUMB_PAD - (cols-1)*THUMB_GAP) / cols;
    int cell_h = (scr_h - 2*THUMB_PAD - (rows-1)*THUMB_GAP) / rows;

    for(int i=0;i<nvis;i++) {
        Client *c=&clients[vis[i]];
        int col = i % cols, row = i / cols;
        int tx = THUMB_PAD + col*(cell_w+THUMB_GAP);
        int ty = THUMB_PAD + row*(cell_h+THUMB_GAP);

        /* selection highlight */
        if(i==switcher_sel) {
            XSetForeground(dpy,gc,xcolor(C_ACCENT));
            XFillRectangle(dpy,switcher_win,gc,tx-4,ty-4,cell_w+8,cell_h+8+20);
        }

        /* thumbnail via XRender scale */
        if(comp_ok && c->cpic) {
            int src_w = FW(c), src_h = FH(c);
            if(src_w>0 && src_h>0) {
                Picture dst = XRenderCreatePicture(dpy,switcher_win,vis_fmt,0,NULL);
                double sx=(double)src_w/cell_w, sy=(double)src_h/cell_h;
                XTransform xfm;
                memset(&xfm,0,sizeof(xfm));
                xfm.matrix[0][0]=XDoubleToFixed(sx);
                xfm.matrix[1][1]=XDoubleToFixed(sy);
                xfm.matrix[2][2]=XDoubleToFixed(1.0);
                XRenderSetPictureTransform(dpy,c->cpic,&xfm);
                XRenderComposite(dpy,PictOpOver,c->cpic,None,dst,
                                 0,0,0,0,tx,ty,cell_w,cell_h);
                /* reset transform */
                XTransform ident;
                memset(&ident,0,sizeof(ident));
                ident.matrix[0][0]=ident.matrix[1][1]=ident.matrix[2][2]=XDoubleToFixed(1.0);
                XRenderSetPictureTransform(dpy,c->cpic,&ident);
                XRenderFreePicture(dpy,dst);
            }
        } else {
            /* fallback: grey rectangle */
            XSetForeground(dpy,gc,xcolor(i==switcher_sel?"#2a2a3a":"#1a1a1a"));
            XFillRectangle(dpy,switcher_win,gc,tx,ty,cell_w,cell_h);
        }

        /* title below thumbnail */
        if(xfont) {
            XftDraw *xd=XftDrawCreate(dpy,switcher_win,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen));
            XftColor fc; XRenderColor rc={0xe000,0xe000,0xe000,0xffff};
            XftColorAllocValue(dpy,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen),&rc,&fc);
            XftDrawStringUtf8(xd,&fc,xfont,tx,ty+cell_h+14,(const FcChar8*)c->title,strlen(c->title));
            XftColorFree(dpy,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen),&fc);
            XftDrawDestroy(xd);
        }
    }
    XFlush(dpy);
}

static void switcher_open_fn(void) {
    if(switcher_open) return;
    switcher_open=1; switcher_sel=0;
    if(!switcher_win) {
        XSetWindowAttributes swa;
        swa.override_redirect=True;
        swa.background_pixel=xcolor("#050505");
        swa.event_mask=KeyPressMask|ButtonPressMask;
        switcher_win=XCreateWindow(dpy,root,0,0,scr_w,scr_h,0,
                                   CopyFromParent,InputOutput,CopyFromParent,
                                   CWOverrideRedirect|CWBackPixel|CWEventMask,&swa);
    }
    XMapRaised(dpy,switcher_win);
    XGrabKeyboard(dpy,switcher_win,False,GrabModeAsync,GrabModeAsync,CurrentTime);
    XGrabPointer(dpy,switcher_win,False,ButtonPressMask,GrabModeAsync,GrabModeAsync,None,None,CurrentTime);
    switcher_draw();
}

static void switcher_close_fn(void) {
    if(!switcher_open) return;
    switcher_open=0;
    XUngrabKeyboard(dpy,CurrentTime);
    XUngrabPointer(dpy,CurrentTime);
    XUnmapWindow(dpy,switcher_win);
    comp_repaint();
}

static void switcher_select(void) {
    int vis[MAX_CLIENTS], nvis=0;
    for(int i=0;i<nc;i++) {
        Client *c=&clients[i];
        if(c->minimized||c->is_panel) continue;
        if(c->desk>=0&&c->desk!=current_desk&&!c->pip) continue;
        vis[nvis++]=i;
    }
    switcher_close_fn();
    if(switcher_sel >= 0 && switcher_sel < nvis)
        focus_client(&clients[vis[switcher_sel]]);
}

/* ══════════════════════════════════════════
   RIGHT-CLICK TITLEBAR MENU
══════════════════════════════════════════ */
static void titlebar_menu(Client *c, XButtonEvent *be) {
    Window mw;
    XSetWindowAttributes swa;
    int mw_w=220;
    struct { const char *label; int action; } items[] = {
        {"Always on Top",      1},
        {"Always on Bottom",   2},
        {"─────────────",      0},
        {"Opacity 100%",      10},
        {"Opacity 75%",       11},
        {"Opacity 50%",       12},
        {"Opacity 25%",       13},
        {"─────────────",      0},
        {"Save Window Rule",  20},
    };
    int nit = sizeof(items)/sizeof(items[0]);
    int item_h = 22;
    int total_h = nit * item_h + 8;

    swa.override_redirect=True;
    swa.background_pixel=xcolor("#111111");
    swa.event_mask=ExposureMask|ButtonPressMask|LeaveWindowMask;
    mw=XCreateWindow(dpy,root,be->x_root,be->y_root,mw_w,total_h,1,
                     CopyFromParent,InputOutput,CopyFromParent,
                     CWOverrideRedirect|CWBackPixel|CWEventMask,&swa);
    XMapRaised(dpy,mw);
    XGrabPointer(dpy,mw,False,ButtonPressMask|LeaveWindowMask,
                 GrabModeAsync,GrabModeAsync,None,None,CurrentTime);

    /* draw menu */
    XSetForeground(dpy,gc,xcolor("#2a2a2a"));
    XDrawRectangle(dpy,mw,gc,0,0,mw_w-1,total_h-1);
    for(int i=0;i<nit;i++) {
        int iy = 4 + i*item_h;
        if(items[i].action==0) {
            XSetForeground(dpy,gc,xcolor("#222222"));
            XDrawLine(dpy,mw,gc,4,iy+item_h/2,mw_w-4,iy+item_h/2);
        } else if(xfont) {
            XftDraw *xd=XftDrawCreate(dpy,mw,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen));
            XftColor fc; XRenderColor rc={0xc000,0xc000,0xc000,0xffff};
            XftColorAllocValue(dpy,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen),&rc,&fc);
            XftDrawStringUtf8(xd,&fc,xfont,12,iy+15,(const FcChar8*)items[i].label,strlen(items[i].label));
            XftColorFree(dpy,DefaultVisual(dpy,screen),DefaultColormap(dpy,screen),&fc);
            XftDrawDestroy(xd);
        }
    }
    XFlush(dpy);

    /* event loop for menu */
    XEvent ev;
    for(;;) {
        XNextEvent(dpy,&ev);
        if(ev.type==LeaveNotify && ev.xcrossing.window==mw) break;
        if(ev.type==ButtonPress) {
            if(ev.xbutton.window==mw) {
                int iy = ev.xbutton.y - 4;
                int idx = iy / item_h;
                if(idx>=0 && idx<nit && items[idx].action!=0) {
                    switch(items[idx].action) {
                        case 1:  c->above=!c->above; c->below=0; break;
                        case 2:  c->below=!c->below; c->above=0; break;
                        case 10: set_client_opacity(c,1.00f); break;
                        case 11: set_client_opacity(c,0.75f); break;
                        case 12: set_client_opacity(c,0.50f); break;
                        case 13: set_client_opacity(c,0.25f); break;
                        case 20: save_rule_for(c);            break;
                    }
                    ewmh_publish_wm_state(c);
                    draw_frame(c);
                }
            }
            break;
        }
    }
    XUngrabPointer(dpy,CurrentTime);
    XDestroyWindow(dpy,mw);
    comp_repaint();
}

/* ══════════════════════════════════════════
   WINDOW LIFECYCLE
══════════════════════════════════════════ */
static void get_wm_class(Window w, char *out, int maxlen) {
    out[0]=0;
    XClassHint ch; if(XGetClassHint(dpy,w,&ch)) {
        if(ch.res_class) strncpy(out,ch.res_class,maxlen-1);
        if(ch.res_name)  XFree(ch.res_name);
        if(ch.res_class) XFree(ch.res_class);
    }
}

static Atom get_window_type(Window w) {
    Atom actual,result=None; int fmt; unsigned long n,extra; unsigned char *data=NULL;
    if(XGetWindowProperty(dpy,w,NET_WM_TYPE,0,1,False,XA_ATOM,
                          &actual,&fmt,&n,&extra,&data)==Success&&data&&n>0)
        result=*(Atom*)data;
    if(data) XFree(data);
    return result;
}
static int classify_window(Window w) {
    Atom t=get_window_type(w);
    if(t==NET_WM_TYPE_DESKTOP) return 1;
    if(t==NET_WM_TYPE_DOCK)    return 2;
    if(t==NET_WM_TYPE_DIALOG||t==NET_WM_TYPE_SPLASH||t==NET_WM_TYPE_TOOLTIP||
       t==NET_WM_TYPE_NOTIFICATION||t==NET_WM_TYPE_POPUP||t==NET_WM_TYPE_UTILITY) return 3;
    return 0;
}

static void update_work_area(void) {
    work_x=0;work_y=0;work_w=scr_w;work_h=scr_h;
    for(int i=0;i<nc;i++) {
        Client *c=&clients[i]; if(c->is_panel!=2) continue;
        Atom actual; int fmt; unsigned long n,extra; unsigned char *data=NULL;
        if(XGetWindowProperty(dpy,c->client,NET_WM_STRUT_PARTIAL,0,12,False,XA_CARDINAL,
                              &actual,&fmt,&n,&extra,&data)==Success&&data&&n>=4) {
            long *s=(long*)data;
            if(s[0]>work_x) work_x=(int)s[0];
            if(s[1]>0)      work_w=scr_w-(int)s[1]-work_x;
            if(s[2]>work_y) work_y=(int)s[2];
            if(s[3]>0)      work_h=scr_h-(int)s[3]-work_y;
        }
        if(data) XFree(data);
    }
}

static void frame_window(Window w) {
    if(nc >= MAX_CLIENTS) return;
    XWindowAttributes wa;
    if(!XGetWindowAttributes(dpy,w,&wa)) return;
    if(wa.override_redirect) return;
    if(by_client(w)) return;

    Client *c = &clients[nc];
    memset(c,0,sizeof(Client));
    c->client   = w;
    c->opacity  = 1.0f;
    c->desk     = current_desk;
    c->tab_group= -1;
    c->chrome_h = TITLEBAR_H + RB;
    c->is_panel = classify_window(w);

    if(c->is_panel==3) {
        /* unmanaged special window — just map it */
        XMapWindow(dpy,w);
        c->frame = w; c->x=wa.x; c->y=wa.y; c->w=wa.width; c->h=wa.height;
        comp_add(c); nc++; ewmh_update_client_list(); return;
    }
    if(c->is_panel) {
        c->frame=w; c->x=wa.x; c->y=wa.y; c->w=wa.width; c->h=wa.height;
        XMapWindow(dpy,w);
        if(c->is_panel==2) update_work_area();
        comp_add(c); nc++; ewmh_update_client_list(); return;
    }

    /* normal window */
    c->w = wa.width; c->h = wa.height;
    c->x = wa.x; c->y = wa.y;
    if(c->x+FW(c) > work_x+work_w) c->x = work_x+work_w-FW(c);
    if(c->y+FH(c) > work_y+work_h) c->y = work_y;
    if(c->x < work_x) c->x=work_x;
    if(c->y < work_y) c->y=work_y;

    /* get class for rules */
    get_wm_class(w, c->wm_class, sizeof(c->wm_class));

    /* create frame */
    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel  = xcolor(C_BAR_FOC_B);
    swa.event_mask = SubstructureRedirectMask|SubstructureNotifyMask|
                     ExposureMask|ButtonPressMask|ButtonReleaseMask|
                     PointerMotionMask|EnterWindowMask|LeaveWindowMask;
    c->frame = XCreateWindow(dpy,root,c->x,c->y,FW(c),FH(c),0,
                             CopyFromParent,InputOutput,CopyFromParent,
                             CWOverrideRedirect|CWBackPixel|CWEventMask,&swa);

    XReparentWindow(dpy,w,c->frame,RB,c->chrome_h-RB);
    XResizeWindow(dpy,w,c->w,c->h);
    XSelectInput(dpy,w,EnterWindowMask|PropertyChangeMask);

    /* apply shape */
    apply_rounded_shape(c->frame,FW(c),FH(c));

    XMapWindow(dpy,w);
    XMapWindow(dpy,c->frame);

    update_title(c);
    nc++;

    /* apply saved rules */
    apply_rule(c);

    /* set EWMH desktop */
    ewmh_set_desktop(c);

    comp_add(c);
    ewmh_update_client_list();

    /* fade in */
    c->opacity = 0.0f;
    draw_frame(c);
    for(int i=1;i<=FADE_STEPS;i++) {
        c->opacity = (float)i/FADE_STEPS;
        if(comp_ok) comp_repaint();
        XFlush(dpy); usleep(FADE_US);
    }
    c->opacity = 1.0f;
}

static void close_client(Client *c) {
    /* try WM_DELETE_WINDOW first */
    Atom *protos=NULL; int np=0;
    if(XGetWMProtocols(dpy,c->client,&protos,&np)) {
        for(int i=0;i<np;i++) {
            if(protos[i]==WM_DEL) {
                XClientMessageEvent ev={};
                ev.type=ClientMessage; ev.window=c->client;
                ev.message_type=WM_PROTO; ev.format=32;
                ev.data.l[0]=(long)WM_DEL; ev.data.l[1]=CurrentTime;
                XSendEvent(dpy,c->client,False,NoEventMask,(XEvent*)&ev);
                if(protos) XFree(protos);
                return;
            }
        }
        if(protos) XFree(protos);
    }
    XKillClient(dpy,c->client);
}

static void minimize_client(Client *c) {
    if(c->minimized) return;
    /* fade out */
    for(int i=FADE_STEPS;i>=1;i--) {
        c->opacity=(float)i/FADE_STEPS;
        if(comp_ok) comp_repaint();
        XFlush(dpy); usleep(FADE_US);
    }
    c->minimized=1; c->opacity=0.0f;
    XUnmapWindow(dpy,c->frame);
    ewmh_publish_wm_state(c);
    comp_repaint();
}
static void restore_client(Client *c) {
    c->minimized=0; c->opacity=0.0f;
    XMapWindow(dpy,c->frame);
    for(int i=1;i<=FADE_STEPS;i++) {
        c->opacity=(float)i/FADE_STEPS;
        if(comp_ok) comp_repaint();
        XFlush(dpy); usleep(FADE_US);
    }
    c->opacity=1.0f;
    focus_client(c);
    ewmh_publish_wm_state(c);
}

static void toggle_maximize(Client *c) {
    if(c->maximized) {
        c->x=c->sx;c->y=c->sy;c->w=c->sw;c->h=c->sh;
        c->maximized=0;
    } else {
        c->sx=c->x;c->sy=c->y;c->sw=c->w;c->sh=c->h;
        c->x=work_x;c->y=work_y;c->w=work_w-2*RB;c->h=work_h-c->chrome_h;
        c->maximized=1;
    }
    XMoveResizeWindow(dpy,c->frame,c->x,c->y,FW(c),FH(c));
    XResizeWindow(dpy,c->client,c->w,c->h);
    apply_rounded_shape(c->frame,FW(c),FH(c));
    ewmh_publish_wm_state(c);
    if(comp_ok){c->dirty=1;comp_repaint();}
    draw_frame(c);
}

static void toggle_fullscreen(Client *c) {
    c->fullscreen = !c->fullscreen;
    if(c->fullscreen) {
        c->sx=c->x;c->sy=c->y;c->sw=c->w;c->sh=c->h;
        c->x=0;c->y=0;c->w=scr_w;c->h=scr_h;
        c->chrome_h = RB;
        XMoveResizeWindow(dpy,c->frame,0,0,scr_w,scr_h);
        XResizeWindow(dpy,c->client,scr_w,scr_h);
        XMoveWindow(dpy,c->client,0,0);
        XRaiseWindow(dpy,c->frame);
    } else {
        c->x=c->sx;c->y=c->sy;c->w=c->sw;c->h=c->sh;
        c->chrome_h = TITLEBAR_H + RB;
        XMoveResizeWindow(dpy,c->frame,c->x,c->y,FW(c),FH(c));
        XResizeWindow(dpy,c->client,c->w,c->h);
        XMoveWindow(dpy,c->client,RB,c->chrome_h-RB);
    }
    ewmh_publish_wm_state(c);
    if(comp_ok){c->dirty=1;comp_repaint();}
    draw_frame(c);
}

static void unframe_window(Window w) {
    Client *c=by_client(w); if(!c) return;
    /* fade out */
    for(int i=FADE_STEPS;i>=0;i--) {
        c->opacity=(float)i/FADE_STEPS;
        if(comp_ok) comp_repaint();
        XFlush(dpy); usleep(FADE_US);
    }
    comp_remove(c);
    if(!c->is_panel) {
        XReparentWindow(dpy,w,root,c->x,c->y);
        XDestroyWindow(dpy,c->frame);
    }
    /* remove from tab group */
    if(c->tab_group>=0) tg_remove(c->tab_group,cidx(c));

    int idx=cidx(c);
    memmove(&clients[idx],&clients[idx+1],(nc-idx-1)*sizeof(Client));
    nc--;
    comp_repaint();
    ewmh_update_client_list();
}

/* ══════════════════════════════════════════
   DRAG HELPERS
══════════════════════════════════════════ */
static int hit_btn(Client *c, int mx, int my) {
    if(my<0||my>=TITLEBAR_H) return 0;
    int r=BTN_D/2, by=TITLEBAR_H/2;
    int bx1=BTN_PAD_L+r, bx2=bx1+BTN_D+BTN_GAP, bx3=bx2+BTN_D+BTN_GAP;
    /* distance check */
    if((mx-bx1)*(mx-bx1)+(my-by)*(my-by)<=r*r) return 1;
    if((mx-bx2)*(mx-bx2)+(my-by)*(my-by)<=r*r) return 2;
    if((mx-bx3)*(mx-bx3)+(my-by)*(my-by)<=r*r) return 3;
    return 0;
}
static int get_zone_resize(Client *c, int mx, int my) {
    int fw=FW(c),fh=FH(c),d=0;
    if(my<RB)         d|=RD_N;
    else if(my>=fh-RB)d|=RD_S;
    if(mx<RB)         d|=RD_W;
    else if(mx>=fw-RB)d|=RD_E;
    return d;
}
static void start_drag(Client *c, int mode, int dir, int rx, int ry) {
    drag_frame=c->frame; drag_mode=mode; drag_dir=dir;
    dsx=rx; dsy=ry; dwx=c->x; dwy=c->y; dww=c->w; dwh=c->h;
    Cursor cur = (mode==1) ? cur_move : (dir ? cur_resize[dir&0xf] : cur_normal);
    XGrabPointer(dpy,root,False,ButtonPressMask|ButtonReleaseMask|PointerMotionMask,
                 GrabModeAsync,GrabModeAsync,None,cur,CurrentTime);
}

/* ══════════════════════════════════════════
   EVENT HANDLERS
══════════════════════════════════════════ */
static void on_map_request(XEvent *e) {
    frame_window(e->xmaprequest.window);
    Client *c=by_client(e->xmaprequest.window);
    if(c&&!c->is_panel) focus_client(c);
}
static void on_unmap_notify(XEvent *e) {
    if(e->xunmap.event==root) return;
    Client *c=by_client(e->xunmap.window);
    if(c&&!c->minimized) unframe_window(e->xunmap.window);
}
static void on_destroy_notify(XEvent *e) {
    Client *c=by_client(e->xdestroywindow.window);
    if(c) unframe_window(e->xdestroywindow.window);
}
static void on_configure_request(XEvent *e) {
    XConfigureRequestEvent *cr=&e->xconfigurerequest;
    Client *c=by_client(cr->window);
    if(c&&!c->is_panel) {
        if(cr->value_mask&CWWidth)  c->w=cr->width;
        if(cr->value_mask&CWHeight) c->h=cr->height;
        XResizeWindow(dpy,c->frame,FW(c),FH(c));
        XMoveResizeWindow(dpy,cr->window,RB,c->chrome_h-RB,c->w,c->h);
        apply_rounded_shape(c->frame,FW(c),FH(c));
        if(comp_ok) c->dirty=1;
    } else if(c&&c->is_panel) {
        if(cr->value_mask&CWWidth)  c->w=cr->width;
        if(cr->value_mask&CWHeight) c->h=cr->height;
        if(cr->value_mask&CWX)      c->x=cr->x;
        if(cr->value_mask&CWY)      c->y=cr->y;
        XWindowChanges wc; wc.x=c->x;wc.y=c->y;wc.width=c->w;wc.height=c->h;
        XConfigureWindow(dpy,cr->window,CWX|CWY|CWWidth|CWHeight,&wc);
        if(c->is_panel==2) update_work_area();
        if(comp_ok) { c->dirty=1; comp_repaint(); }
    } else {
        XWindowChanges wc;
        wc.x=cr->x;wc.y=cr->y;wc.width=cr->width;wc.height=cr->height;
        wc.border_width=cr->border_width;wc.sibling=cr->above;wc.stack_mode=cr->detail;
        XConfigureWindow(dpy,cr->window,cr->value_mask,&wc);
    }
}
static void on_expose(XEvent *e) {
    if(e->xexpose.count>0) return;
    Client *c=by_frame(e->xexpose.window);
    if(c&&!c->is_panel) draw_frame(c);
    else comp_repaint();
}
static void on_enter_notify(XEvent *e) {
    Client *c=by_frame(e->xcrossing.window);
    if(!c) c=by_client(e->xcrossing.window);
    if(c&&!c->is_panel&&!c->focused) focus_client(c);
}
static void on_leave_notify(XEvent *e) {
    Client *c=by_frame(e->xcrossing.window); if(!c||c->is_panel) return;
    if(hover_frame==c->frame){hover_frame=None;hover_btn=0;draw_frame(c);}
    if(last_cur_frame==c->frame){last_cur_frame=None;last_cur_zone=-1;XDefineCursor(dpy,c->frame,cur_normal);}
}
static void on_property_notify(XEvent *e) {
    if(e->xproperty.atom==XA_WM_NAME||e->xproperty.atom==NET_WM_NAME) {
        Client *c=by_client(e->xproperty.window);
        if(c&&!c->is_panel){update_title(c);draw_frame(c);}
    }
    if(e->xproperty.atom==NET_WM_STRUT||e->xproperty.atom==NET_WM_STRUT_PARTIAL)
        update_work_area();
}
static void on_damage_notify(XEvent *e) {
    XDamageNotifyEvent *de=(XDamageNotifyEvent*)e;
    for(int i=0;i<nc;i++) {
        if(clients[i].cdmg==de->damage) {
            XDamageSubtract(dpy,de->damage,None,None);
            clients[i].dirty=1;
            comp_repaint();
            return;
        }
    }
}

static void on_button_press(XEvent *e) {
    XButtonEvent *be=&e->xbutton;

    /* switcher click */
    if(switcher_open && be->window==switcher_win) {
        /* find clicked thumbnail */
        int nvis=0;
        for(int i=0;i<nc;i++) {
            Client *c=&clients[i];
            if(c->minimized||c->is_panel) continue;
            if(c->desk>=0&&c->desk!=current_desk&&!c->pip) continue;
            nvis++;
        }
        int cols=nvis<THUMB_COLS?nvis:THUMB_COLS;
        int cell_w=(scr_w-2*THUMB_PAD-(cols-1)*THUMB_GAP)/cols;
        int rows=(nvis+cols-1)/cols;
        int cell_h=(scr_h-2*THUMB_PAD-(rows-1)*THUMB_GAP)/rows;
        for(int i=0;i<nvis;i++) {
            int col=i%cols,row=i/cols;
            int tx=THUMB_PAD+col*(cell_w+THUMB_GAP);
            int ty=THUMB_PAD+row*(cell_h+THUMB_GAP);
            if(be->x>=tx&&be->x<tx+cell_w&&be->y>=ty&&be->y<ty+cell_h) {
                switcher_sel=i; switcher_select(); return;
            }
        }
        switcher_close_fn(); return;
    }

    /* client content drag */
    Client *cc=by_client(be->window);
    if(cc&&!cc->is_panel) {
        focus_client(cc);
        if(be->state&MOD)
            start_drag(cc,(be->button==Button1)?1:2,
                       (be->button==Button1)?0:(RD_S|RD_E),be->x_root,be->y_root);
        return;
    }

    Client *cf=by_frame(be->window); if(!cf||cf->is_panel) return;
    focus_client(cf);

    /* right-click titlebar → menu */
    if(be->button==Button3 && be->y<TITLEBAR_H) {
        titlebar_menu(cf,be); return;
    }

    /* traffic-light buttons */
    int btn=hit_btn(cf,be->x,be->y);
    if(btn&&be->button==Button1) {
        if(btn==1) close_client(cf);
        else if(btn==2) minimize_client(cf);
        else if(btn==3) toggle_maximize(cf);
        return;
    }

    /* tab strip click */
    if(cf->tab_group>=0 && be->y>=TITLEBAR_H && be->y<TITLEBAR_H+TAB_H) {
        TabGroup *tg=&tab_groups[cf->tab_group];
        int tab_w = (cf->w > 0) ? cf->w / tg->count : 1;
        int clicked = (be->x - RB) / tab_w;
        if(clicked>=0&&clicked<tg->count) { tg_activate(cf->tab_group,clicked); return; }
    }

    /* resize zone */
    int zone=get_zone_resize(cf,be->x,be->y);
    if(zone&&!cf->fullscreen){start_drag(cf,2,zone,be->x_root,be->y_root);return;}

    /* titlebar drag / double-click */
    if(be->y<TITLEBAR_H&&be->button==Button1&&!cf->fullscreen) {
        if(be->window==dbl_win&&(be->time-dbl_time)<DBL_MS){
            toggle_maximize(cf);dbl_win=None;return;
        }
        dbl_win=be->window;dbl_time=be->time;
        start_drag(cf,1,0,be->x_root,be->y_root);
    }
}

static void on_button_release(XEvent *e) {
    if(drag_frame==None) return;
    Client *c=by_frame(drag_frame);
    int was_move=(drag_mode==1);
    int rx=e->xbutton.x_root, ry=e->xbutton.y_root;
    XUngrabPointer(dpy,CurrentTime);
    drag_frame=None;drag_mode=0;drag_dir=0;

    /* hide ghost */
    ghost_show(ZONE_NONE);
    if(ghost_win) XUnmapWindow(dpy,ghost_win);
    ghost_zone=ZONE_NONE;

    /* snap to tile zone on drop */
    if(c&&was_move) {
        int zone=detect_tile_zone(rx,ry);
        if(zone!=ZONE_NONE) tile_to_zone(c,zone);
    }
}

static void on_motion_notify(XEvent *e) {
    if(drag_frame!=None) {
        while(XCheckTypedEvent(dpy,MotionNotify,e));
        Client *c=by_frame(drag_frame); if(!c) return;
        int dx=e->xmotion.x_root-dsx, dy=e->xmotion.y_root-dsy;

        if(drag_mode==1) {
            int nx=dwx+dx, ny=dwy+dy;
            /* edge snap */
            if(abs(nx-work_x)<SNAP_PX)                 nx=work_x;
            if(abs(ny-work_y)<SNAP_PX)                 ny=work_y;
            if(abs(nx+FW(c)-work_x-work_w)<SNAP_PX)   nx=work_x+work_w-FW(c);
            if(abs(ny+FH(c)-work_y-work_h)<SNAP_PX)   ny=work_y+work_h-FH(c);
            c->x=nx; c->y=ny;
            XMoveWindow(dpy,c->frame,nx,ny);
            /* ghost preview */
            int zone=detect_tile_zone(e->xmotion.x_root,e->xmotion.y_root);
            ghost_show(zone);
            if(comp_ok) comp_repaint();
        } else {
            int nx=dwx,ny=dwy,nw=dww,nh=dwh;
            if(drag_dir&RD_E) nw=dww+dx;
            if(drag_dir&RD_S) nh=dwh+dy;
            if(drag_dir&RD_W){nw=dww-dx;nx=dwx+dx;}
            if(drag_dir&RD_N){nh=dwh-dy;ny=dwy+dy;}
            if(nw<MIN_W){if(drag_dir&RD_W)nx=dwx+dww-MIN_W;nw=MIN_W;}
            if(nh<MIN_H){if(drag_dir&RD_N)ny=dwy+dwh-MIN_H;nh=MIN_H;}
            c->x=nx;c->y=ny;c->w=nw;c->h=nh;
            XMoveResizeWindow(dpy,c->frame,nx,ny,FW(c),FH(c));
            XResizeWindow(dpy,c->client,nw,nh);
            XMoveWindow(dpy,c->client,RB,c->chrome_h-RB);
            apply_rounded_shape(c->frame,FW(c),FH(c));
            if(comp_ok)c->dirty=1;
            draw_frame(c);
        }
    } else {
        Client *c=by_frame(e->xmotion.window); if(!c||c->is_panel) return;
        int zone=get_zone_resize(c,e->xmotion.x,e->xmotion.y);
        if(zone!=last_cur_zone||c->frame!=last_cur_frame) {
            last_cur_zone=zone;last_cur_frame=c->frame;
            XDefineCursor(dpy,c->frame,zone?cur_resize[zone&0xf]:cur_normal);
        }
        int hbtn=(e->xmotion.y>=0&&e->xmotion.y<TITLEBAR_H)
                 ?hit_btn(c,e->xmotion.x,e->xmotion.y):0;
        if(hover_frame!=c->frame||hover_btn!=hbtn){
            hover_frame=c->frame;hover_btn=hbtn;draw_frame(c);
        }
    }
}

static void on_client_message(XEvent *e) {
    XClientMessageEvent *cm=&e->xclient;
    Client *c=by_client(cm->window); if(!c||c->is_panel) return;
    if(cm->message_type==NET_WM_STATE) {
        int action=(int)cm->data.l[0];
        for(int i=1;i<=2;i++) {
            Atom a=(Atom)cm->data.l[i]; if(!a) continue;
            if(a==NET_WM_STATE_MAX_H||a==NET_WM_STATE_MAX_V) {
                int want=(action==1)||(action==2&&!c->maximized);
                if(want!=!!c->maximized) toggle_maximize(c);
            }
            if(a==NET_WM_STATE_FULLSCREEN) {
                int want=(action==1)||(action==2&&!c->fullscreen);
                if(want!=c->fullscreen) toggle_fullscreen(c);
            }
            if(a==NET_WM_STATE_HIDDEN) {
                if(action==0&&c->minimized) restore_client(c);
                else if(action==1&&!c->minimized) minimize_client(c);
            }
        }
    }
    if(cm->message_type==NET_ACTIVE_WIN) {
        if(c->minimized) restore_client(c);
        else focus_client(c);
    }
    if(cm->message_type==NET_WM_DESKTOP) {
        move_to_desk(c,(int)cm->data.l[0]);
    }
}

static void on_key_press(XEvent *e) {
    if(switcher_open) {
        int vis_count=0;
        for(int i=0;i<nc;i++) {
            Client *c=&clients[i];
            if(!c->minimized&&!c->is_panel&&(c->desk<0||c->desk==current_desk)) vis_count++;
        }
        KeySym sym=XkbKeycodeToKeysym(dpy,e->xkey.keycode,0,0);
        if(sym==XK_Escape)  { switcher_close_fn(); return; }
        if(sym==XK_Return)  { switcher_select();   return; }
        if(sym==XK_Left)    { if(switcher_sel>0)          {switcher_sel--;switcher_draw();} return; }
        if(sym==XK_Right)   { if(switcher_sel<vis_count-1){switcher_sel++;switcher_draw();} return; }
        if(sym==XK_Up)      { if(switcher_sel>=THUMB_COLS) {switcher_sel-=THUMB_COLS;switcher_draw();} return; }
        if(sym==XK_Down)    { if(switcher_sel+THUMB_COLS<vis_count){switcher_sel+=THUMB_COLS;switcher_draw();} return; }
        return;
    }

    KeySym sym=XkbKeycodeToKeysym(dpy,e->xkey.keycode,0,0);
    unsigned int mod=e->xkey.state;

    /* quit */
    if((mod&MOD)&&(mod&ShiftMask)&&sym==XK_q) exit(0);

    /* launch */
    if((mod&MOD)&&sym==XK_Return)          { spawn(cfg_terminal); return; }
    if((mod&MOD)&&(sym==XK_d||sym==XK_F2)) { spawn(cfg_launcher); return; }

    /* close focused */
    if((mod&MOD)&&sym==XK_q) {
        Window fw; int rev; XGetInputFocus(dpy,&fw,&rev);
        Client *c=by_client(fw); if(c&&!c->is_panel) close_client(c); return;
    }

    /* thumbnail switcher */
    if((mod&MOD)&&sym==XK_Tab) { switcher_open_fn(); return; }

    /* workspace switch: Super+1-4 */
    if((mod&MOD)&&!(mod&ShiftMask)&&sym>=XK_1&&sym<=XK_4) {
        switch_desk((int)(sym-XK_1)); return;
    }
    /* move window to workspace: Super+Shift+1-4 */
    if((mod&MOD)&&(mod&ShiftMask)&&sym>=XK_1&&sym<=XK_4) {
        Window fw; int rev; XGetInputFocus(dpy,&fw,&rev);
        Client *c=by_client(fw);
        if(c&&!c->is_panel) move_to_desk(c,(int)(sym-XK_1));
        return;
    }

    /* get focused client for window ops */
    Window fw; int rev; XGetInputFocus(dpy,&fw,&rev);
    Client *c=by_client(fw);

    /* fullscreen */
    if((mod&MOD)&&sym==XK_f) { if(c&&!c->is_panel) toggle_fullscreen(c); return; }

    /* PiP */
    if((mod&MOD)&&sym==XK_p) {
        if(c&&!c->is_panel) { if(c->pip) pip_disable(c); else pip_enable(c); }
        return;
    }

    /* tab: merge / detach */
    if((mod&MOD)&&sym==XK_t) { tab_merge_focused(); return; }
    if((mod&MOD)&&sym==XK_u) { tab_detach_focused(); return; }

    /* tiling: halves */
    if(c&&!c->is_panel&&(mod&MOD)&&!(mod&(ShiftMask|ControlMask))) {
        if(sym==XK_Left)  { tile_to_zone(c,ZONE_LEFT);  return; }
        if(sym==XK_Right) { tile_to_zone(c,ZONE_RIGHT); return; }
        if(sym==XK_Up)    { if(!c->maximized)toggle_maximize(c); return; }
        if(sym==XK_Down)  {
            if(c->fullscreen) toggle_fullscreen(c);
            else if(c->maximized) toggle_maximize(c);
            else minimize_client(c);
            return;
        }
    }

    /* quarter tiling: Super+Ctrl+arrows */
    if(c&&!c->is_panel&&(mod&MOD)&&(mod&ControlMask)&&!(mod&ShiftMask)) {
        if(sym==XK_Left)  { tile_to_zone(c,ZONE_TL); return; }
        if(sym==XK_Right) { tile_to_zone(c,ZONE_TR); return; }
    }
    if(c&&!c->is_panel&&(mod&MOD)&&(mod&ControlMask)&&(mod&ShiftMask)) {
        if(sym==XK_Left)  { tile_to_zone(c,ZONE_BL); return; }
        if(sym==XK_Right) { tile_to_zone(c,ZONE_BR); return; }
    }

    /* alt+tab fallback (cycle without switcher) */
    if((mod&MOD)&&!(mod&ShiftMask)&&sym==XK_grave) {
        int cur=-1;
        for(int i=0;i<nc;i++) if(clients[i].focused){cur=i;break;}
        for(int off=1;off<=nc;off++) {
            int next=(cur+off)%nc;
            if(!clients[next].minimized&&!clients[next].is_panel&&
               (clients[next].desk<0||clients[next].desk==current_desk)) {
                focus_client(&clients[next]); break;
            }
        }
        return;
    }
}

static int xerr(Display *d, XErrorEvent *ev) { (void)d; (void)ev; return 0; }

/* ══════════════════════════════════════════
   MAIN
══════════════════════════════════════════ */
int main(void) {
    dpy = XOpenDisplay(NULL);
    if(!dpy){fputs("shadowwm: cannot open display\n",stderr);return 1;}
    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy,screen);
    scr_w  = DisplayWidth(dpy,screen);
    scr_h  = DisplayHeight(dpy,screen);
    work_x=0;work_y=0;work_w=scr_w;work_h=scr_h;

    XSetErrorHandler(xerr);
    XSelectInput(dpy,root,
        SubstructureRedirectMask|SubstructureNotifyMask|
        KeyPressMask|PropertyChangeMask);
    XSync(dpy,False);

    /* load config */
    load_config();

    /* GC */
    XGCValues gcv;
    gcv.foreground=xcolor(C_TXT_FOC);
    gcv.background=xcolor(C_BAR_FOC_B);
    gc = XCreateGC(dpy,root,GCForeground|GCBackground,&gcv);

    /* font */
    xfont = XftFontOpenName(dpy,screen,FONT_NAME);
    if(!xfont) xfont=XftFontOpenName(dpy,screen,"fixed:size=9");

    /* atoms */
    WM_DEL               =XInternAtom(dpy,"WM_DELETE_WINDOW",     False);
    WM_PROTO             =XInternAtom(dpy,"WM_PROTOCOLS",         False);
    WM_TAKE_FOCUS        =XInternAtom(dpy,"WM_TAKE_FOCUS",        False);
    WM_CLASS_ATOM        =XInternAtom(dpy,"WM_CLASS",             False);
    NET_WM_TYPE          =XInternAtom(dpy,"_NET_WM_WINDOW_TYPE",           False);
    NET_WM_TYPE_DESKTOP  =XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_DESKTOP",   False);
    NET_WM_TYPE_DOCK     =XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_DOCK",      False);
    NET_WM_TYPE_DIALOG   =XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_DIALOG",    False);
    NET_WM_TYPE_SPLASH   =XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_SPLASH",    False);
    NET_WM_TYPE_TOOLTIP  =XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_TOOLTIP",   False);
    NET_WM_TYPE_UTILITY  =XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_UTILITY",   False);
    NET_WM_TYPE_POPUP    =XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_POPUP_MENU",False);
    NET_WM_TYPE_NOTIFICATION=XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_NOTIFICATION",False);
    NET_WM_STATE         =XInternAtom(dpy,"_NET_WM_STATE",                 False);
    NET_WM_STATE_MAX_H   =XInternAtom(dpy,"_NET_WM_STATE_MAXIMIZED_HORZ",  False);
    NET_WM_STATE_MAX_V   =XInternAtom(dpy,"_NET_WM_STATE_MAXIMIZED_VERT",  False);
    NET_WM_STATE_HIDDEN  =XInternAtom(dpy,"_NET_WM_STATE_HIDDEN",          False);
    NET_WM_STATE_FULLSCREEN=XInternAtom(dpy,"_NET_WM_STATE_FULLSCREEN",    False);
    NET_WM_STATE_ABOVE   =XInternAtom(dpy,"_NET_WM_STATE_ABOVE",           False);
    NET_WM_STATE_BELOW   =XInternAtom(dpy,"_NET_WM_STATE_BELOW",           False);
    NET_WM_STRUT         =XInternAtom(dpy,"_NET_WM_STRUT",                 False);
    NET_WM_STRUT_PARTIAL =XInternAtom(dpy,"_NET_WM_STRUT_PARTIAL",         False);
    NET_ACTIVE_WIN       =XInternAtom(dpy,"_NET_ACTIVE_WINDOW",            False);
    NET_CLIENT_LIST      =XInternAtom(dpy,"_NET_CLIENT_LIST",              False);
    NET_SUPPORTED        =XInternAtom(dpy,"_NET_SUPPORTED",                False);
    NET_WM_NAME          =XInternAtom(dpy,"_NET_WM_NAME",                  False);
    NET_WM_DESKTOP       =XInternAtom(dpy,"_NET_WM_DESKTOP",               False);
    NET_CURRENT_DESKTOP  =XInternAtom(dpy,"_NET_CURRENT_DESKTOP",          False);
    NET_NUMBER_DESKTOPS  =XInternAtom(dpy,"_NET_NUMBER_OF_DESKTOPS",       False);
    UTF8_STRING          =XInternAtom(dpy,"UTF8_STRING",                   False);

    /* advertise EWMH */
    Atom supported[]={
        NET_WM_TYPE,NET_WM_TYPE_DESKTOP,NET_WM_TYPE_DOCK,
        NET_WM_STATE,NET_WM_STATE_MAX_H,NET_WM_STATE_MAX_V,
        NET_WM_STATE_HIDDEN,NET_WM_STATE_FULLSCREEN,
        NET_WM_STATE_ABOVE,NET_WM_STATE_BELOW,
        NET_WM_STRUT,NET_WM_STRUT_PARTIAL,
        NET_ACTIVE_WIN,NET_CLIENT_LIST,NET_WM_NAME,
        NET_WM_DESKTOP,NET_CURRENT_DESKTOP,NET_NUMBER_DESKTOPS
    };
    XChangeProperty(dpy,root,NET_SUPPORTED,XA_ATOM,32,PropModeReplace,
                    (unsigned char*)supported,(int)(sizeof(supported)/sizeof(supported[0])));

    /* publish workspace info */
    long nd=NUM_DESKS;
    XChangeProperty(dpy,root,NET_NUMBER_DESKTOPS,XA_CARDINAL,32,PropModeReplace,(unsigned char*)&nd,1);
    ewmh_update_current_desk();

    /* cursors */
    cur_normal             = XCreateFontCursor(dpy,XC_left_ptr);
    cur_move               = XCreateFontCursor(dpy,XC_fleur);
    cur_resize[RD_N]       = XCreateFontCursor(dpy,XC_top_side);
    cur_resize[RD_S]       = XCreateFontCursor(dpy,XC_bottom_side);
    cur_resize[RD_W]       = XCreateFontCursor(dpy,XC_left_side);
    cur_resize[RD_E]       = XCreateFontCursor(dpy,XC_right_side);
    cur_resize[RD_N|RD_W]  = XCreateFontCursor(dpy,XC_top_left_corner);
    cur_resize[RD_N|RD_E]  = XCreateFontCursor(dpy,XC_top_right_corner);
    cur_resize[RD_S|RD_W]  = XCreateFontCursor(dpy,XC_bottom_left_corner);
    cur_resize[RD_S|RD_E]  = XCreateFontCursor(dpy,XC_bottom_right_corner);

    /* compositor */
    comp_init();

    /* key grabs */
    struct{KeySym k;unsigned int m;}keys[]={
        {XK_Return,MOD},{XK_d,MOD},{XK_F2,MOD},{XK_q,MOD},{XK_q,MOD|ShiftMask},
        {XK_Tab,MOD},{XK_grave,MOD},
        {XK_f,MOD},{XK_p,MOD},{XK_t,MOD},{XK_u,MOD},
        {XK_1,MOD},{XK_2,MOD},{XK_3,MOD},{XK_4,MOD},
        {XK_1,MOD|ShiftMask},{XK_2,MOD|ShiftMask},{XK_3,MOD|ShiftMask},{XK_4,MOD|ShiftMask},
        {XK_Left,MOD},{XK_Right,MOD},{XK_Up,MOD},{XK_Down,MOD},
        {XK_Left,MOD|ControlMask},{XK_Right,MOD|ControlMask},
        {XK_Left,MOD|ControlMask|ShiftMask},{XK_Right,MOD|ControlMask|ShiftMask},
    };
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++)
        XGrabKey(dpy,XKeysymToKeycode(dpy,keys[i].k),keys[i].m,
                 root,True,GrabModeAsync,GrabModeAsync);

    /* adopt existing windows */
    Window w1,w2,*wins; unsigned int nw;
    XQueryTree(dpy,root,&w1,&w2,&wins,&nw);
    for(unsigned i=0;i<nw;i++){
        XWindowAttributes wa; XGetWindowAttributes(dpy,wins[i],&wa);
        if(!wa.override_redirect&&wa.map_state==IsViewable) frame_window(wins[i]);
    }
    if(wins) XFree(wins);

    fputs("shadowwm v5: running — Super+? for commands, Super+Shift+Q to quit\n",stderr);

    /* event loop */
    XEvent ev;
    for(;;) {
        XNextEvent(dpy,&ev);
        if(comp_ok&&ev.type==dmg_base+XDamageNotify){on_damage_notify(&ev);continue;}
        switch(ev.type){
        case MapRequest:       on_map_request(&ev);      break;
        case UnmapNotify:      on_unmap_notify(&ev);     break;
        case DestroyNotify:    on_destroy_notify(&ev);   break;
        case ConfigureRequest: on_configure_request(&ev);break;
        case ClientMessage:    on_client_message(&ev);   break;
        case ButtonPress:      on_button_press(&ev);     break;
        case ButtonRelease:    on_button_release(&ev);   break;
        case MotionNotify:     on_motion_notify(&ev);    break;
        case Expose:           on_expose(&ev);           break;
        case EnterNotify:      on_enter_notify(&ev);     break;
        case LeaveNotify:      on_leave_notify(&ev);     break;
        case KeyPress:         on_key_press(&ev);        break;
        case PropertyNotify:   on_property_notify(&ev);  break;
        }
    }
}
