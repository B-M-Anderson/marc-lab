/* Sony IR Search v3.1 — binary-searches SIRC cmd space for Sony CMT-NE3
 * Protocol: SIRC 12-bit (7-bit cmd + 5-bit addr), carrier 40kHz handled by SDK
 * Common Sony audio addresses: 0=audio/general, 1=TV/audio, 17=CD, 18=Tuner/Deck
 *
 * Controls (context-sensitive, shown on every screen):
 *   Welcome  : [OK] start  [^][v] addr  [<] exit
 *   Sending  : [<] skip function
 *   Asking   : [^] yes  [v] no  [>] resend batch  [<] skip
 *   Single   : [OK] yes  [v] no  [>] resend code   [<] skip
 *   Found    : [OK] next  [>] test code again
 *   Saved    : [OK]/[<] exit
 */

#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <storage/storage.h>
#include <lib/infrared/worker/infrared_transmit.h>
#include <lib/infrared/encoder_decoder/infrared.h>
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════ */
/*  Constants                                                        */
/* ══════════════════════════════════════════════════════════════════ */

#define SIRC_ADDR_DEFAULT   1       /* Sony audio addr; 0 also common  */
#define CMD_MIN             0
#define CMD_MAX             127
#define NUM_FUNCS           12
#define SEND_REPS           3       /* IR repetitions per command code  */
#define INTER_CMD_MS        55      /* pause between successive codes   */
#define OUTPUT_PATH         "/ext/infrared/Sony_CMT_NE3_confirmed.ir"
#define RECORD_STORAGE      "storage"
#define ANIM_MS             60      /* animation tick period            */
#define APP_VIEW            0

/* Common Sony SIRC-12 device addresses to cycle through on welcome */
static const uint8_t ADDR_OPTIONS[]  = { 0, 1, 17, 18, 26 };
static const char*   ADDR_LABELS[]   = { "0=audio", "1=audio", "17=CD", "18=deck", "26=sys" };
#define NUM_ADDR_OPTIONS    5

/* ══════════════════════════════════════════════════════════════════ */
/*  State machine                                                    */
/* ══════════════════════════════════════════════════════════════════ */

typedef enum {
    AppStateWelcome,   /* title / start screen                 */
    AppStateSending,   /* IR thread blasting a range           */
    AppStateAsking,    /* range question: ^yes vno >resend <skip */
    AppStateSingle,    /* final 1-cmd confirm: OKyes vno >resend */
    AppStateFound,     /* code confirmed                       */
    AppStateNotFound,  /* no code found for this function      */
    AppStateDone,      /* all functions done, saving           */
    AppStateSaved,     /* file written                         */
} AppState;

typedef enum {
    EvtIrProgress = 0,
    EvtIrDone     = 1,
} AppEvt;

/* ══════════════════════════════════════════════════════════════════ */
/*  Function table                                                   */
/* ══════════════════════════════════════════════════════════════════ */

static const char* const FUNC_NAMES[NUM_FUNCS] = {
    "Power",   "Vol+",  "Vol-",  "CD",
    "Tape",    "Tuner", "Mute",  "Play",
    "Stop",    "Pause", "Next",  "Prev",
};

/* ══════════════════════════════════════════════════════════════════ */
/*  View model  (ViewModelTypeLocking — GUI thread + event loop)    */
/* ══════════════════════════════════════════════════════════════════ */

typedef struct {
    AppState state;
    uint8_t  fi;            /* current function index 0-11      */
    uint8_t  lo, hi;        /* binary-search bounds (inclusive) */
    uint8_t  round;         /* 1-based round counter            */
    uint8_t  send_prog;     /* command currently being sent     */
    uint8_t  anim;          /* 0-255 wrapping, driven by tick   */
    uint8_t  addr_idx;      /* index into ADDR_OPTIONS[]        */
    int32_t  codes[NUM_FUNCS];  /* -1 = skipped / not found     */
    bool     done[NUM_FUNCS];
    uint8_t  found_count;
} AppModel;

