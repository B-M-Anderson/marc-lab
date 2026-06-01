/* Sony IR Search — binary-searches SIRC cmd space for Sony CMT-NE3
 * Protocol: SIRC 12-bit, address = 1, command range 0-127
 * Controls: OK/Up/Down/Back — see each screen's bottom row
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

#define SIRC_ADDR       1
#define CMD_MIN         0
#define CMD_MAX         127
#define NUM_FUNCS       12
#define SEND_REPS       3       /* IR repetitions per command code */
#define INTER_CMD_MS    55      /* pause between successive codes   */
#define OUTPUT_PATH     "/ext/infrared/Sony_CMT_NE3_confirmed.ir"
#define RECORD_STORAGE  "storage"
#define ANIM_PERIOD_MS  60      /* tick period for animation        */
#define APP_VIEW        0

/* ══════════════════════════════════════════════════════════════════ */
/*  State machine                                                    */
/* ══════════════════════════════════════════════════════════════════ */

typedef enum {
    AppStateWelcome,   /* title screen */
    AppStateSending,   /* IR thread active, blasting a range */
    AppStateAsking,    /* range question: ^Yes  vNo  <Skip  */
    AppStateSingle,    /* final 1-cmd question: OKYes  vNo  */
    AppStateFound,     /* confirmed code — brief celebration */
    AppStateNotFound,  /* no code found for this function    */
    AppStateDone,      /* all funcs done, saving file        */
    AppStateSaved,     /* file written, show path + exit     */
} AppState;

/* Custom events posted from IR thread → ViewDispatcher */
typedef enum {
    EvtIrProgress = 0,
    EvtIrDone     = 1,
} AppEvt;

/* ══════════════════════════════════════════════════════════════════ */
/*  Data tables                                                      */
/* ══════════════════════════════════════════════════════════════════ */

static const char* const FUNC_NAMES[NUM_FUNCS] = {
    "Power",   "Vol+",  "Vol-",  "CD",
    "Tape",    "Tuner", "Mute",  "Play",
    "Stop",    "Pause", "Next",  "Prev",
};

/* ══════════════════════════════════════════════════════════════════ */
/*  View model  (mutex-locked: written from IR thread + main)       */
/* ══════════════════════════════════════════════════════════════════ */

typedef struct {
    AppState state;
    uint8_t  fi;            /* current function index 0-11 */
    uint8_t  lo, hi;        /* binary-search bounds (inclusive) */
    uint8_t  round;         /* 1-based round counter */
    uint8_t  send_prog;     /* command currently being sent */
    uint8_t  anim;          /* 0-255 wrapping, incremented by tick */
    int32_t  codes[NUM_FUNCS];   /* -1 = skipped / not found */
    bool     done[NUM_FUNCS];
    uint8_t  found_count;
    bool     saving;        /* true while writing file */
} AppModel;

/* ══════════════════════════════════════════════════════════════════ */
/*  App context                                                      */
/* ══════════════════════════════════════════════════════════════════ */

typedef struct {
    ViewDispatcher* vd;
    View*           view;
    Gui*            gui;
    FuriThread*     ir_thread;
    /* Written by main before thread start; read by thread */
    uint8_t  tlo, thi;
    /* Written by IR thread atomically; read by main via event */
    volatile uint8_t t_prog;
    volatile bool    abort;
} App;

/* ══════════════════════════════════════════════════════════════════ */
/*  Drawing helpers                                                  */
/* ══════════════════════════════════════════════════════════════════ */

/* Top progress bar: shows completed functions / 12 */
static void draw_progress(Canvas* c, uint8_t done, uint8_t total) {
    /* outer frame */
    canvas_draw_frame(c, 0, 0, 128, 6);
    /* filled portion */
    if(done > 0) {
        uint8_t w = (uint8_t)((uint16_t)126 * done / total);
        canvas_draw_box(c, 1, 1, w, 4);
    }
    /* fraction text on right — tight space, so only if done>0 */
    char frac[8];
    snprintf(frac, sizeof(frac), "%u/%u", done, total);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 127, 1, AlignRight, AlignTop, frac);
}

/* Thin separator line below the progress bar */
static void draw_sep(Canvas* c) {
    canvas_draw_line(c, 0, 7, 127, 7);
}

