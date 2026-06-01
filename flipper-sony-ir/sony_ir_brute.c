/* Sony IR Finder v4 — linear auto-scan, user presses OK when device reacts
 * Protocol: SIRC-12, 40kHz carrier (handled by SDK)
 * Timing: 3 frames × explicit 25ms inter-frame gap (Sony spec: 45ms period)
 * addr=1: Power/Vol/Inputs/Mute   addr=17: Play/Stop/Pause/Next/Prev
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

/* ── Constants ──────────────────────────────────────────────────── */
#define CMD_MIN         0
#define CMD_MAX         127
#define NUM_FUNCS       12
#define SEND_REPS       3
#define INTER_FRAME_MS  25   /* gap after each frame; frame~20ms → ~45ms period */
#define INTER_CMD_MS    55   /* gap between different command codes */
#define OUTPUT_PATH     "/ext/infrared/Sony_CMT_NE3_confirmed.ir"
#define RECORD_STORAGE  "storage"
#define ANIM_MS         60
#define APP_VIEW        0

static const char* const FUNC_NAMES[NUM_FUNCS] = {
    "Power", "Vol+",  "Vol-",  "CD",
    "Tape",  "Tuner", "Mute",  "Play",
    "Stop",  "Pause", "Next",  "Prev",
};
static const uint8_t FUNC_ADDRS[NUM_FUNCS] = {
    1,1,1,1,1,1,1,   /* system controls */
    17,17,17,17,17,  /* CD transport    */
};

/* ── States ─────────────────────────────────────────────────────── */
typedef enum {
    AppStateWelcome,
    AppStateScanning, /* auto-cycling 0→127 */
    AppStateCapture,  /* user caught a reaction — confirm code */
    AppStateFound,
    AppStateDone,
    AppStateSaved,
    AppStateRemote,   /* quick-fire remote after save */
} AppState;

typedef enum { EvtProgress=0, EvtDone=1, EvtWrap=2 } AppEvt;
typedef enum { AfterNone=0, AfterResume=1, AfterSkip=2 } AfterPlan;

/* ── Model ───────────────────────────────────────────────────────── */
typedef struct {
    AppState state;
    uint8_t  fi;
    uint8_t  scan_prog;   /* code currently being sent    */
    uint8_t  capture;     /* code captured when OK pressed */
    uint8_t  pass;        /* full cycles completed         */
    uint8_t  anim;
    uint8_t  remote_fi;   /* selected function in Remote mode */
    int32_t  codes[NUM_FUNCS];
    bool     done[NUM_FUNCS];
    uint8_t  found_count;
} AppModel;

/* ── App ─────────────────────────────────────────────────────────── */
typedef struct {
    ViewDispatcher*  vd;
    View*            view;
    Gui*             gui;
    NotificationApp* notif;
    FuriThread*      ir_thread;
    bool             thread_running;
    uint8_t          tfi, tstart;
    bool             single_shot;
    volatile uint8_t t_prog;
    volatile bool    abort;
    AfterPlan        after_done;
    uint8_t          resume_from;
} App;

/* ── Drawing helpers ─────────────────────────────────────────────── */
static void draw_prog_bar(Canvas* c, uint8_t done, uint8_t total) {
    canvas_draw_frame(c, 0, 0, 128, 6);
    if(done && total) {
        uint8_t w = (uint8_t)((uint16_t)126 * done / total);
        if(w) canvas_draw_box(c, 1, 1, w, 4);
    }
}
static void draw_sep(Canvas* c) { canvas_draw_line(c, 0, 7, 127, 7); }
static void draw_frac(Canvas* c, uint8_t d, uint8_t t) {
    char b[8]; snprintf(b,sizeof(b),"%u/%u",d,t);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 126, 0, AlignRight, AlignTop, b);
}
static void draw_header(Canvas* c, AppModel* m) {
    draw_prog_bar(c, m->found_count, NUM_FUNCS);
    draw_sep(c);
    draw_frac(c, m->found_count, NUM_FUNCS);
}
static void draw_ir_beam(Canvas* c, uint8_t x1, uint8_t x2, uint8_t y, uint8_t a) {
    uint8_t span = x2 - x1; if(!span) return;
    for(uint8_t d=0;d<4;d++){
        uint8_t ph=(uint8_t)(((uint16_t)a*2+d*16)%span);
        uint8_t x=x1+ph; if(x<=x2) canvas_draw_disc(c,x,y,1);
    }
}
static void draw_flipper(Canvas* c, uint8_t x, uint8_t y) {
    canvas_draw_rframe(c,x,y+2,10,6,1);
    canvas_draw_line(c,x+5,y,x+5,y+2);
    canvas_draw_dot(c,x+5,y);
}
static void draw_stereo(Canvas* c, uint8_t x, uint8_t y) {
    canvas_draw_frame(c,x,y,14,8);
    canvas_draw_disc(c,x+4,y+4,2); canvas_draw_disc(c,x+10,y+4,2);
    canvas_draw_line(c,x+1,y+1,x+12,y+1);
}