/* ══════════════════════════════════════════════════════════════════ */
/*  App context                                                      */
/* ══════════════════════════════════════════════════════════════════ */

typedef struct {
    ViewDispatcher* vd;
    View*           view;
    Gui*            gui;
    FuriThread*     ir_thread;

    /* set by main before thread start; read by thread             */
    uint8_t  tlo, thi, taddr;

    /* written by IR thread; read by main via custom event         */
    volatile uint8_t t_prog;
    volatile bool    abort;
    bool             resending; /* true = resend, not advancing search */
} App;

/* ══════════════════════════════════════════════════════════════════ */
/*  Drawing primitives                                               */
/* ══════════════════════════════════════════════════════════════════ */

static void draw_progress_bar(Canvas* c, uint8_t done, uint8_t total) {
    canvas_draw_frame(c, 0, 0, 128, 6);
    if(done > 0) {
        uint8_t w = (uint8_t)((uint16_t)126 * done / total);
        if(w > 0) canvas_draw_box(c, 1, 1, w, 4);
    }
}

static void draw_sep(Canvas* c) {
    canvas_draw_line(c, 0, 7, 127, 7);
}

/* Fraction "N/M" right-aligned in the progress bar */
static void draw_frac(Canvas* c, uint8_t done, uint8_t total) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%u/%u", done, total);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 126, 0, AlignRight, AlignTop, buf);
}

/* header = progress bar + separator + fraction */
static void draw_header(Canvas* c, AppModel* m) {
    draw_progress_bar(c, m->found_count, NUM_FUNCS);
    draw_sep(c);
    draw_frac(c, m->found_count, NUM_FUNCS);
}

/* Animated IR beam: 4 dots moving from x1 to x2 at row y */
static void draw_ir_beam(Canvas* c, uint8_t x1, uint8_t x2, uint8_t y, uint8_t anim) {
    uint8_t span = x2 - x1;
    if(span == 0) return;
    for(uint8_t d = 0; d < 4; d++) {
        uint8_t phase = (uint8_t)(((uint16_t)anim * 2 + d * 16) % span);
        uint8_t x = x1 + phase;
        if(x <= x2) canvas_draw_disc(c, x, y, 1);
    }
}

/* Compact Flipper silhouette ~10×8 */
static void draw_flipper(Canvas* c, uint8_t x, uint8_t y) {
    canvas_draw_rframe(c, x, y + 2, 10, 6, 1);
    canvas_draw_line(c, x + 5, y, x + 5, y + 2);
    canvas_draw_dot(c, x + 5, y);
}

/* Compact stereo silhouette ~14×8 */
static void draw_stereo(Canvas* c, uint8_t x, uint8_t y) {
    canvas_draw_frame(c, x, y, 14, 8);
    canvas_draw_disc(c, x + 4, y + 4, 2);
    canvas_draw_disc(c, x + 10, y + 4, 2);
    canvas_draw_line(c, x + 1, y + 1, x + 12, y + 1);
}

/* Growing checkmark animation (anim 0→32) */
static void draw_checkmark(Canvas* c, uint8_t ox, uint8_t oy, uint8_t anim) {
    uint8_t p = anim > 32 ? 32 : anim;
    if(p > 0) {
        uint8_t p1 = p > 10 ? 10 : p;
        canvas_draw_line(c, ox, oy, ox + p1 / 2, oy + p1 / 2);
    }
    if(p > 10) {
        uint8_t p2 = (uint8_t)(p - 10);
        uint8_t cap = p2 > 22 ? 22 : p2;
        canvas_draw_line(c, ox + 5, oy + 5, ox + 5 + cap, oy + 5 - cap / 2);
    }
}