/* Bottom row control hint — always drawn in FontSecondary at y=62 */
static void draw_hint(Canvas* c, const char* hint) {
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 62, AlignCenter, AlignBottom, hint);
}

/* Animated IR-beam between x1..x2 at y, anim drives phase */
static void draw_ir_beam(Canvas* c, uint8_t x1, uint8_t x2, uint8_t y, uint8_t anim) {
    uint8_t span = x2 - x1;
    /* 4 dots equally spaced, phase-shifted by anim */
    for(int d = 0; d < 4; d++) {
        uint8_t phase = (uint8_t)(((uint16_t)anim * 2 + d * 16) % (span > 0 ? span : 1));
        uint8_t x = x1 + phase;
        if(x >= x1 && x <= x2) canvas_draw_disc(c, x, y, 1);
    }
}

/* Small Flipper silhouette (10×8) at (x,y) */
static void draw_flipper(Canvas* c, uint8_t x, uint8_t y) {
    canvas_draw_rframe(c, x, y + 2, 10, 6, 1);   /* body */
    canvas_draw_line(c, x + 5, y, x + 5, y + 2); /* antenna */
    canvas_draw_dot(c, x + 5, y);
}

/* Small stereo silhouette (14×8) at (x,y) */
static void draw_stereo(Canvas* c, uint8_t x, uint8_t y) {
    canvas_draw_frame(c, x, y, 14, 8);            /* chassis */
    canvas_draw_disc(c, x + 4, y + 4, 2);         /* left speaker */
    canvas_draw_disc(c, x + 10, y + 4, 2);        /* right speaker */
    canvas_draw_line(c, x + 1, y + 1, x + 12, y + 1); /* top detail */
}

/* Animated checkmark — draws progressively as anim goes 0→31 */
static void draw_checkmark(Canvas* c, uint8_t cx, uint8_t cy, uint8_t anim) {
    uint8_t progress = anim > 31 ? 31 : anim;
    /* tick: short stroke down-right then long stroke up-right */
    /* first half (0-10): down stroke */
    if(progress > 0) {
        uint8_t p1 = progress > 10 ? 10 : progress;
        canvas_draw_line(c, cx, cy, cx + p1 / 2, cy + p1 / 2);
    }
    /* second half (10-31): long up-right stroke */
    if(progress > 10) {
        uint8_t p2 = progress - 10;
        canvas_draw_line(c, cx + 5, cy + 5, cx + 5 + p2, cy + 5 - p2 / 2);
    }
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Per-state draw routines                                          */
/* ══════════════════════════════════════════════════════════════════ */

static void draw_welcome(Canvas* c, AppModel* m) {
    draw_progress(c, m->found_count, NUM_FUNCS);
    draw_sep(c);

    /* Pulsing title */
    canvas_set_font(c, FontPrimary);
    /* XOR blink on even frames — gives a subtle shimmer */
    if((m->anim / 8) % 2 == 0) canvas_set_color(c, ColorBlack);
    else canvas_set_color(c, ColorBlack); /* keep solid, use XOR on box */
    canvas_draw_str_aligned(c, 64, 20, AlignCenter, AlignBottom, "SONY IR SEARCH");
    canvas_set_color(c, ColorBlack);

    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 30, AlignCenter, AlignBottom, "12 Functions  |  SIRC addr=1");

    /* Animated IR scene */
    draw_flipper(c, 8, 37);
    draw_stereo(c, 106, 37);
    draw_ir_beam(c, 22, 102, 41, m->anim);

    /* Pulsing OK button hint */
    if((m->anim / 16) % 2 == 0) {
        canvas_draw_rbox(c, 32, 53, 34, 9, 2);
        canvas_set_color(c, ColorWhite);
        canvas_draw_str_aligned(c, 49, 62, AlignCenter, AlignBottom, "[OK] Start");
        canvas_set_color(c, ColorBlack);
    } else {
        canvas_draw_rframe(c, 32, 53, 34, 9, 2);
        canvas_draw_str_aligned(c, 49, 62, AlignCenter, AlignBottom, "[OK] Start");
    }
    canvas_draw_str_aligned(c, 106, 62, AlignCenter, AlignBottom, "[<] Exit");
}

