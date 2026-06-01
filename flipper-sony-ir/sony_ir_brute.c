#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <lib/infrared/worker/infrared_transmit.h>
#include <lib/infrared/encoder_decoder/infrared.h>
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>

#define SIRC_ADDRESS   1
#define CMD_MIN        0
#define CMD_MAX        127
#define NUM_FUNCTIONS  12
#define SEND_REPEATS   3
#define OUTPUT_PATH    "/ext/infrared/Sony_CMT_NE3_confirmed.ir"
#define RECORD_STORAGE "storage"

typedef enum {
    AppStateIntro,
    AppStateSending,
    AppStateAsking,
    AppStateFound,
    AppStateDone,
    AppStateSaved,
} AppState;

typedef struct {
    const char* name;
    int32_t code;
    bool done;
} IrFunction;

typedef struct {
    AppState state;
    IrFunction funcs[NUM_FUNCTIONS];
    uint8_t current_func;
    uint8_t lo;
    uint8_t hi;
    uint8_t round;
    uint8_t send_progress;
    bool skip_requested;
    FuriMessageQueue* queue;
    ViewPort* view_port;
    Gui* gui;
} AppContext;

static const char* const FUNC_NAMES[NUM_FUNCTIONS] = {
    "Power",    "Vol Up",   "Vol Down", "CD",
    "Tape",     "Tuner",    "Mute",     "Play",
    "Stop",     "Pause",    "Skip Fwd", "Skip Bck",
};

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_progress_bar(Canvas* canvas, uint8_t done, uint8_t total) {
    uint8_t w = canvas_width(canvas);
    canvas_draw_frame(canvas, 0, 0, w, 5);
    if(done > 0) {
        uint8_t fill = (uint8_t)((uint16_t)w * done / total);
        if(fill > 0) canvas_draw_box(canvas, 0, 0, fill, 5);
    }
}

static void draw_callback(Canvas* canvas, void* ctx) {
    AppContext* app = (AppContext*)ctx;
    canvas_clear(canvas);

    uint8_t done_count = 0;
    for(uint8_t i = 0; i < NUM_FUNCTIONS; i++) {
        if(app->funcs[i].done) done_count++;
    }
    draw_progress_bar(canvas, done_count, NUM_FUNCTIONS);

    char line[40];

    switch(app->state) {
    case AppStateIntro:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter, "Sony IR BruteForce");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "SIRC addr=1 cmds 0-127");
        canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignCenter, "[OK] Start  [<] Exit");
        break;

    case AppStateSending: {
        uint8_t mid = (app->lo + app->hi) / 2;
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 16, app->funcs[app->current_func].name);
        canvas_set_font(canvas, FontSecondary);
        snprintf(line, sizeof(line), "Rnd %u  Range %u-%u", app->round, app->lo, app->hi);
        canvas_draw_str(canvas, 2, 27, line);
        snprintf(line, sizeof(line), "Sending %u..%u", app->lo, mid);
        canvas_draw_str(canvas, 2, 38, line);
        snprintf(line, sizeof(line), "Cmd: %u", app->send_progress);
        canvas_draw_str(canvas, 2, 49, line);
        canvas_draw_str(canvas, 2, 62, "[<] Skip function");
        break;
    }

    case AppStateAsking: {
        uint8_t mid = (app->lo + app->hi) / 2;
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 16, app->funcs[app->current_func].name);
        canvas_set_font(canvas, FontSecondary);
        snprintf(line, sizeof(line), "Sent cmds %u-%u", app->lo, mid);
        canvas_draw_str(canvas, 2, 28, line);
        canvas_draw_str(canvas, 2, 40, "Did stereo respond?");
        canvas_draw_str(canvas, 2, 52, "[^]Yes  [v]No  [<]Skip");
        break;
    }

    case AppStateFound: {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 16, app->funcs[app->current_func].name);
        canvas_set_font(canvas, FontSecondary);
        snprintf(line, sizeof(line), "Found! Code: %u (0x%02X)",
                 (uint8_t)app->funcs[app->current_func].code,
                 (uint8_t)app->funcs[app->current_func].code);
        canvas_draw_str(canvas, 2, 32, line);
        canvas_draw_str(canvas, 2, 50, "[OK] Next function");
        break;
    }

    case AppStateDone:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "All Done!");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Saving .ir file...");
        break;

    case AppStateSaved:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignCenter, "Saved!");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Sony_CMT_NE3_confirmed.ir");
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, "in /ext/infrared/");
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "[<] Exit");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Input callback (GUI thread → main thread via queue)                */
/* ------------------------------------------------------------------ */