/* Compute estimated seconds for current sending round */
static uint32_t round_eta_seconds(uint8_t lo, uint8_t hi) {
    uint8_t mid = (lo + hi) / 2;
    uint32_t cmds = (uint32_t)(mid - lo + 1);
    /* each command: ~45ms × 3 reps + 55ms inter-cmd gap ≈ 190ms */
    return (cmds * 190u + 500u) / 1000u;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Per-state draw functions                                         */
/* ══════════════════════════════════════════════════════════════════ */

static void draw_welcome(Canvas* c, AppModel* m) {
    draw_header(c, m);

    /* Alternating inverted / normal title for a shimmer effect */
    bool inv = (m->anim / 12) % 2 == 0;
    canvas_set_font(c, FontPrimary);
    if(inv) {
        canvas_draw_box(c, 14, 10, 100, 12);
        canvas_set_color(c, ColorWhite);
    }
    canvas_draw_str_aligned(c, 64, 21, AlignCenter, AlignBottom, "SONY IR FINDER");
    canvas_set_color(c, ColorBlack);

    /* Dynamic subtitle showing current address selection */
    char info[32];
    snprintf(info, sizeof(info), "SIRC-12  addr=%s", ADDR_LABELS[m->addr_idx]);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 31, AlignCenter, AlignBottom, info);

    /* Animated Flipper→stereo scene */
    draw_flipper(c, 8, 37);
    draw_stereo(c, 106, 37);
    draw_ir_beam(c, 22, 102, 41, m->anim);

    /* Address selector row: [^][v] cycle, current address highlighted */
    bool fill_btn = (m->anim / 18) % 2 == 0;
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 52, "[^][v]");
    if(fill_btn) {
        canvas_draw_rbox(c, 28, 44, 40, 10, 2);
        canvas_set_color(c, ColorWhite);
        canvas_draw_str_aligned(c, 48, 53, AlignCenter, AlignBottom, ADDR_LABELS[m->addr_idx]);
        canvas_set_color(c, ColorBlack);
    } else {
        canvas_draw_rframe(c, 28, 44, 40, 10, 2);
        canvas_draw_str_aligned(c, 48, 53, AlignCenter, AlignBottom, ADDR_LABELS[m->addr_idx]);
    }

    /* Bottom row: start / exit */
    canvas_draw_str(c, 2, 63, "[OK] Start");
    canvas_draw_str(c, 78, 63, "[Back] Exit");
}

static void draw_sending(Canvas* c, AppModel* m) {
    draw_header(c, m);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 18, FUNC_NAMES[m->fi]);

    canvas_set_font(c, FontSecondary);
    char rnd[12];
    snprintf(rnd, sizeof(rnd), "Rnd %u/7", m->round);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignTop, rnd);

    uint8_t mid = (m->lo + m->hi) / 2;
    char rng[28];
    snprintf(rng, sizeof(rng), "Testing %u-%u", m->lo, mid);
    canvas_draw_str(c, 2, 28, rng);

    /* ETA */
    uint32_t eta = round_eta_seconds(m->lo, m->hi);
    char etastr[16];
    if(eta > 0) snprintf(etastr, sizeof(etastr), "~%lus", (unsigned long)eta);
    else snprintf(etastr, sizeof(etastr), "<1s");
    canvas_draw_str_aligned(c, 126, 19, AlignRight, AlignTop, etastr);

    /* IR scene */
    draw_flipper(c, 2, 34);
    draw_stereo(c, 106, 34);
    draw_ir_beam(c, 16, 102, 38, m->anim);

    /* Current code progress */
    char prog[20];
    snprintf(prog, sizeof(prog), "Sending: %u", m->send_prog);
    canvas_draw_str(c, 2, 53, prog);

    canvas_draw_str(c, 2, 63, "[<] Skip");
}