static void draw_sending(Canvas* c, AppModel* m) {
    draw_progress(c, m->found_count, NUM_FUNCS);
    draw_sep(c);

    /* Function name + round */
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 18, FUNC_NAMES[m->fi]);
    canvas_set_font(c, FontSecondary);
    char rnd[16];
    snprintf(rnd, sizeof(rnd), "Rnd %u/7", m->round);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignTop, rnd);

    /* Range indicator */
    uint8_t mid = (m->lo + m->hi) / 2;
    char rng[28];
    snprintf(rng, sizeof(rng), "Testing codes %u-%u", m->lo, mid);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 28, rng);

    /* Animated IR scene (compact) */
    draw_flipper(c, 2, 34);
    draw_stereo(c, 106, 34);
    draw_ir_beam(c, 16, 102, 38, m->anim);

    /* Current code */
    char prog[22];
    snprintf(prog, sizeof(prog), "Sending: %u", m->send_prog);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 52, prog);

    draw_hint(c, "[<] Skip function");
}

static void draw_asking(Canvas* c, AppModel* m) {
    draw_progress(c, m->found_count, NUM_FUNCS);
    draw_sep(c);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 18, FUNC_NAMES[m->fi]);
    canvas_set_font(c, FontSecondary);
    char rnd[16];
    snprintf(rnd, sizeof(rnd), "Rnd %u/7", m->round);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignTop, rnd);

    uint8_t mid = (m->lo + m->hi) / 2;
    char sent[30];
    snprintf(sent, sizeof(sent), "Sent codes %u - %u", m->lo, mid);
    canvas_draw_str_aligned(c, 64, 30, AlignCenter, AlignBottom, sent);

    /* Big question — slightly animated (XOR blink on question mark) */
    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 44, AlignCenter, AlignBottom, "Did stereo react?");

    /* Button row */
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 4,  60, "[^] Yes");
    canvas_draw_str(c, 50, 60, "[v] No");
    canvas_draw_str(c, 92, 60, "[<] Skip");
}

static void draw_single(Canvas* c, AppModel* m) {
    draw_progress(c, m->found_count, NUM_FUNCS);
    draw_sep(c);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 18, FUNC_NAMES[m->fi]);

    /* Show the exact code being tested */
    char codestr[24];
    snprintf(codestr, sizeof(codestr), "Code: %u  (0x%02X)", m->lo, m->lo);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 30, AlignCenter, AlignBottom, codestr);

    /* Pulse animation around the code box */
    uint8_t p = (m->anim / 4) % 4;
    canvas_draw_rframe(c, 20 - p, 20 - p, 88 + p * 2, 14 + p * 2, 3);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 45, AlignCenter, AlignBottom, "Did it work?");

    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2,  62, "[OK] Yes");
    canvas_draw_str(c, 50, 62, "[v] No");
    canvas_draw_str(c, 94, 62, "[<] Skip");
}

static void draw_found(Canvas* c, AppModel* m) {
    draw_progress(c, m->found_count, NUM_FUNCS);
    draw_sep(c);

    /* Checkmark animation */
    draw_checkmark(c, 100, 14, m->anim);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 55, 22, AlignCenter, AlignBottom, "FOUND!");

    char detail[28];
    snprintf(
        detail, sizeof(detail), "%s = %u (0x%02X)",
        FUNC_NAMES[m->fi],
        (uint8_t)m->codes[m->fi],
        (uint8_t)m->codes[m->fi]);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 36, AlignCenter, AlignBottom, detail);

    /* Starburst effect using lines from center */
    uint8_t bx = 64, by = 46;
    uint8_t r = 6 + (m->anim % 4);
    canvas_draw_circle(c, bx, by, r);
    if(m->anim % 8 < 4) canvas_draw_disc(c, bx, by, r / 2);

    draw_hint(c, "[OK] Next function");
}