/* ── Per-state draw ──────────────────────────────────────────────── */
static void draw_welcome(Canvas* c, AppModel* m) {
    draw_header(c, m);
    bool inv = (m->anim/12)%2==0;
    canvas_set_font(c, FontPrimary);
    if(inv){ canvas_draw_box(c,10,10,108,12); canvas_set_color(c,ColorWhite); }
    canvas_draw_str_aligned(c,64,21,AlignCenter,AlignBottom,"SONY IR FINDER");
    canvas_set_color(c,ColorBlack);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str_aligned(c,64,31,AlignCenter,AlignBottom,"Auto-scan: press OK when stereo reacts");
    draw_flipper(c,8,37); draw_stereo(c,106,37); draw_ir_beam(c,22,102,41,m->anim);
    bool fill=(m->anim/18)%2==0;
    if(fill){ canvas_draw_rbox(c,31,52,36,10,2); canvas_set_color(c,ColorWhite);
              canvas_draw_str_aligned(c,49,61,AlignCenter,AlignBottom,"[OK] Start"); canvas_set_color(c,ColorBlack); }
    else    { canvas_draw_rframe(c,31,52,36,10,2);
              canvas_draw_str_aligned(c,49,61,AlignCenter,AlignBottom,"[OK] Start"); }
    canvas_draw_str(c,92,62,"[<] Exit");
}

static void draw_scanning(Canvas* c, AppModel* m) {
    draw_header(c, m);
    /* Function + address */
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 18, FUNC_NAMES[m->fi]);
    char ab[8]; snprintf(ab,sizeof(ab),"a=%u",FUNC_ADDRS[m->fi]);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 64, 18, ab);
    /* Pass counter */
    char pb[12]; snprintf(pb,sizeof(pb),"Pass %u",m->pass+1);
    canvas_draw_str_aligned(c,126,9,AlignRight,AlignTop,pb);
    /* Code progress bar 0-127 */
    canvas_draw_frame(c, 2, 21, 124, 5);
    uint8_t bw=(uint8_t)((uint16_t)122*m->scan_prog/CMD_MAX);
    if(bw) canvas_draw_box(c,3,22,bw,3);
    /* Current code */
    char cd[16]; snprintf(cd,sizeof(cd),"Code: %u",m->scan_prog);
    canvas_draw_str(c,2,34,cd);
    /* IR animation */
    draw_flipper(c,2,37); draw_stereo(c,106,37); draw_ir_beam(c,16,102,41,m->anim);
    /* Blinking prompt */
    if((m->anim/16)%2==0){
        canvas_set_font(c,FontPrimary);
        canvas_draw_str_aligned(c,64,55,AlignCenter,AlignBottom,"OK when triggered!");
    }
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,2,63,"[v]Skip  [<]Exit");
}