static void draw_asking(Canvas* c, AppModel* m) {
    draw_header(c, m);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 18, FUNC_NAMES[m->fi]);

    canvas_set_font(c, FontSecondary);
    char rnd[12];
    snprintf(rnd, sizeof(rnd), "Rnd %u/7", m->round);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignTop, rnd);

    uint8_t mid = (m->lo + m->hi) / 2;
    char sent[30];
    snprintf(sent, sizeof(sent), "Sent codes %u - %u", m->lo, mid);
    canvas_draw_str_aligned(c, 64, 30, AlignCenter, AlignBottom, sent);

    /* Blinking question */
    if((m->anim / 14) % 2 == 0) {
        canvas_set_font(c, FontPrimary);
        canvas_draw_str_aligned(c, 64, 44, AlignCenter, AlignBottom, "Did stereo react?");
    }

    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c,  2, 63, "[^]Yes");
    canvas_draw_str(c, 36, 63, "[v]No");
    canvas_draw_str(c, 66, 63, "[>]Again");
    canvas_draw_str(c, 101, 63, "[<]Skip");
}

static void draw_single(Canvas* c, AppModel* m) {
    draw_header(c, m);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 18, FUNC_NAMES[m->fi]);

    /* Highlighted code box — pulse with anim */
    uint8_t pulse = (m->anim / 6) % 2;
    if(pulse) canvas_draw_rbox(c, 18, 20, 92, 12, 2);
    else       canvas_draw_rframe(c, 18, 20, 92, 12, 2);

    char codestr[22];
    snprintf(codestr, sizeof(codestr), "Code %u  (0x%02X)", m->lo, m->lo);
    canvas_set_font(c, FontSecondary);
    if(pulse) canvas_set_color(c, ColorWhite);
    canvas_draw_str_aligned(c, 64, 30, AlignCenter, AlignBottom, codestr);
    canvas_set_color(c, ColorBlack);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 44, AlignCenter, AlignBottom, "Did it trigger?");

    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c,  2, 63, "[OK]Yes");
    canvas_draw_str(c, 40, 63, "[v]No");
    canvas_draw_str(c, 66, 63, "[>]Resend");
    canvas_draw_str(c, 104, 63, "[<]Skip");
}

static void draw_found(Canvas* c, AppModel* m) {
    draw_header(c, m);

    /* Animated checkmark top-right */
    draw_checkmark(c, 108, 13, m->anim);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 55, 22, AlignCenter, AlignBottom, "FOUND!");

    char detail[28];
    snprintf(
        detail, sizeof(detail), "%s = %u  (0x%02X)",
        FUNC_NAMES[m->fi],
        (uint8_t)m->codes[m->fi],
        (uint8_t)m->codes[m->fi]);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 34, AlignCenter, AlignBottom, detail);

    /* Pulsing circle */
    uint8_t r = 5 + (m->anim % 4);
    canvas_draw_circle(c, 64, 47, r);
    if((m->anim / 4) % 2 == 0) canvas_draw_disc(c, 64, 47, r - 3 > 0 ? r - 3 : 1);

    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 63, "[OK] Next");
    canvas_draw_str(c, 66, 63, "[>] Test again");
}

static void draw_not_found(Canvas* c, AppModel* m) {
    draw_header(c, m);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 24, AlignCenter, AlignBottom, "Not Found");

    canvas_set_font(c, FontSecondary);
    char line[36];
    snprintf(line, sizeof(line), "%s: no code identified", FUNC_NAMES[m->fi]);
    canvas_draw_str_aligned(c, 64, 36, AlignCenter, AlignBottom, line);

    /* X mark */
    canvas_draw_line(c, 58, 42, 70, 54);
    canvas_draw_line(c, 70, 42, 58, 54);

    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "[OK] Next function");
}