static void draw_not_found(Canvas* c, AppModel* m) {
    draw_progress(c, m->found_count, NUM_FUNCS);
    draw_sep(c);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 24, AlignCenter, AlignBottom, "Not Found");

    canvas_set_font(c, FontSecondary);
    char line[32];
    snprintf(line, sizeof(line), "%s: no code identified", FUNC_NAMES[m->fi]);
    canvas_draw_str_aligned(c, 64, 38, AlignCenter, AlignBottom, line);

    /* Sad X */
    uint8_t cx = 64, cy = 48;
    canvas_draw_line(c, cx - 5, cy - 5, cx + 5, cy + 5);
    canvas_draw_line(c, cx + 5, cy - 5, cx - 5, cy + 5);

    draw_hint(c, "[OK] Next function");
}

static void draw_done(Canvas* c, AppModel* m) {
    draw_progress(c, m->found_count, NUM_FUNCS);
    draw_sep(c);

    canvas_set_font(c, FontPrimary);
    char summary[28];
    snprintf(summary, sizeof(summary), "Done!  %u/%u codes found", m->found_count, NUM_FUNCS);
    canvas_draw_str_aligned(c, 64, 22, AlignCenter, AlignBottom, summary);

    if(m->saving) {
        canvas_set_font(c, FontSecondary);
        canvas_draw_str_aligned(c, 64, 34, AlignCenter, AlignBottom, "Writing .ir file...");
        /* Animated progress dots */
        uint8_t dots = (m->anim / 8) % 4;
        char dotstr[5] = "    ";
        for(uint8_t i = 0; i < dots; i++) dotstr[i] = '.';
        canvas_draw_str_aligned(c, 64, 46, AlignCenter, AlignBottom, dotstr);
    }
}

static void draw_saved(Canvas* c, AppModel* m) {
    UNUSED(m);
    draw_progress(c, NUM_FUNCS, NUM_FUNCS);
    draw_sep(c);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 20, AlignCenter, AlignBottom, "Remote Saved!");

    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 32, AlignCenter, AlignBottom, "Sony_CMT_NE3_confirmed.ir");
    canvas_draw_str_aligned(c, 64, 42, AlignCenter, AlignBottom, "saved to /ext/infrared/");
    canvas_draw_str_aligned(c, 64, 52, AlignCenter, AlignBottom, "Load it in the IR Remote app");

    draw_hint(c, "[<] Exit");
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Main draw callback (dispatches to per-state functions)          */
/* ══════════════════════════════════════════════════════════════════ */