static void draw_capture(Canvas* c, AppModel* m) {
    draw_header(c, m);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c,2,18,FUNC_NAMES[m->fi]);
    char ab[8]; snprintf(ab,sizeof(ab),"a=%u",FUNC_ADDRS[m->fi]);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,64,18,ab);
    /* Code box, pulsing */
    uint8_t pulse=(m->anim/6)%2;
    if(pulse) canvas_draw_rbox(c,10,21,108,13,2);
    else      canvas_draw_rframe(c,10,21,108,13,2);
    char cs[24]; snprintf(cs,sizeof(cs),"Code %u  (0x%02X)",m->capture,m->capture);
    canvas_set_font(c,FontSecondary);
    if(pulse) canvas_set_color(c,ColorWhite);
    canvas_draw_str_aligned(c,64,32,AlignCenter,AlignBottom,cs);
    canvas_set_color(c,ColorBlack);
    /* Question */
    canvas_set_font(c,FontPrimary);
    canvas_draw_str_aligned(c,64,45,AlignCenter,AlignBottom,"Did this trigger?");
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,2,54,"[OK]Yes  [v]No-resume");
    canvas_draw_str(c,2,63,"[^]Try-1  [>]Resend  [<]Skip");
}

static void draw_found(Canvas* c, AppModel* m) {
    draw_header(c, m);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str_aligned(c,64,24,AlignCenter,AlignBottom,"FOUND!");
    char det[32];
    snprintf(det,sizeof(det),"%s = %u  (0x%02X)  a=%u",
             FUNC_NAMES[m->fi],(uint8_t)m->codes[m->fi],
             (uint8_t)m->codes[m->fi],FUNC_ADDRS[m->fi]);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str_aligned(c,64,36,AlignCenter,AlignBottom,det);
    uint8_t r=5+(m->anim%4);
    canvas_draw_circle(c,64,48,r);
    if((m->anim/4)%2==0) canvas_draw_disc(c,64,48,r>3?r-3:1);
    canvas_draw_str_aligned(c,64,63,AlignCenter,AlignBottom,"[OK] Next function");
}

static void draw_done(Canvas* c, AppModel* m) {
    draw_prog_bar(c,NUM_FUNCS,NUM_FUNCS); draw_sep(c);
    canvas_set_font(c,FontPrimary);
    char s[32]; snprintf(s,sizeof(s),"Done!  %u/%u found",m->found_count,NUM_FUNCS);
    canvas_draw_str_aligned(c,64,22,AlignCenter,AlignBottom,s);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str_aligned(c,64,34,AlignCenter,AlignBottom,"Saving remote file...");
    char dots[5]={0}; uint8_t n=(m->anim/10)%4;
    for(uint8_t i=0;i<n;i++) dots[i]='.';
    canvas_draw_str_aligned(c,64,46,AlignCenter,AlignBottom,dots);
}

static void draw_saved(Canvas* c, AppModel* m) {
    draw_prog_bar(c,NUM_FUNCS,NUM_FUNCS); draw_sep(c);
    draw_frac(c,NUM_FUNCS,NUM_FUNCS);
    canvas_set_font(c,FontPrimary);
    char h[24]; snprintf(h,sizeof(h),"Saved! (%u found)",m->found_count);
    canvas_draw_str_aligned(c,64,20,AlignCenter,AlignBottom,h);
    canvas_set_font(c,FontSecondary);
    uint8_t st=((uint16_t)(m->anim/40))%NUM_FUNCS;
    for(uint8_t row=0;row<3;row++){
        uint8_t idx=(st+row)%NUM_FUNCS;
        char rb[28];
        if(m->codes[idx]>=0)
            snprintf(rb,sizeof(rb),"%-6s %3u(0x%02X) a=%u",
                     FUNC_NAMES[idx],(uint8_t)m->codes[idx],
                     (uint8_t)m->codes[idx],FUNC_ADDRS[idx]);
        else snprintf(rb,sizeof(rb),"%-6s ---",FUNC_NAMES[idx]);
        canvas_draw_str(c,4,(uint8_t)(30+row*10),rb);
    }
    if(m->found_count > 0)
        canvas_draw_str_aligned(c,64,63,AlignCenter,AlignBottom,"[OK] Use Remote  [<] Exit");
    else
        canvas_draw_str_aligned(c,64,63,AlignCenter,AlignBottom,"[OK]/[<] Exit");
}

