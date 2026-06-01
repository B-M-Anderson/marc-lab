/* Sony IR Finder v3.2 — binary-searches SIRC cmd space for Sony CMT-NE3
 * Protocol: SIRC-12 (7-bit cmd + 5-bit addr), 40kHz carrier handled by SDK
 *
 * Confirmed addresses (tablix.org brute-force + LIRC RM-S6/S61/S311):
 *   addr=1  : Power, Vol+, Vol-, CD-input, Tape, Tuner, Mute
 *   addr=17 : Play, Stop, Pause, Next, Prev  (Sony CD sub-device)
 *
 * Controls (context-sensitive):
 *   Welcome  : [OK] start  [<] exit
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
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════ */
/*  Constants                                                        */
/* ══════════════════════════════════════════════════════════════════ */

#define CMD_MIN         0
#define CMD_MAX         127
#define NUM_FUNCS       12
#define SEND_REPS       3       /* Sony spec: minimum 3 frames per code  */
#define INTER_CMD_MS    55      /* pause between successive codes        */
#define OUTPUT_PATH     "/ext/infrared/Sony_CMT_NE3_confirmed.ir"
#define RECORD_STORAGE  "storage"
#define ANIM_MS         60      /* animation tick period                 */
#define APP_VIEW        0

/* Per-function SIRC device addresses confirmed for CMT-NE3 */
static const char* const FUNC_NAMES[NUM_FUNCS] = {
    "Power",   "Vol+",  "Vol-",  "CD",
    "Tape",    "Tuner", "Mute",  "Play",
    "Stop",    "Pause", "Next",  "Prev",
};

static const uint8_t FUNC_ADDRS[NUM_FUNCS] = {
    1,  /* Power  */
    1,  /* Vol+   */
    1,  /* Vol-   */
    1,  /* CD     */
    1,  /* Tape   */
    1,  /* Tuner  */
    1,  /* Mute   */
    17, /* Play   — Sony CD sub-device */
    17, /* Stop   */
    17, /* Pause  */
    17, /* Next   */
    17, /* Prev   */
};

/* ══════════════════════════════════════════════════════════════════ */
/*  State machine                                                    */
/* ══════════════════════════════════════════════════════════════════ */

typedef enum {
    AppStateWelcome,
    AppStateSending,
    AppStateAsking,
    AppStateSingle,
    AppStateFound,
    AppStateNotFound,
    AppStateDone,
    AppStateSaved,
} AppState;

typedef enum {
    EvtIrProgress = 0,
    EvtIrDone     = 1,
} AppEvt;

/* ══════════════════════════════════════════════════════════════════ */
/*  View model  (ViewModelTypeLocking)                               */
/* ══════════════════════════════════════════════════════════════════ */

typedef struct {
    AppState state;
    uint8_t  fi;
    uint8_t  lo, hi;
    uint8_t  round;
    uint8_t  send_prog;
    uint8_t  anim;
    int32_t  codes[NUM_FUNCS];
    bool     done[NUM_FUNCS];
    uint8_t  found_count;
} AppModel;

/* ══════════════════════════════════════════════════════════════════ */
/*  App context                                                      */
/* ══════════════════════════════════════════════════════════════════ */

typedef struct {
    ViewDispatcher*  vd;
    View*            view;
    Gui*             gui;
    NotificationApp* notif;
    FuriThread*      ir_thread;

    uint8_t  tlo, thi, taddr;
    volatile uint8_t t_prog;
    volatile bool    abort;
    bool             resending;
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

static void draw_frac(Canvas* c, uint8_t done, uint8_t total) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%u/%u", done, total);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 126, 0, AlignRight, AlignTop, buf);
}

static void draw_header(Canvas* c, AppModel* m) {
    draw_progress_bar(c, m->found_count, NUM_FUNCS);
    draw_sep(c);
    draw_frac(c, m->found_count, NUM_FUNCS);
}

static void draw_ir_beam(Canvas* c, uint8_t x1, uint8_t x2, uint8_t y, uint8_t anim) {
    uint8_t span = x2 - x1;
    if(span == 0) return;
    for(uint8_t d = 0; d < 4; d++) {
        uint8_t phase = (uint8_t)(((uint16_t)anim * 2 + d * 16) % span);
        uint8_t x = x1 + phase;
        if(x <= x2) canvas_draw_disc(c, x, y, 1);
    }
}

static void draw_flipper(Canvas* c, uint8_t x, uint8_t y) {
    canvas_draw_rframe(c, x, y + 2, 10, 6, 1);
    canvas_draw_line(c, x + 5, y, x + 5, y + 2);
    canvas_draw_dot(c, x + 5, y);
}