static void input_callback(InputEvent* event, void* ctx) {
    AppContext* app = (AppContext*)ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

/* ------------------------------------------------------------------ */
/* File save                                                           */
/* ------------------------------------------------------------------ */

static void save_ir_file(AppContext* app) {
    Storage* storage = (Storage*)furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, "/ext/infrared");

    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, OUTPUT_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        const char* hdr = "Filetype: IR signals file\nVersion: 1\n";
        storage_file_write(file, hdr, (uint16_t)strlen(hdr));

        char buf[128];
        int len;
        for(uint8_t i = 0; i < NUM_FUNCTIONS; i++) {
            if(!app->funcs[i].done) continue;
            if(app->funcs[i].code < 0) continue;

            len = snprintf(
                buf,
                sizeof(buf),
                "\nname: %s\ntype: parsed\nprotocol: SIRC\n"
                "address: %02X 00 00 00\ncommand: %02X 00 00 00\n",
                app->funcs[i].name,
                (uint8_t)SIRC_ADDRESS,
                (uint8_t)app->funcs[i].code);
            if(len > 0) storage_file_write(file, buf, (uint16_t)len);
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* ------------------------------------------------------------------ */
/* State helpers                                                       */
/* ------------------------------------------------------------------ */

static void begin_function(AppContext* app) {
    app->lo = CMD_MIN;
    app->hi = CMD_MAX;
    app->round = 1;
    app->skip_requested = false;
    app->state = AppStateSending;
    view_port_update(app->view_port);
}

static void advance_function(AppContext* app) {
    app->current_func++;
    if(app->current_func >= NUM_FUNCTIONS) {
        app->state = AppStateDone;
    } else {
        begin_function(app);
    }
    view_port_update(app->view_port);
}

/* ------------------------------------------------------------------ */
/* IR send phase — runs in main thread, blocking                       */
/* ------------------------------------------------------------------ */

static void do_send(AppContext* app) {
    uint8_t mid = (app->lo + app->hi) / 2;

    for(uint8_t cmd = app->lo; cmd <= mid; cmd++) {
        app->send_progress = cmd;
        view_port_update(app->view_port);

        InfraredMessage msg = {
            .protocol = InfraredProtocolSIRC,
            .address = SIRC_ADDRESS,
            .command = cmd,
            .repeat = false,
        };
        infrared_send(&msg, SEND_REPEATS);
        furi_delay_ms(60);

        /* check for skip while sending */
        InputEvent ev;
        if(furi_message_queue_get(app->queue, &ev, 0) == FuriStatusOk) {
            if(ev.type == InputTypePress && ev.key == InputKeyBack) {
                app->funcs[app->current_func].done = true;
                advance_function(app);
                return;
            }
        }
    }

    app->state = AppStateAsking;
    view_port_update(app->view_port);
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                    */
/* ------------------------------------------------------------------ */

int32_t sony_ir_brute_app(void* p) {
    UNUSED(p);

    AppContext* app = malloc(sizeof(AppContext));
    memset(app, 0, sizeof(AppContext));

    for(uint8_t i = 0; i < NUM_FUNCTIONS; i++) {
        app->funcs[i].name = FUNC_NAMES[i];
        app->funcs[i].code = -1;
        app->funcs[i].done = false;
    }
    app->state = AppStateIntro;

    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    app->gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    view_port_update(app->view_port);

    bool running = true;
    InputEvent event;

    while(running) {
        switch(app->state) {
        /* ---------- INTRO ---------- */
        case AppStateIntro:
            if(furi_message_queue_get(app->queue, &event, FuriWaitForever) == FuriStatusOk) {
                if(event.type == InputTypePress) {
                    if(event.key == InputKeyBack) {
                        running = false;
                    } else if(event.key == InputKeyOk) {
                        app->current_func = 0;
                        begin_function(app);
                    }
                }
            }
            break;

        /* ---------- SENDING ---------- */
        case AppStateSending:
            do_send(app);
            break;

        /* ---------- ASKING ---------- */
        case AppStateAsking:
            if(furi_message_queue_get(app->queue, &event, FuriWaitForever) == FuriStatusOk) {
                if(event.type != InputTypePress) break;

                if(event.key == InputKeyBack) {
                    app->funcs[app->current_func].done = true;
                    advance_function(app);
                    break;
                }

                uint8_t mid = (app->lo + app->hi) / 2;

                if(event.key == InputKeyUp) {
                    /* YES — code is in [lo, mid] */
                    app->hi = mid;
                } else if(event.key == InputKeyDown) {
                    /* NO — code is in [mid+1, hi] */
                    app->lo = mid + 1;
                } else {
                    break;
                }

                app->round++;

                if(app->lo == app->hi) {
                    app->funcs[app->current_func].code = (int32_t)app->lo;
                    app->funcs[app->current_func].done = true;
                    app->state = AppStateFound;
                } else {
                    app->state = AppStateSending;
                }
                view_port_update(app->view_port);
            }
            break;

        /* ---------- FOUND ---------- */
        case AppStateFound:
            if(furi_message_queue_get(app->queue, &event, FuriWaitForever) == FuriStatusOk) {
                if(event.type == InputTypePress && event.key == InputKeyOk) {
                    advance_function(app);
                }
            }
            break;

        /* ---------- DONE ---------- */
        case AppStateDone:
            view_port_update(app->view_port);
            save_ir_file(app);
            app->state = AppStateSaved;
            view_port_update(app->view_port);
            break;

        /* ---------- SAVED ---------- */
        case AppStateSaved:
            if(furi_message_queue_get(app->queue, &event, FuriWaitForever) == FuriStatusOk) {
                if(event.type == InputTypePress && event.key == InputKeyBack) {
                    running = false;
                }
            }
            break;
        }
    }

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->queue);
    free(app);

    return 0;
}