static void draw_remote(Canvas* c, AppModel* m) {
    draw_prog_bar(c, m->found_count, NUM_FUNCS);
    draw_sep(c);
    draw_frac(c, m->found_count, NUM_FUNCS);

    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 17, AlignCenter, AlignBottom, "Sony CMT-NE3 Remote");

    /* Centered function name with arrows */
    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 29, AlignCenter, AlignBottom, FUNC_NAMES[m->remote_fi]);

    /* Code + address detail */
    canvas_set_font(c, FontSecondary);
    if(m->codes[m->remote_fi] >= 0) {
        char det[24];
        snprintf(det, sizeof(det), "Code %u  a=%u",
                 (uint8_t)m->codes[m->remote_fi], FUNC_ADDRS[m->remote_fi]);
        canvas_draw_str_aligned(c, 64, 39, AlignCenter, AlignBottom, det);
    } else {
        canvas_draw_str_aligned(c, 64, 39, AlignCenter, AlignBottom, "(not found)");
    }

    /* Nav arrows */
    canvas_draw_str(c, 2, 29, "<");
    canvas_draw_str(c, 121, 29, ">");

    /* Pulsing OK button */
    bool fill = (m->anim / 12) % 2 == 0;
    if(m->codes[m->remote_fi] >= 0) {
        if(fill) {
            canvas_draw_rbox(c, 34, 44, 60, 12, 2);
            canvas_set_color(c, ColorWhite);
            canvas_draw_str_aligned(c, 64, 54, AlignCenter, AlignBottom, "[OK] Send!");
            canvas_set_color(c, ColorBlack);
        } else {
            canvas_draw_rframe(c, 34, 44, 60, 12, 2);
            canvas_draw_str_aligned(c, 64, 54, AlignCenter, AlignBottom, "[OK] Send!");
        }
    } else {
        canvas_draw_str_aligned(c, 64, 54, AlignCenter, AlignBottom, "-- no code --");
    }
    canvas_draw_str_aligned(c, 64, 63, AlignCenter, AlignBottom, "[<] / [v] Exit");
}

static void draw_cb(Canvas* c, void* model) {
    AppModel* m=(AppModel*)model;
    canvas_clear(c); canvas_set_color(c,ColorBlack);
    switch(m->state){
    case AppStateWelcome:  draw_welcome(c,m);  break;
    case AppStateScanning: draw_scanning(c,m); break;
    case AppStateCapture:  draw_capture(c,m);  break;
    case AppStateFound:    draw_found(c,m);    break;
    case AppStateDone:     draw_done(c,m);     break;
    case AppStateSaved:    draw_saved(c,m);    break;
    case AppStateRemote:   draw_remote(c,m);   break;
    }
}

/* ── IR thread ───────────────────────────────────────────────────── */
static int32_t ir_thread_fn(void* ctx) {
    App* app=(App*)ctx;
    uint8_t fi   =app->tfi;
    uint8_t start=app->tstart;
    bool single  =app->single_shot;
    uint8_t addr =FUNC_ADDRS[fi];
    uint8_t cmd  =start;

    while(!app->abort) {
        app->t_prog=cmd;
        view_dispatcher_send_custom_event(app->vd, EvtProgress);

        InfraredMessage msg={
            .protocol=InfraredProtocolSIRC,
            .address=addr, .command=cmd, .repeat=false };

        /* Send 3 frames with explicit inter-frame gap for correct Sony timing */
        for(int rep=0; rep<SEND_REPS && !app->abort; rep++) {
            infrared_send(&msg,1);
            furi_delay_ms(INTER_FRAME_MS);
        }

        if(single) break;

        if(!app->abort) {
            furi_delay_ms(INTER_CMD_MS);
            if(cmd==CMD_MAX) {
                cmd=CMD_MIN;
                view_dispatcher_send_custom_event(app->vd, EvtWrap);
            } else {
                cmd++;
            }
        }
    }
    view_dispatcher_send_custom_event(app->vd, EvtDone);
    return 0;
}

static void start_ir_thread(App* app, uint8_t fi, uint8_t start_cmd, bool single_shot) {
    app->abort=false;
    app->tfi=fi; app->tstart=start_cmd; app->single_shot=single_shot;
    if(app->ir_thread){ furi_thread_free(app->ir_thread); app->ir_thread=NULL; }
    app->ir_thread=furi_thread_alloc_ex("ir_send",1024,ir_thread_fn,app);
    furi_thread_start(app->ir_thread);
    app->thread_running=true;
}