static void draw_cb(Canvas* c, void* ctx) {
    AppModel* m = (AppModel*)ctx;
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
/*  State transition helpers (called with model lock held)          */
/* ══════════════════════════════════════════════════════════════════ */


static void advance_to_next(AppModel* m) {
    m->fi++;
    if(m->fi >= NUM_FUNCS) {
        m->saving = true;
        m->state  = AppStateDone;
    } else {
        m->lo    = CMD_MIN;
        m->hi    = CMD_MAX;
        m->round = 1;
        m->state = AppStateSending;
    }
}

/* ══════════════════════════════════════════════════════════════════ */
/*  IR worker thread                                                 */
/* ══════════════════════════════════════════════════════════════════ */

static int32_t ir_thread_fn(void* ctx) {
    App* app = (App*)ctx;
    uint8_t lo = app->tlo;
    uint8_t hi = app->thi;
    uint8_t mid = (lo + hi) / 2;

    for(uint8_t cmd = lo; cmd <= mid; cmd++) {
        if(app->abort) break;

        app->t_prog = cmd;
        view_dispatcher_send_custom_event(app->vd, EvtIrProgress);

        InfraredMessage msg = {
            .protocol = InfraredProtocolSIRC,
            .address  = SIRC_ADDR,
            .command  = cmd,
            .repeat   = false,
        };
        infrared_send(&msg, SEND_REPS);
        furi_delay_ms(INTER_CMD_MS);
    }

    view_dispatcher_send_custom_event(app->vd, EvtIrDone);
    return 0;
}

static void start_ir_thread(App* app, uint8_t lo, uint8_t hi) {
    app->abort = false;
    app->tlo   = lo;
    app->thi   = hi;
    if(app->ir_thread) {
        furi_thread_join(app->ir_thread);
        furi_thread_free(app->ir_thread);
    }
    app->ir_thread = furi_thread_alloc_ex("ir_send", 1024, ir_thread_fn, app);
    furi_thread_start(app->ir_thread);
}

static void stop_ir_thread(App* app) {
    if(app->ir_thread) {
        app->abort = true;
        furi_thread_join(app->ir_thread);
        furi_thread_free(app->ir_thread);
        app->ir_thread = NULL;
    }
}

/* ══════════════════════════════════════════════════════════════════ */
/*  File save                                                        */
/* ══════════════════════════════════════════════════════════════════ */

static void save_ir_file(AppModel* m) {
    Storage* st = (Storage*)furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(st, "/ext/infrared");

    File* f = storage_file_alloc(st);
    if(storage_file_open(f, OUTPUT_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        const char* hdr = "Filetype: IR signals file\nVersion: 1\n";
        storage_file_write(f, hdr, (uint16_t)strlen(hdr));

        for(uint8_t i = 0; i < NUM_FUNCS; i++) {
            if(!m->done[i] || m->codes[i] < 0) continue;
            char buf[128];
            int n = snprintf(
                buf, sizeof(buf),
                "\nname: %s\ntype: parsed\nprotocol: SIRC\n"
                "address: %02X 00 00 00\ncommand: %02X 00 00 00\n",
                FUNC_NAMES[i],
                (uint8_t)SIRC_ADDR,
                (uint8_t)m->codes[i]);
            if(n > 0) storage_file_write(f, buf, (uint16_t)n);
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Input callback (runs on GUI thread — just post to dispatcher)   */
/* ══════════════════════════════════════════════════════════════════ */

static bool input_cb(InputEvent* ev, void* ctx) {
    App* app = (App*)ctx;
    if(ev->type != InputTypePress && ev->type != InputTypeRepeat) return false;

    AppModel* m = (AppModel*)view_get_model(app->view);
    bool redraw = false;
    bool handled = true;

    switch(m->state) {

    /* ── Welcome ── */
    case AppStateWelcome:
        if(ev->key == InputKeyOk) {
            m->state = AppStateSending;
            redraw   = true;
            /* start IR thread after releasing model lock */
        } else if(ev->key == InputKeyBack) {
            view_dispatcher_stop(app->vd);
        }
        break;

    /* ── Sending: only Back = skip ── */
    case AppStateSending:
        if(ev->key == InputKeyBack) {
            app->abort    = true;       /* IR thread sees this */
            m->done[m->fi] = true;      /* mark skipped */
            redraw = true;
            /* IR thread will post EvtIrDone soon; we advance in that handler */
        }
        break;

    /* ── Asking: Up=yes Down=no Back=skip ── */
    case AppStateAsking: {
        uint8_t mid = (m->lo + m->hi) / 2;
        if(ev->key == InputKeyUp) {
            m->hi = mid;           /* code in lo..mid */
            m->round++;
        } else if(ev->key == InputKeyDown) {
            m->lo = mid + 1;       /* code in mid+1..hi */
            m->round++;
        } else if(ev->key == InputKeyBack) {
            m->done[m->fi] = true; /* skip */
            advance_to_next(m);
            redraw = true;
            break;
        } else {
            handled = false; break;
        }

        if(m->lo == m->hi) {
            /* narrowed to single — send it and ask explicitly */
            m->state = AppStateSending;
        } else if(m->lo > m->hi) {
            /* shouldn't happen but guard anyway */
            m->done[m->fi] = true;
            advance_to_next(m);
        } else {
            m->state = AppStateSending;
        }
        redraw = true;
        break;
    }

    /* ── Single: OK=yes Down/Back=no ── */
    case AppStateSingle:
        if(ev->key == InputKeyOk) {
            m->codes[m->fi] = (int32_t)m->lo;
            m->done[m->fi]  = true;
            m->found_count++;
            m->state = AppStateFound;
            m->anim  = 0; /* restart checkmark anim */
            redraw   = true;
        } else if(ev->key == InputKeyDown || ev->key == InputKeyBack) {
            /* This specific code didn't work */
            m->codes[m->fi] = -1;
            m->done[m->fi]  = true;
            m->state = AppStateNotFound;
            redraw   = true;
        }
        break;

    /* ── Found / NotFound: OK or Back advances ── */
    case AppStateFound:
    case AppStateNotFound:
        if(ev->key == InputKeyOk || ev->key == InputKeyBack) {
            advance_to_next(m);
            redraw = true;
        }
        break;

    /* ── Done: shouldn't get input here normally ── */
    case AppStateDone:
        handled = false;
        break;

    /* ── Saved: Back exits ── */
    case AppStateSaved:
        if(ev->key == InputKeyBack || ev->key == InputKeyOk) {
            view_dispatcher_stop(app->vd);
        }
        break;
    }

    /* If we just set state = AppStateSending, fire off the IR thread */
    if(m->state == AppStateSending && redraw) {
        /* only start a new thread if the previous one isn't already running
         * (the abort from Back in Sending state will let the existing thread
         * finish via EvtIrDone, which handles the skip — we don't start a new
         * one here in that case) */
        if(!app->abort) {
            uint8_t lo = m->lo, hi = m->hi;
            view_commit_model(app->view, true);
            start_ir_thread(app, lo, hi);
            return true;
        }
    }

    view_commit_model(app->view, redraw);
    return handled;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Custom event callback (called on main thread by ViewDispatcher) */
/* ══════════════════════════════════════════════════════════════════ */

static bool custom_event_cb(void* ctx, uint32_t ev) {
    App* app = (App*)ctx;
    AppModel* m = (AppModel*)view_get_model(app->view);
    bool redraw = false;

    if(ev == EvtIrProgress) {
        m->send_prog = app->t_prog;
        redraw = true;

    } else if(ev == EvtIrDone) {
        /* join the thread so it's cleaned up */
        if(app->ir_thread) {
            furi_thread_join(app->ir_thread);
            furi_thread_free(app->ir_thread);
            app->ir_thread = NULL;
        }

        if(m->state == AppStateSending) {
            if(app->abort && m->done[m->fi]) {
                /* user pressed Back during sending — skip was already set */
                app->abort = false;
                advance_to_next(m);
            } else if(m->lo == m->hi) {
                /* single code — ask specifically */
                m->state = AppStateSingle;
            } else {
                /* show the range question */
                m->state = AppStateAsking;
            }
            redraw = true;
        }

        /* If we just transitioned to Done, save the file */
        if(m->state == AppStateDone) {
            view_commit_model(app->view, true);
            save_ir_file(m);
            m = (AppModel*)view_get_model(app->view);
            m->saving = false;
            m->state  = AppStateSaved;
            redraw    = true;
        }
    }

    view_commit_model(app->view, redraw);
    return true;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Tick callback — drives animation counter                        */
/* ══════════════════════════════════════════════════════════════════ */

static void tick_cb(void* ctx) {
    App* app = (App*)ctx;
    AppModel* m = (AppModel*)view_get_model(app->view);
    m->anim++;

    /* If we just entered AppStateDone via advance_to_next (not from EvtIrDone),
     * save the file here on the first tick */
    if(m->state == AppStateDone && m->saving) {
        view_commit_model(app->view, true);
        save_ir_file(m);
        m = (AppModel*)view_get_model(app->view);
        m->saving = false;
        m->state  = AppStateSaved;
    }

    view_commit_model(app->view, true);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Navigation callback — called if no view handles Back            */
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

    /* View + model */
    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(AppModel));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, draw_cb);
    view_set_input_callback(app->view, input_cb);

    /* Initialise model */
    AppModel* m = (AppModel*)view_get_model(app->view);
    m->state = AppStateWelcome;
    for(uint8_t i = 0; i < NUM_FUNCS; i++) {
        m->codes[i] = -1;
        m->done[i]  = false;
    }
    view_commit_model(app->view, false);

    /* ViewDispatcher */
    app->vd = view_dispatcher_alloc();
    view_dispatcher_set_custom_event_callback(app->vd, custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->vd, nav_cb);
    view_dispatcher_set_tick_event_callback(app->vd, tick_cb, ANIM_PERIOD_MS);
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_add_view(app->vd, APP_VIEW, app->view);
    view_dispatcher_switch_to_view(app->vd, APP_VIEW);

    /* Attach to GUI */
    app->gui = (Gui*)furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    /* Run — blocks until view_dispatcher_stop() */
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