static void draw_stereo(Canvas* c, uint8_t x, uint8_t y) {
    canvas_draw_frame(c, x, y, 14, 8);
    canvas_draw_disc(c, x + 4, y + 4, 2);
    canvas_draw_disc(c, x + 10, y + 4, 2);
    canvas_draw_line(c, x + 1, y + 1, x + 12, y + 1);
}

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

static uint32_t round_eta_seconds(uint8_t lo, uint8_t hi) {
    uint8_t mid = (lo + hi) / 2;
    uint32_t cmds = (uint32_t)(mid - lo + 1);
    return (cmds * 190u + 500u) / 1000u;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Per-state draw functions                                         */
/* ══════════════════════════════════════════════════════════════════ */

static void draw_welcome(Canvas* c, AppModel* m) {
    draw_header(c, m);

    bool inv = (m->anim / 12) % 2 == 0;
    canvas_set_font(c, FontPrimary);
    if(inv) {
        canvas_draw_box(c, 10, 10, 108, 12);
        canvas_set_color(c, ColorWhite);
    }
    canvas_draw_str_aligned(c, 64, 21, AlignCenter, AlignBottom, "SONY IR FINDER");
    canvas_set_color(c, ColorBlack);

    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 31, AlignCenter, AlignBottom,
        "SIRC-12 | addr 1 (sys) / 17 (CD)");

    draw_flipper(c, 8, 37);
    draw_stereo(c, 106, 37);
    draw_ir_beam(c, 22, 102, 41, m->anim);

    bool fill = (m->anim / 18) % 2 == 0;
    if(fill) {
        canvas_draw_rbox(c, 31, 52, 36, 10, 2);
        canvas_set_color(c, ColorWhite);
        canvas_draw_str_aligned(c, 49, 61, AlignCenter, AlignBottom, "[OK] Start");
        canvas_set_color(c, ColorBlack);
    } else {
        canvas_draw_rframe(c, 31, 52, 36, 10, 2);
        canvas_draw_str_aligned(c, 49, 61, AlignCenter, AlignBottom, "[OK] Start");
    }
    canvas_draw_str(c, 92, 62, "[<] Exit");
}

static void draw_sending(Canvas* c, AppModel* m) {
    draw_header(c, m);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 18, FUNC_NAMES[m->fi]);

    /* Show address badge next to function name */
    char addrbuf[10];
    snprintf(addrbuf, sizeof(addrbuf), "a=%u", FUNC_ADDRS[m->fi]);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 60, 18, addrbuf);

    char rnd[12];
    snprintf(rnd, sizeof(rnd), "Rnd %u/7", m->round);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignTop, rnd);

    uint8_t mid = (m->lo + m->hi) / 2;
    char rng[28];
    snprintf(rng, sizeof(rng), "Testing %u-%u", m->lo, mid);
    canvas_draw_str(c, 2, 28, rng);

    uint32_t eta = round_eta_seconds(m->lo, m->hi);
    char etastr[16];
    if(eta > 0) snprintf(etastr, sizeof(etastr), "~%lus", (unsigned long)eta);
    else snprintf(etastr, sizeof(etastr), "<1s");
    canvas_draw_str_aligned(c, 126, 19, AlignRight, AlignTop, etastr);

    draw_flipper(c, 2, 34);
    draw_stereo(c, 106, 34);
    draw_ir_beam(c, 16, 102, 38, m->anim);

    char prog[20];
    snprintf(prog, sizeof(prog), "Sending: %u", m->send_prog);
    canvas_draw_str(c, 2, 53, prog);

    canvas_draw_str(c, 2, 63, "[<] Skip");
}

static void draw_asking(Canvas* c, AppModel* m) {
    draw_header(c, m);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 18, FUNC_NAMES[m->fi]);

    char addrbuf[10];
    snprintf(addrbuf, sizeof(addrbuf), "a=%u", FUNC_ADDRS[m->fi]);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 60, 18, addrbuf);

    char rnd[12];
    snprintf(rnd, sizeof(rnd), "Rnd %u/7", m->round);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignTop, rnd);

    uint8_t mid = (m->lo + m->hi) / 2;
    char sent[30];
    snprintf(sent, sizeof(sent), "Sent codes %u - %u", m->lo, mid);
    canvas_draw_str_aligned(c, 64, 30, AlignCenter, AlignBottom, sent);

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

    char addrbuf[10];
    snprintf(addrbuf, sizeof(addrbuf), "a=%u", FUNC_ADDRS[m->fi]);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 60, 18, addrbuf);

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

    draw_checkmark(c, 108, 13, m->anim);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 55, 22, AlignCenter, AlignBottom, "FOUND!");

    char detail[32];
    snprintf(
        detail, sizeof(detail), "%s=%u(0x%02X) a=%u",
        FUNC_NAMES[m->fi],
        (uint8_t)m->codes[m->fi],
        (uint8_t)m->codes[m->fi],
        FUNC_ADDRS[m->fi]);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 34, AlignCenter, AlignBottom, detail);

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

    canvas_draw_line(c, 58, 42, 70, 54);
    canvas_draw_line(c, 70, 42, 58, 54);

    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "[OK] Next function");
}