static void join_ir_thread(App* app) {
    if(app->ir_thread){
        furi_thread_join(app->ir_thread);
        furi_thread_free(app->ir_thread);
        app->ir_thread=NULL;
    }
    app->thread_running=false;
}

static void stop_ir_thread(App* app) {
    app->abort=true; join_ir_thread(app);
}

/* ── Helpers ─────────────────────────────────────────────────────── */
static bool advance_to_next(AppModel* m) {
    m->fi++;
    if(m->fi>=NUM_FUNCS){ m->state=AppStateDone; return false; }
    m->scan_prog=CMD_MIN; m->pass=0;
    m->state=AppStateScanning;
    return true;
}

typedef struct { int32_t codes[NUM_FUNCS]; bool done[NUM_FUNCS]; } SaveData;

static void save_ir_file(const SaveData* d) {
    Storage* st=(Storage*)furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(st,"/ext/infrared");
    File* f=storage_file_alloc(st);
    if(storage_file_open(f,OUTPUT_PATH,FSAM_WRITE,FSOM_CREATE_ALWAYS)){
        const char* hdr="Filetype: IR signals file\nVersion: 1\n";
        storage_file_write(f,hdr,(uint16_t)strlen(hdr));
        for(uint8_t i=0;i<NUM_FUNCS;i++){
            if(!d->done[i]||d->codes[i]<0) continue;
            char buf[128];
            int n=snprintf(buf,sizeof(buf),
                "\nname: %s\ntype: parsed\nprotocol: SIRC\n"
                "address: %02X 00 00 00\ncommand: %02X 00 00 00\n",
                FUNC_NAMES[i],(uint8_t)FUNC_ADDRS[i],(uint8_t)d->codes[i]);
            if(n>0) storage_file_write(f,buf,(uint16_t)n);
        }
        storage_file_close(f);
    }
    storage_file_free(f); furi_record_close(RECORD_STORAGE);
}

static void save_from_model(App* app) {
    SaveData snap;
    AppModel* m=(AppModel*)view_get_model(app->view);
    memcpy(snap.codes,m->codes,sizeof(snap.codes));
    memcpy(snap.done,m->done,sizeof(snap.done));
    m->state=AppStateSaved;
    view_commit_model(app->view,true);
    save_ir_file(&snap);
}