static void draw_done(Canvas* c, AppModel* m) {
    /* use full bar while saving */
    draw_progress_bar(c, NUM_FUNCS, NUM_FUNCS);
    draw_sep(c);

    canvas_set_font(c, FontPrimary);
    char summary[32];
    snprintf(summary, sizeof(summary), "Done!  %u/%u found", m->found_count, NUM_FUNCS);
    canvas_draw_str_aligned(c, 64, 22, AlignCenter, AlignBottom, summary);

    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 34, AlignCenter, AlignBottom, "Saving remote file...");

    /* Animated dots */
    char dots[5] = {0};
    uint8_t n = (m->anim / 10) % 4;
    for(uint8_t i = 0; i < n; i++) dots[i] = '.';
    canvas_draw_str_aligned(c, 64, 46, AlignCenter, AlignBottom, dots);
}

static void draw_saved(Canvas* c, AppModel* m) {
    draw_progress_bar(c, NUM_FUNCS, NUM_FUNCS);
    draw_sep(c);
    draw_frac(c, NUM_FUNCS, NUM_FUNCS);

    canvas_set_font(c, FontPrimary);
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "Saved! (%u found)", m->found_count);
    canvas_draw_str_aligned(c, 64, 20, AlignCenter, AlignBottom, hdr);

    /* Scrolling results strip */
    canvas_set_font(c, FontSecondary);
    /* show up to 3 rows of results, cycling via anim */
    uint8_t start = ((uint16_t)(m->anim / 40)) % NUM_FUNCS;
    for(uint8_t row = 0; row < 3; row++) {
        uint8_t idx = (start + row) % NUM_FUNCS;
        char row_buf[24];
        if(m->codes[idx] >= 0)
            snprintf(row_buf, sizeof(row_buf), "%-6s %3u (0x%02X)",
                     FUNC_NAMES[idx], (uint8_t)m->codes[idx], (uint8_t)m->codes[idx]);
        else
            snprintf(row_buf, sizeof(row_buf), "%-6s  ---", FUNC_NAMES[idx]);
        canvas_draw_str(c, 4, (uint8_t)(30 + row * 10), row_buf);
    }

    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "[OK]/[<] Exit");
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Main draw dispatch                                               */
/* ══════════════════════════════════════════════════════════════════ */

static void draw_cb(Canvas* c, void* model) {
    AppModel* m = (AppModel*)model;
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);
    switch(m->state) {
    case AppStateWelcome:  draw_welcome(c, m);    break;
    case AppStateSending:  draw_sending(c, m);    break;
    case AppStateAsking:   draw_asking(c, m);     break;
    case AppStateSingle:   draw_single(c, m);     break;
    case AppStateFound:    draw_found(c, m);      break;
    case AppStateNotFound: draw_not_found(c, m);  break;
    case AppStateDone:     draw_done(c, m);       break;
    case AppStateSaved:    draw_saved(c, m);      break;
    }
}

/* ══════════════════════════════════════════════════════════════════ */
/*  IR worker thread                                                 */
/* ══════════════════════════════════════════════════════════════════ */

static int32_t ir_thread_fn(void* ctx) {
    App* app = (App*)ctx;
    uint8_t lo   = app->tlo;
    uint8_t hi   = app->thi;
    uint8_t addr = app->taddr;
    uint8_t mid  = (lo + hi) / 2;

    for(uint8_t cmd = lo; cmd <= mid; cmd++) {
        if(app->abort) break;
        app->t_prog = cmd;
        view_dispatcher_send_custom_event(app->vd, EvtIrProgress);
        InfraredMessage msg = {
            .protocol = InfraredProtocolSIRC,
            .address  = addr,
            .command  = cmd,
            .repeat   = false,
        };
        infrared_send(&msg, SEND_REPS);
        furi_delay_ms(INTER_CMD_MS);
    }

    view_dispatcher_send_custom_event(app->vd, EvtIrDone);
    return 0;
}