static void draw_done(Canvas* c, AppModel* m) {
    draw_progress_bar(c, NUM_FUNCS, NUM_FUNCS);
    draw_sep(c);

    canvas_set_font(c, FontPrimary);
    char summary[32];
    snprintf(summary, sizeof(summary), "Done!  %u/%u found", m->found_count, NUM_FUNCS);
    canvas_draw_str_aligned(c, 64, 22, AlignCenter, AlignBottom, summary);

    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 34, AlignCenter, AlignBottom, "Saving remote file...");

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

    canvas_set_font(c, FontSecondary);
    uint8_t start = ((uint16_t)(m->anim / 40)) % NUM_FUNCS;
    for(uint8_t row = 0; row < 3; row++) {
        uint8_t idx = (start + row) % NUM_FUNCS;
        char row_buf[28];
        if(m->codes[idx] >= 0)
            snprintf(row_buf, sizeof(row_buf), "%-6s %3u(0x%02X) a=%u",
                     FUNC_NAMES[idx],
                     (uint8_t)m->codes[idx],
                     (uint8_t)m->codes[idx],
                     FUNC_ADDRS[idx]);
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
                (uint8_t)FUNC_ADDRS[i],
                (uint8_t)d->codes[i]);
            if(n > 0) storage_file_write(f, buf, (uint16_t)n);
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

static void save_from_model(App* app) {
    SaveData snap;
    AppModel* m = (AppModel*)view_get_model(app->view);
    memcpy(snap.codes, m->codes, sizeof(snap.codes));
    memcpy(snap.done,  m->done,  sizeof(snap.done));
    m->state = AppStateSaved;
    view_commit_model(app->view, true);
    save_ir_file(&snap);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Input callback                                                   */
/* ══════════════════════════════════════════════════════════════════ */

static bool input_cb(InputEvent* ev, void* ctx) {
    App* app = (App*)ctx;
    if(ev->type != InputTypePress && ev->type != InputTypeRepeat) return false;

    AppModel* m   = (AppModel*)view_get_model(app->view);
    bool redraw   = false;
    bool handled  = true;
    bool do_start = false;

    switch(m->state) {

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
        } else {
            handled = false;
        }
        break;

    case AppStateSending:
        if(ev->key == InputKeyBack) {
            app->abort      = true;
            m->done[m->fi]  = true;
            redraw = true;
        }
        break;

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

    case AppStateNotFound:
        if(ev->key == InputKeyOk || ev->key == InputKeyBack) {
            bool need_thread = advance_to_next(m);
            redraw   = true;
            do_start = need_thread;
        } else {
            handled = false;
        }
        break;

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
        uint8_t addr = FUNC_ADDRS[m->fi];
        view_commit_model(app->view, redraw);
        start_ir_thread(app, lo, hi, addr);
        return true;
    }

    view_commit_model(app->view, redraw);
    return handled;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Custom event callback                                            */
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
            m->state = (m->lo == m->hi) ? AppStateSingle : AppStateAsking;
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
                uint8_t addr = FUNC_ADDRS[m->fi];
                view_commit_model(app->view, true);
                start_ir_thread(app, lo, hi, addr);
                return true;
            }

        } else if(m->lo == m->hi) {
            m->state = AppStateSingle;
            notification_message(app->notif, &sequence_single_vibro);

        } else {
            m->state = AppStateAsking;
            notification_message(app->notif, &sequence_single_vibro);
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
/*  Tick callback                                                    */
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
/*  Navigation callback                                              */
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

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(AppModel));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, draw_cb);
    view_set_input_callback(app->view, input_cb);

    AppModel* m = (AppModel*)view_get_model(app->view);
    m->state = AppStateWelcome;
    for(uint8_t i = 0; i < NUM_FUNCS; i++) m->codes[i] = -1;
    view_commit_model(app->view, false);

    app->vd = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_custom_event_callback(app->vd, custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->vd, nav_cb);
    view_dispatcher_set_tick_event_callback(app->vd, tick_cb, ANIM_MS);
    view_dispatcher_add_view(app->vd, APP_VIEW, app->view);
    view_dispatcher_switch_to_view(app->vd, APP_VIEW);

    app->gui   = (Gui*)furi_record_open(RECORD_GUI);
    app->notif = (NotificationApp*)furi_record_open(RECORD_NOTIFICATION);
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    view_dispatcher_run(app->vd);

    stop_ir_thread(app);
    view_dispatcher_remove_view(app->vd, APP_VIEW);
    view_dispatcher_free(app->vd);
    view_free_model(app->view);
    view_free(app->view);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