/* ── Input callback ──────────────────────────────────────────────── */
static bool input_cb(InputEvent* ev, void* ctx) {
    App* app=(App*)ctx;
    if(ev->type!=InputTypePress && ev->type!=InputTypeRepeat) return false;

    AppModel* m=(AppModel*)view_get_model(app->view);
    bool redraw=false, handled=true, do_start=false;
    uint8_t start_fi=0, start_cmd=0; bool start_single=false;

    switch(m->state){

    case AppStateWelcome:
        if(ev->key==InputKeyOk){
            m->fi=0; m->scan_prog=CMD_MIN; m->pass=0;
            m->state=AppStateScanning; redraw=true;
            do_start=true; start_fi=0; start_cmd=CMD_MIN; start_single=false;
        } else if(ev->key==InputKeyBack){
            view_commit_model(app->view,false);
            view_dispatcher_stop(app->vd); return true;
        } else handled=false;
        break;

    case AppStateScanning:
        if(ev->key==InputKeyOk){
            /* Capture: abort scan, record current code */
            m->capture=app->t_prog;
            m->state=AppStateCapture; redraw=true;
            if(app->thread_running){
                app->abort=true;
                app->after_done=AfterNone;
            }
        } else if(ev->key==InputKeyDown || ev->key==InputKeyBack){
            /* Skip this function */
            m->done[m->fi]=true;
            if(app->thread_running){
                app->abort=true;
                app->after_done=AfterSkip;
                redraw=true;
            } else {
                bool need=advance_to_next(m); redraw=true;
                if(need){ do_start=true; start_fi=m->fi; start_cmd=CMD_MIN; start_single=false; }
            }
        } else handled=false;
        break;

    case AppStateCapture:
        if(ev->key==InputKeyOk){
            /* Confirm */
            m->codes[m->fi]=(int32_t)m->capture;
            m->done[m->fi]=true; m->found_count++; m->anim=0;
            m->state=AppStateFound; redraw=true;
            notification_message(app->notif,&sequence_single_vibro);
        } else if(ev->key==InputKeyDown){
            /* Reject — resume scan from capture+1 */
            uint8_t next=(m->capture<CMD_MAX)?m->capture+1:CMD_MIN;
            m->state=AppStateScanning; m->scan_prog=next; redraw=true;
            if(app->thread_running){
                app->after_done=AfterResume; app->resume_from=next;
            } else {
                do_start=true; start_fi=m->fi; start_cmd=next; start_single=false;
            }
        } else if(ev->key==InputKeyUp){
            /* Try one code earlier (reaction-lag compensation) */
            if(m->capture>CMD_MIN) m->capture--;
            redraw=true;
            if(!app->thread_running){
                do_start=true; start_fi=m->fi; start_cmd=m->capture; start_single=true;
            }
        } else if(ev->key==InputKeyRight){
            /* Resend capture code */
            if(!app->thread_running){
                do_start=true; start_fi=m->fi; start_cmd=m->capture; start_single=true;
            }
        } else if(ev->key==InputKeyBack){
            /* Skip function */
            m->done[m->fi]=true;
            if(app->thread_running){
                app->after_done=AfterSkip;
            } else {
                bool need=advance_to_next(m); redraw=true;
                if(need){ do_start=true; start_fi=m->fi; start_cmd=CMD_MIN; start_single=false; }
            }
        } else handled=false;
        break;

    case AppStateFound:
        if(ev->key==InputKeyOk || ev->key==InputKeyBack){
            bool need=advance_to_next(m); redraw=true;
            if(need){ do_start=true; start_fi=m->fi; start_cmd=CMD_MIN; start_single=false; }
        } else handled=false;
        break;

    case AppStateDone: handled=false; break;

    case AppStateSaved:
        if(ev->key==InputKeyOk && m->found_count>0){
            /* Enter quick-remote mode — find first confirmed function */
            m->remote_fi=0;
            while(m->remote_fi<NUM_FUNCS && m->codes[m->remote_fi]<0) m->remote_fi++;
            if(m->remote_fi<NUM_FUNCS) { m->state=AppStateRemote; redraw=true; }
            else { view_commit_model(app->view,false); view_dispatcher_stop(app->vd); return true; }
        } else if(ev->key==InputKeyBack){
            view_commit_model(app->view,false);
            view_dispatcher_stop(app->vd); return true;
        }
        break;

    case AppStateRemote: {
        if(ev->key==InputKeyLeft || ev->key==InputKeyRight){
            /* Cycle to prev/next confirmed function */
            int8_t dir=(ev->key==InputKeyRight)?1:-1;
            uint8_t nfi=m->remote_fi;
            for(uint8_t i=0;i<NUM_FUNCS;i++){
                nfi=(uint8_t)((nfi+NUM_FUNCS+dir)%NUM_FUNCS);
                if(m->codes[nfi]>=0) break;
            }
            m->remote_fi=nfi; redraw=true;
        } else if(ev->key==InputKeyUp){
            /* cycle backward */
            uint8_t nfi=m->remote_fi;
            for(uint8_t i=0;i<NUM_FUNCS;i++){
                nfi=(uint8_t)((nfi+NUM_FUNCS-1)%NUM_FUNCS);
                if(m->codes[nfi]>=0) break;
            }
            m->remote_fi=nfi; redraw=true;
        } else if(ev->key==InputKeyDown){
            view_commit_model(app->view,false);
            view_dispatcher_stop(app->vd); return true;
        } else if(ev->key==InputKeyOk){
            if(m->codes[m->remote_fi]>=0 && !app->thread_running){
                do_start=true;
                start_fi=m->remote_fi;
                start_cmd=(uint8_t)m->codes[m->remote_fi];
                start_single=true;
                notification_message(app->notif,&sequence_single_vibro);
            }
        } else if(ev->key==InputKeyBack){
            view_commit_model(app->view,false);
            view_dispatcher_stop(app->vd); return true;
        } else handled=false;
        break;
    }
    }

    if(do_start){
        view_commit_model(app->view,redraw);
        start_ir_thread(app,start_fi,start_cmd,start_single);
        return true;
    }
    view_commit_model(app->view,redraw);
    return handled;
}