static void start_ir_thread(App* app, uint8_t lo, uint8_t hi, uint8_t addr) {
    app->abort  = false;
    app->tlo    = lo;
    app->thi    = hi;
    app->taddr  = addr;
    if(app->ir_thread) {
        furi_thread_join(app->ir_thread);
        furi_thread_free(app->ir_thread);
        app->ir_thread = NULL;
    }
    app->ir_thread = furi_thread_alloc_ex("ir_send", 1024, ir_thread_fn, app);
    furi_thread_start(app->ir_thread);
}

static void join_ir_thread(App* app) {
    if(app->ir_thread) {
        furi_thread_join(app->ir_thread);
        furi_thread_free(app->ir_thread);
        app->ir_thread = NULL;
    }
}

static void stop_ir_thread(App* app) {
    if(app->ir_thread) {
        app->abort = true;
        join_ir_thread(app);
    }
}

/* ══════════════════════════════════════════════════════════════════ */
/*  State transitions                                                */
/* ══════════════════════════════════════════════════════════════════ */

/* Call while holding the model lock; returns true if IR thread needed */
static bool advance_to_next(AppModel* m) {
    m->fi++;
    if(m->fi >= NUM_FUNCS) {
        m->state = AppStateDone;
        return false;
    }
    m->lo    = CMD_MIN;
    m->hi    = CMD_MAX;
    m->round = 1;
    m->state = AppStateSending;
    return true;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  File save (call WITHOUT holding the model lock)                 */
/* ══════════════════════════════════════════════════════════════════ */

typedef struct {
    int32_t codes[NUM_FUNCS];
    bool    done[NUM_FUNCS];
    uint8_t addr;
} SaveData;

static void save_ir_file(const SaveData* d) {
    Storage* st = (Storage*)furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(st, "/ext/infrared");

    File* f = storage_file_alloc(st);
    if(storage_file_open(f, OUTPUT_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        const char* hdr = "Filetype: IR signals file\nVersion: 1\n";
        storage_file_write(f, hdr, (uint16_t)strlen(hdr));

        for(uint8_t i = 0; i < NUM_FUNCS; i++) {
            if(!d->done[i] || d->codes[i] < 0) continue;
            char buf[128];
            int n = snprintf(
                buf, sizeof(buf),
                "\nname: %s\ntype: parsed\nprotocol: SIRC\n"
                "address: %02X 00 00 00\ncommand: %02X 00 00 00\n",
                FUNC_NAMES[i],
                (uint8_t)d->addr,
                (uint8_t)d->codes[i]);
            if(n > 0) storage_file_write(f, buf, (uint16_t)n);
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

/* Snapshot model data, release lock, then save (lock-safe) */
static void save_from_model(App* app) {
    SaveData snap;
    AppModel* m = (AppModel*)view_get_model(app->view);
    memcpy(snap.codes, m->codes, sizeof(snap.codes));
    memcpy(snap.done,  m->done,  sizeof(snap.done));
    snap.addr   = ADDR_OPTIONS[m->addr_idx];
    m->state    = AppStateSaved;        /* transition before releasing lock */
    view_commit_model(app->view, true);
    save_ir_file(&snap);               /* outside lock — can take time     */
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Input callback  (GUI thread → sets model, may start IR thread)  */
/* ══════════════════════════════════════════════════════════════════ */

static bool input_cb(InputEvent* ev, void* ctx) {
    App* app = (App*)ctx;
    if(ev->type != InputTypePress && ev->type != InputTypeRepeat) return false;

    AppModel* m   = (AppModel*)view_get_model(app->view);
    bool redraw   = false;
    bool handled  = true;
    bool do_start = false;

    switch(m->state) {

    /* ── Welcome ── */
    case AppStateWelcome:
        if(ev->key == InputKeyOk) {
            m->fi    = 0;
            m->lo    = CMD_MIN;
            m->hi    = CMD_MAX;
            m->round = 1;
            m->state = AppStateSending;
            redraw   = true;
            do_start = true;
        } else if(ev->key == InputKeyBack) {
            view_commit_model(app->view, false);
            view_dispatcher_stop(app->vd);
            return true;
        } else if(ev->key == InputKeyUp) {
            m->addr_idx = (uint8_t)((m->addr_idx + 1) % NUM_ADDR_OPTIONS);
            redraw = true;
        } else if(ev->key == InputKeyDown) {
            m->addr_idx = (uint8_t)((m->addr_idx + NUM_ADDR_OPTIONS - 1) % NUM_ADDR_OPTIONS);
            redraw = true;
        } else {
            handled = false;
        }
        break;

    /* ── Sending — only skip ── */
    case AppStateSending:
        if(ev->key == InputKeyBack) {
            app->abort      = true;
            m->done[m->fi]  = true;
            redraw = true;
        }
        break;

    /* ── Asking — yes / no / resend / skip ── */
    case AppStateAsking: {
        uint8_t mid = (m->lo + m->hi) / 2;
        if(ev->key == InputKeyUp) {
            m->hi = mid;
            m->round++;
            m->state = AppStateSending;
            redraw   = true;
            do_start = true;
        } else if(ev->key == InputKeyDown) {
            m->lo = (uint8_t)(mid + 1);
            m->round++;
            if(m->lo > m->hi) {
                m->codes[m->fi] = -1;
                m->done[m->fi]  = true;
                m->state = AppStateNotFound;
            } else {
                m->state = AppStateSending;
                do_start = true;
            }
            redraw = true;
        } else if(ev->key == InputKeyRight) {
            app->resending = true;
            m->state       = AppStateSending;
            redraw         = true;
            do_start       = true;
        } else if(ev->key == InputKeyBack) {
            m->done[m->fi] = true;
            bool need_thread = advance_to_next(m);
            redraw   = true;
            do_start = need_thread;
        } else {
            handled = false;
        }
        break;
    }

    /* ── Single — OK=yes, down=no, right=resend, back=skip ── */
    case AppStateSingle:
        if(ev->key == InputKeyOk) {
            m->codes[m->fi] = (int32_t)m->lo;
            m->done[m->fi]  = true;
            m->found_count++;
            m->anim  = 0;
            m->state = AppStateFound;
            redraw   = true;
        } else if(ev->key == InputKeyDown) {
            m->codes[m->fi] = -1;
            m->done[m->fi]  = true;
            m->state = AppStateNotFound;
            redraw   = true;
        } else if(ev->key == InputKeyRight) {
            app->resending = true;
            m->state       = AppStateSending;
            redraw         = true;
            do_start       = true;
        } else if(ev->key == InputKeyBack) {
            m->done[m->fi] = true;
            bool need_thread = advance_to_next(m);
            redraw   = true;
            do_start = need_thread;
        } else {
            handled = false;
        }
        break;

    /* ── Found — OK/back=next, right=test code again ── */
    case AppStateFound:
        if(ev->key == InputKeyRight) {
            app->resending  = true;
            m->state        = AppStateSending;
            redraw          = true;
            do_start        = true;
        } else if(ev->key == InputKeyOk || ev->key == InputKeyBack) {
            bool need_thread = advance_to_next(m);
            redraw   = true;
            do_start = need_thread;
        } else {
            handled = false;
        }
        break;

    /* ── Not found — OK/back=next ── */
    case AppStateNotFound:
        if(ev->key == InputKeyOk || ev->key == InputKeyBack) {
            bool need_thread = advance_to_next(m);
            redraw   = true;
            do_start = need_thread;
        } else {
            handled = false;
        }
        break;

    /* ── Done / Saved ── */
    case AppStateDone:
        handled = false;
        break;

    case AppStateSaved:
        if(ev->key == InputKeyOk || ev->key == InputKeyBack) {
            view_commit_model(app->view, false);
            view_dispatcher_stop(app->vd);
            return true;
        }
        break;
    }

    if(do_start) {
        uint8_t lo   = m->lo, hi = m->hi;
        uint8_t addr = ADDR_OPTIONS[m->addr_idx];
        view_commit_model(app->view, redraw);
        start_ir_thread(app, lo, hi, addr);
        return true;
    }

    view_commit_model(app->view, redraw);
    return handled;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Custom event callback  (ViewDispatcher event loop, main thread) */
/* ══════════════════════════════════════════════════════════════════ */

static bool custom_event_cb(void* ctx, uint32_t ev) {
    App* app = (App*)ctx;

    if(ev == EvtIrProgress) {
        AppModel* m = (AppModel*)view_get_model(app->view);
        m->send_prog = app->t_prog;
        view_commit_model(app->view, true);
        return true;
    }

    if(ev == EvtIrDone) {
        join_ir_thread(app);

        if(app->resending) {
            app->resending = false;
            AppModel* m = (AppModel*)view_get_model(app->view);
            if(m->lo == m->hi)
                m->state = AppStateSingle;
            else
                m->state = AppStateAsking;
            view_commit_model(app->view, true);
            return true;
        }

        AppModel* m = (AppModel*)view_get_model(app->view);

        if(m->state != AppStateSending) {
            view_commit_model(app->view, false);
            return true;
        }

        if(app->abort && m->done[m->fi]) {
            app->abort = false;
            bool need_thread = advance_to_next(m);
            if(need_thread) {
                uint8_t lo   = m->lo, hi = m->hi;
                uint8_t addr = ADDR_OPTIONS[m->addr_idx];
                view_commit_model(app->view, true);
                start_ir_thread(app, lo, hi, addr);
                return true;
            }

        } else if(m->lo == m->hi) {
            m->state = AppStateSingle;

        } else {
            m->state = AppStateAsking;
        }

        if(m->state == AppStateDone) {
            view_commit_model(app->view, true);
            save_from_model(app);
            return true;
        }

        view_commit_model(app->view, true);
    }

    return true;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Tick callback — animation + Done→save transition                */
/* ══════════════════════════════════════════════════════════════════ */

static void tick_cb(void* ctx) {
    App* app = (App*)ctx;
    AppModel* m = (AppModel*)view_get_model(app->view);
    m->anim++;

    if(m->state == AppStateDone) {
        view_commit_model(app->view, true);
        save_from_model(app);
        return;
    }

    view_commit_model(app->view, true);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Navigation callback — fallback exit                             */
/* ══════════════════════════════════════════════════════════════════ */

static bool nav_cb(void* ctx) {
    App* app = (App*)ctx;
    view_dispatcher_stop(app->vd);
    return true;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Entry point                                                      */
/* ══════════════════════════════════════════════════════════════════ */

int32_t sony_ir_search_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    /* View */
    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(AppModel));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, draw_cb);
    view_set_input_callback(app->view, input_cb);

    /* Initialise model */
    AppModel* m = (AppModel*)view_get_model(app->view);
    m->state    = AppStateWelcome;
    m->addr_idx = 1; /* default: ADDR_OPTIONS[1] = 1 (common Sony audio) */
    for(uint8_t i = 0; i < NUM_FUNCS; i++) m->codes[i] = -1;
    view_commit_model(app->view, false);

    /* ViewDispatcher */
    app->vd = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_custom_event_callback(app->vd, custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->vd, nav_cb);
    view_dispatcher_set_tick_event_callback(app->vd, tick_cb, ANIM_MS);
    view_dispatcher_add_view(app->vd, APP_VIEW, app->view);
    view_dispatcher_switch_to_view(app->vd, APP_VIEW);

    app->gui = (Gui*)furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    view_dispatcher_run(app->vd);

    /* Cleanup */
    stop_ir_thread(app);
    view_dispatcher_remove_view(app->vd, APP_VIEW);
    view_dispatcher_free(app->vd);
    view_free_model(app->view);
    view_free(app->view);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