/* ── Custom event callback ───────────────────────────────────────── */
static bool custom_event_cb(void* ctx, uint32_t ev) {
    App* app=(App*)ctx;

    if(ev==EvtProgress){
        AppModel* m=(AppModel*)view_get_model(app->view);
        m->scan_prog=app->t_prog;
        view_commit_model(app->view,true);
        return true;
    }

    if(ev==EvtWrap){
        AppModel* m=(AppModel*)view_get_model(app->view);
        m->pass++;
        view_commit_model(app->view,true);
        notification_message(app->notif,&sequence_single_vibro);
        return true;
    }

    if(ev==EvtDone){
        join_ir_thread(app);

        AfterPlan plan=app->after_done;
        app->after_done=AfterNone;

        if(plan==AfterResume){
            AppModel* m=(AppModel*)view_get_model(app->view);
            m->state=AppStateScanning;
            m->scan_prog=app->resume_from;
            view_commit_model(app->view,true);
            start_ir_thread(app,m->fi,app->resume_from,false);
            return true;
        }

        if(plan==AfterSkip){
            AppModel* m=(AppModel*)view_get_model(app->view);
            if(!m->done[m->fi]) m->done[m->fi]=true;
            bool need=advance_to_next(m);
            if(need){
                view_commit_model(app->view,true);
                start_ir_thread(app,m->fi,CMD_MIN,false);
            } else {
                view_commit_model(app->view,true);
                save_from_model(app);
            }
            return true;
        }

        /* AfterNone — thread stopped for capture, single-shot, or remote fire */
        AppModel* m=(AppModel*)view_get_model(app->view);
        if(m->state==AppStateDone){
            view_commit_model(app->view,true);
            save_from_model(app);
            return true;
        }
        /* Remote single-shot: stay on Remote screen */
        view_commit_model(app->view, m->state==AppStateRemote);
        return true;
    }

    return true;
}

/* ── Tick ────────────────────────────────────────────────────────── */
static void tick_cb(void* ctx) {
    App* app=(App*)ctx;
    AppModel* m=(AppModel*)view_get_model(app->view);
    m->anim++;
    if(m->state==AppStateDone){
        view_commit_model(app->view,true);
        save_from_model(app);
        return;
    }
    view_commit_model(app->view,true);
}

static bool nav_cb(void* ctx) {
    App* app=(App*)ctx;
    view_dispatcher_stop(app->vd);
    return true;
}

/* ── Entry point ─────────────────────────────────────────────────── */
int32_t sony_ir_search_app(void* p) {
    UNUSED(p);
    App* app=malloc(sizeof(App));
    memset(app,0,sizeof(App));

    app->view=view_alloc();
    view_allocate_model(app->view,ViewModelTypeLocking,sizeof(AppModel));
    view_set_context(app->view,app);
    view_set_draw_callback(app->view,draw_cb);
    view_set_input_callback(app->view,input_cb);

    AppModel* m=(AppModel*)view_get_model(app->view);
    m->state=AppStateWelcome;
    for(uint8_t i=0;i<NUM_FUNCS;i++) m->codes[i]=-1;
    view_commit_model(app->view,false);

    app->vd=view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->vd,app);
    view_dispatcher_set_custom_event_callback(app->vd,custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->vd,nav_cb);
    view_dispatcher_set_tick_event_callback(app->vd,tick_cb,ANIM_MS);
    view_dispatcher_add_view(app->vd,APP_VIEW,app->view);
    view_dispatcher_switch_to_view(app->vd,APP_VIEW);

    app->gui=(Gui*)furi_record_open(RECORD_GUI);
    app->notif=(NotificationApp*)furi_record_open(RECORD_NOTIFICATION);
    view_dispatcher_attach_to_gui(app->vd,app->gui,ViewDispatcherTypeFullscreen);

    view_dispatcher_run(app->vd);

    stop_ir_thread(app);
    view_dispatcher_remove_view(app->vd,APP_VIEW);
    view_dispatcher_free(app->vd);
    view_free_model(app->view);
    view_free(app->view);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
