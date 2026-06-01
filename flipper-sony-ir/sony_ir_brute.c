/* Sony IR Finder v5 — dual mode: Binary Search or Auto-Scan
 * Protocol: SIRC-12, 40kHz handled by SDK
 * Timing:   3 frames × 25ms gap (Sony spec: ~45ms repeat period)
 * Addresses: addr=1 system, addr=17 CD transport (confirmed CMT-NE3)
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

/* ── Constants ─────────────────────────────────────────────────── */
#define CMD_MIN        0
#define CMD_MAX        127
#define NUM_FUNCS      12
#define SEND_REPS      3
#define INTER_FRAME_MS 25    /* after each frame; ~20ms frame + 25ms = 45ms period */
#define INTER_CMD_MS   55
#define OUTPUT_PATH    "/ext/infrared/Sony_CMT_NE3_confirmed.ir"
#define RECORD_STORAGE "storage"
#define ANIM_MS        60
#define APP_VIEW       0

static const char* const FUNC_NAMES[NUM_FUNCS] = {
    "Power","Vol+","Vol-","CD","Tape","Tuner","Mute","Play","Stop","Pause","Next","Prev",
};
static const uint8_t FUNC_ADDRS[NUM_FUNCS] = {
    1,1,1,1,1,1,1, 17,17,17,17,17,
};

/* ── States ─────────────────────────────────────────────────────── */
typedef enum {
    AppStateWelcome,
    /* Binary search states */
    AppStateSending,    /* IR thread blasting lo→mid             */
    AppStateAsking,     /* Did it react? ^yes vno >resend <skip  */
    AppStateSingle,     /* Last code: OKyes vno >resend <skip    */
    AppStateNotFound,
    /* Auto-scan states */
    AppStateScanning,   /* continuous 0→127 loop                 */
    AppStateCapture,    /* caught a reaction — confirm code      */
    /* Shared */
    AppStateFound,
    AppStateDone,
    AppStateSaved,
    AppStateRemote,
} AppState;

typedef enum { EvtProgress=0, EvtDone=1, EvtWrap=2 } AppEvt;
typedef enum { AfterNone=0, AfterResume=1, AfterSkip=2 } AfterPlan;
typedef enum { ThreadBinary=0, ThreadScan=1, ThreadSingle=2 } ThreadMode;

/* ── Model ──────────────────────────────────────────────────────── */
typedef struct {
    AppState state;
    bool     binary_mode;    /* true = binary search, false = auto-scan */
    uint8_t  fi;
    /* binary search fields */
    uint8_t  lo, hi, round, send_prog;
    /* scan fields */
    uint8_t  scan_prog, capture, pass;
    /* shared */
    uint8_t  anim, remote_fi;
    int32_t  codes[NUM_FUNCS];
    bool     done[NUM_FUNCS];
    uint8_t  found_count;
} AppModel;

/* ── App context ────────────────────────────────────────────────── */
typedef struct {
    ViewDispatcher*  vd;
    View*            view;
    Gui*             gui;
    NotificationApp* notif;
    FuriThread*      ir_thread;
    bool             thread_running;
    /* thread params */
    uint8_t    tfi, tlo, thi;
    ThreadMode thread_mode;
    volatile uint8_t t_prog;
    volatile bool    abort;
    /* deferred actions */
    AfterPlan after_done;
    uint8_t   resume_from;
    bool      resending;
} App;

/* ── Draw primitives ────────────────────────────────────────────── */
static void draw_pbar(Canvas* c, uint8_t d, uint8_t t) {
    canvas_draw_frame(c,0,0,128,6);
    if(d&&t){ uint8_t w=(uint8_t)((uint16_t)126*d/t); if(w) canvas_draw_box(c,1,1,w,4); }
}
static void draw_sep(Canvas* c) { canvas_draw_line(c,0,7,127,7); }
static void draw_frac(Canvas* c, uint8_t d, uint8_t t) {
    char b[8]; snprintf(b,8,"%u/%u",d,t);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str_aligned(c,126,0,AlignRight,AlignTop,b);
}
static void draw_header(Canvas* c, AppModel* m) {
    draw_pbar(c,m->found_count,NUM_FUNCS); draw_sep(c); draw_frac(c,m->found_count,NUM_FUNCS);
}
static void draw_beam(Canvas* c, uint8_t x1, uint8_t x2, uint8_t y, uint8_t a) {
    uint8_t sp=x2-x1; if(!sp) return;
    for(uint8_t d=0;d<4;d++){
        uint8_t ph=(uint8_t)(((uint16_t)a*2+d*16)%sp);
        uint8_t x=x1+ph; if(x<=x2) canvas_draw_disc(c,x,y,1);
    }
}
static void draw_flipper_icon(Canvas* c, uint8_t x, uint8_t y) {
    canvas_draw_rframe(c,x,y+2,10,6,1);
    canvas_draw_line(c,x+5,y,x+5,y+2); canvas_draw_dot(c,x+5,y);
}
static void draw_stereo_icon(Canvas* c, uint8_t x, uint8_t y) {
    canvas_draw_frame(c,x,y,14,8);
    canvas_draw_disc(c,x+4,y+4,2); canvas_draw_disc(c,x+10,y+4,2);
    canvas_draw_line(c,x+1,y+1,x+12,y+1);
}

/* ── Per-state draw ─────────────────────────────────────────────── */
static void draw_welcome(Canvas* c, AppModel* m) {
    draw_header(c,m);
    /* Animated title */
    bool inv=(m->anim/12)%2==0;
    canvas_set_font(c,FontPrimary);
    if(inv){ canvas_draw_box(c,10,10,108,12); canvas_set_color(c,ColorWhite); }
    canvas_draw_str_aligned(c,64,21,AlignCenter,AlignBottom,"IR CRACKER");
    canvas_set_color(c,ColorBlack);
    /* Mode selector */
    canvas_set_font(c,FontSecondary);
    if(m->binary_mode){
        canvas_draw_rbox(c,14,23,100,9,2);
        canvas_set_color(c,ColorWhite);
        canvas_draw_str_aligned(c,64,31,AlignCenter,AlignBottom,">> Binary Search <<");
        canvas_set_color(c,ColorBlack);
        canvas_draw_str_aligned(c,64,42,AlignCenter,AlignBottom,"   Auto-Scan");
    } else {
        canvas_draw_str_aligned(c,64,31,AlignCenter,AlignBottom,"   Binary Search");
        canvas_draw_rbox(c,14,33,100,9,2);
        canvas_set_color(c,ColorWhite);
        canvas_draw_str_aligned(c,64,42,AlignCenter,AlignBottom,">> Auto-Scan <<");
        canvas_set_color(c,ColorBlack);
    }
    canvas_draw_str_aligned(c,64,50,AlignCenter,AlignBottom,"[^][v] switch mode");
    canvas_draw_str(c,2,63,"[OK] Start"); canvas_draw_str(c,86,63,"[<] Exit");
}

static void draw_sending(Canvas* c, AppModel* m) {
    draw_header(c,m);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str(c,2,18,FUNC_NAMES[m->fi]);
    char ab[8]; snprintf(ab,8,"a=%u",FUNC_ADDRS[m->fi]);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,60,18,ab);
    char rnd[12]; snprintf(rnd,12,"Rnd %u/7",m->round);
    canvas_draw_str_aligned(c,126,9,AlignRight,AlignTop,rnd);
    uint8_t mid=(m->lo+m->hi)/2;
    char rng[24]; snprintf(rng,24,"Testing %u-%u",m->lo,mid);
    canvas_draw_str(c,2,28,rng);
    draw_flipper_icon(c,2,34); draw_stereo_icon(c,106,34); draw_beam(c,16,102,38,m->anim);
    char pr[20]; snprintf(pr,20,"Sending: %u",m->send_prog);
    canvas_draw_str(c,2,53,pr);
    canvas_draw_str(c,2,63,"[<] Skip");
}

static void draw_asking(Canvas* c, AppModel* m) {
    draw_header(c,m);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str(c,2,18,FUNC_NAMES[m->fi]);
    char ab[8]; snprintf(ab,8,"a=%u",FUNC_ADDRS[m->fi]);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,60,18,ab);
    char rnd[12]; snprintf(rnd,12,"Rnd %u/7",m->round);
    canvas_draw_str_aligned(c,126,9,AlignRight,AlignTop,rnd);
    uint8_t mid=(m->lo+m->hi)/2;
    char sent[28]; snprintf(sent,28,"Sent codes %u - %u",m->lo,mid);
    canvas_draw_str_aligned(c,64,30,AlignCenter,AlignBottom,sent);
    if((m->anim/14)%2==0){
        canvas_set_font(c,FontPrimary);
        canvas_draw_str_aligned(c,64,44,AlignCenter,AlignBottom,"Did stereo react?");
    }
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,2,63,"[^]Yes [v]No [>]Again [<]Skip");
}

static void draw_single(Canvas* c, AppModel* m) {
    draw_header(c,m);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str(c,2,18,FUNC_NAMES[m->fi]);
    char ab[8]; snprintf(ab,8,"a=%u",FUNC_ADDRS[m->fi]);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,60,18,ab);
    uint8_t pulse=(m->anim/6)%2;
    if(pulse) canvas_draw_rbox(c,18,20,92,12,2);
    else      canvas_draw_rframe(c,18,20,92,12,2);
    char cs[22]; snprintf(cs,22,"Code %u  (0x%02X)",m->lo,m->lo);
    if(pulse) canvas_set_color(c,ColorWhite);
    canvas_draw_str_aligned(c,64,30,AlignCenter,AlignBottom,cs);
    canvas_set_color(c,ColorBlack);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str_aligned(c,64,44,AlignCenter,AlignBottom,"Did it trigger?");
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,2,63,"[OK]Yes [v]No [>]Resend [<]Skip");
}

static void draw_not_found(Canvas* c, AppModel* m) {
    draw_header(c,m);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str_aligned(c,64,24,AlignCenter,AlignBottom,"Not Found");
    canvas_set_font(c,FontSecondary);
    char l[36]; snprintf(l,36,"%s: no code identified",FUNC_NAMES[m->fi]);
    canvas_draw_str_aligned(c,64,36,AlignCenter,AlignBottom,l);
    canvas_draw_line(c,58,42,70,54); canvas_draw_line(c,70,42,58,54);
    canvas_draw_str_aligned(c,64,63,AlignCenter,AlignBottom,"[OK] Next function");
}

static void draw_scanning(Canvas* c, AppModel* m) {
    draw_header(c,m);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str(c,2,18,FUNC_NAMES[m->fi]);
    char ab[8]; snprintf(ab,8,"a=%u",FUNC_ADDRS[m->fi]);
    char pb[12]; snprintf(pb,12,"Pass %u",m->pass+1);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,60,18,ab);
    canvas_draw_str_aligned(c,126,9,AlignRight,AlignTop,pb);
    canvas_draw_frame(c,2,21,124,5);
    uint8_t bw=(uint8_t)((uint16_t)122*m->scan_prog/CMD_MAX);
    if(bw) canvas_draw_box(c,3,22,bw,3);
    char cd[16]; snprintf(cd,16,"Code: %u",m->scan_prog);
    canvas_draw_str(c,2,34,cd);
    draw_flipper_icon(c,2,37); draw_stereo_icon(c,106,37); draw_beam(c,16,102,41,m->anim);
    if((m->anim/16)%2==0){
        canvas_set_font(c,FontPrimary);
        canvas_draw_str_aligned(c,64,55,AlignCenter,AlignBottom,"OK when triggered!");
    }
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,2,63,"[v]Skip  [<]Exit");
}

static void draw_capture(Canvas* c, AppModel* m) {
    draw_header(c,m);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str(c,2,18,FUNC_NAMES[m->fi]);
    char ab[8]; snprintf(ab,8,"a=%u",FUNC_ADDRS[m->fi]);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,60,18,ab);
    uint8_t pulse=(m->anim/6)%2;
    if(pulse) canvas_draw_rbox(c,10,21,108,13,2);
    else      canvas_draw_rframe(c,10,21,108,13,2);
    char cs[24]; snprintf(cs,24,"Code %u  (0x%02X)",m->capture,m->capture);
    if(pulse) canvas_set_color(c,ColorWhite);
    canvas_draw_str_aligned(c,64,32,AlignCenter,AlignBottom,cs);
    canvas_set_color(c,ColorBlack);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str_aligned(c,64,45,AlignCenter,AlignBottom,"Did this trigger?");
    canvas_set_font(c,FontSecondary);
    canvas_draw_str(c,2,54,"[OK]Yes  [v]No-resume");
    canvas_draw_str(c,2,63,"[^]Try-1  [>]Resend  [<]Skip");
}

static void draw_found(Canvas* c, AppModel* m) {
    draw_header(c,m);
    canvas_set_font(c,FontPrimary);
    canvas_draw_str_aligned(c,64,24,AlignCenter,AlignBottom,"FOUND!");
    char det[34]; snprintf(det,34,"%s=%u(0x%02X) a=%u",
        FUNC_NAMES[m->fi],(uint8_t)m->codes[m->fi],(uint8_t)m->codes[m->fi],FUNC_ADDRS[m->fi]);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str_aligned(c,64,36,AlignCenter,AlignBottom,det);
    uint8_t r=5+(m->anim%4);
    canvas_draw_circle(c,64,48,r);
    if((m->anim/4)%2==0) canvas_draw_disc(c,64,48,r>3?r-3:1);
    canvas_draw_str_aligned(c,64,63,AlignCenter,AlignBottom,"[OK] Next function");
}

static void draw_done(Canvas* c, AppModel* m) {
    draw_pbar(c,NUM_FUNCS,NUM_FUNCS); draw_sep(c);
    canvas_set_font(c,FontPrimary);
    char s[32]; snprintf(s,32,"Done!  %u/%u found",m->found_count,NUM_FUNCS);
    canvas_draw_str_aligned(c,64,22,AlignCenter,AlignBottom,s);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str_aligned(c,64,34,AlignCenter,AlignBottom,"Saving remote file...");
    char dots[5]={0}; uint8_t n=(m->anim/10)%4;
    for(uint8_t i=0;i<n;i++) dots[i]='.';
    canvas_draw_str_aligned(c,64,46,AlignCenter,AlignBottom,dots);
}

static void draw_saved(Canvas* c, AppModel* m) {
    draw_pbar(c,NUM_FUNCS,NUM_FUNCS); draw_sep(c); draw_frac(c,NUM_FUNCS,NUM_FUNCS);
    canvas_set_font(c,FontPrimary);
    char h[24]; snprintf(h,24,"Saved! (%u found)",m->found_count);
    canvas_draw_str_aligned(c,64,20,AlignCenter,AlignBottom,h);
    canvas_set_font(c,FontSecondary);
    uint8_t st=((uint16_t)(m->anim/40))%NUM_FUNCS;
    for(uint8_t row=0;row<3;row++){
        uint8_t idx=(st+row)%NUM_FUNCS;
        char rb[28];
        if(m->codes[idx]>=0)
            snprintf(rb,28,"%-6s %3u(0x%02X) a=%u",FUNC_NAMES[idx],
                     (uint8_t)m->codes[idx],(uint8_t)m->codes[idx],FUNC_ADDRS[idx]);
        else snprintf(rb,28,"%-6s ---",FUNC_NAMES[idx]);
        canvas_draw_str(c,4,(uint8_t)(30+row*10),rb);
    }
    if(m->found_count>0)
        canvas_draw_str_aligned(c,64,63,AlignCenter,AlignBottom,"[OK] Use Remote  [<] Exit");
    else
        canvas_draw_str_aligned(c,64,63,AlignCenter,AlignBottom,"[OK]/[<] Exit");
}

static void draw_remote(Canvas* c, AppModel* m) {
    draw_pbar(c,m->found_count,NUM_FUNCS); draw_sep(c); draw_frac(c,m->found_count,NUM_FUNCS);
    canvas_set_font(c,FontSecondary);
    canvas_draw_str_aligned(c,64,17,AlignCenter,AlignBottom,"Sony CMT-NE3 Remote");
    canvas_set_font(c,FontPrimary);
    canvas_draw_str_aligned(c,64,29,AlignCenter,AlignBottom,FUNC_NAMES[m->remote_fi]);
    canvas_set_font(c,FontSecondary);
    if(m->codes[m->remote_fi]>=0){
        char det[24]; snprintf(det,24,"Code %u  a=%u",
                               (uint8_t)m->codes[m->remote_fi],FUNC_ADDRS[m->remote_fi]);
        canvas_draw_str_aligned(c,64,39,AlignCenter,AlignBottom,det);
    } else {
        canvas_draw_str_aligned(c,64,39,AlignCenter,AlignBottom,"(not found)");
    }
    canvas_draw_str(c,2,29,"<"); canvas_draw_str(c,121,29,">");
    if(m->codes[m->remote_fi]>=0){
        bool fill=(m->anim/12)%2==0;
        if(fill){ canvas_draw_rbox(c,34,44,60,12,2); canvas_set_color(c,ColorWhite);
                  canvas_draw_str_aligned(c,64,54,AlignCenter,AlignBottom,"[OK] Send!");
                  canvas_set_color(c,ColorBlack); }
        else    { canvas_draw_rframe(c,34,44,60,12,2);
                  canvas_draw_str_aligned(c,64,54,AlignCenter,AlignBottom,"[OK] Send!"); }
    } else {
        canvas_draw_str_aligned(c,64,54,AlignCenter,AlignBottom,"-- no code --");
    }
    canvas_draw_str_aligned(c,64,63,AlignCenter,AlignBottom,"[v]/[<] Exit");
}

static void draw_cb(Canvas* c, void* model) {
    AppModel* m=(AppModel*)model;
    canvas_clear(c); canvas_set_color(c,ColorBlack);
    switch(m->state){
    case AppStateWelcome:   draw_welcome(c,m);   break;
    case AppStateSending:   draw_sending(c,m);   break;
    case AppStateAsking:    draw_asking(c,m);    break;
    case AppStateSingle:    draw_single(c,m);    break;
    case AppStateNotFound:  draw_not_found(c,m); break;
    case AppStateScanning:  draw_scanning(c,m);  break;
    case AppStateCapture:   draw_capture(c,m);   break;
    case AppStateFound:     draw_found(c,m);     break;
    case AppStateDone:      draw_done(c,m);      break;
    case AppStateSaved:     draw_saved(c,m);     break;
    case AppStateRemote:    draw_remote(c,m);    break;
    }
}

/* ── IR thread ──────────────────────────────────────────────────── */
static void send_code_reps(App* app, uint8_t addr, uint8_t cmd) {
    InfraredMessage msg={.protocol=InfraredProtocolSIRC,.address=addr,.command=cmd,.repeat=false};
    for(int r=0;r<SEND_REPS&&!app->abort;r++){
        infrared_send(&msg,1);
        furi_delay_ms(INTER_FRAME_MS);
    }
}

static int32_t ir_thread_fn(void* ctx) {
    App* app=(App*)ctx;
    uint8_t fi  =app->tfi;
    uint8_t addr=FUNC_ADDRS[fi];

    if(app->thread_mode==ThreadSingle) {
        send_code_reps(app,addr,app->tlo);

    } else if(app->thread_mode==ThreadBinary) {
        uint8_t lo=app->tlo, hi=app->thi;
        uint8_t mid=(lo+hi)/2;
        for(uint8_t cmd=lo; cmd<=mid&&!app->abort; cmd++){
            app->t_prog=cmd;
            view_dispatcher_send_custom_event(app->vd,EvtProgress);
            send_code_reps(app,addr,cmd);
            if(!app->abort) furi_delay_ms(INTER_CMD_MS);
        }

    } else { /* ThreadScan */
        uint8_t cmd=CMD_MIN;
        while(!app->abort){
            app->t_prog=cmd;
            view_dispatcher_send_custom_event(app->vd,EvtProgress);
            send_code_reps(app,addr,cmd);
            if(!app->abort){
                furi_delay_ms(INTER_CMD_MS);
                if(cmd==CMD_MAX){
                    cmd=CMD_MIN;
                    view_dispatcher_send_custom_event(app->vd,EvtWrap);
                } else cmd++;
            }
        }
    }
    view_dispatcher_send_custom_event(app->vd,EvtDone);
    return 0;
}

static void start_ir_thread(App* app, uint8_t fi, uint8_t tlo, uint8_t thi, ThreadMode mode) {
    app->abort=false; app->tfi=fi; app->tlo=tlo; app->thi=thi; app->thread_mode=mode;
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

static void stop_ir_thread(App* app) { app->abort=true; join_ir_thread(app); }

/* ── State helpers ──────────────────────────────────────────────── */
static bool advance_to_next(AppModel* m) {
    m->fi++;
    if(m->fi>=NUM_FUNCS){ m->state=AppStateDone; return false; }
    m->lo=CMD_MIN; m->hi=CMD_MAX; m->round=1;
    m->scan_prog=CMD_MIN; m->pass=0;
    m->state=m->binary_mode?AppStateSending:AppStateScanning;
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

/* ── Input callback ─────────────────────────────────────────────── */
static bool input_cb(InputEvent* ev, void* ctx) {
    App* app=(App*)ctx;
    if(ev->type!=InputTypePress&&ev->type!=InputTypeRepeat) return false;

    AppModel* m=(AppModel*)view_get_model(app->view);
    bool redraw=false,handled=true,do_start=false;
    uint8_t sfi=0,slo=0,shi=0; ThreadMode smode=ThreadBinary;

    switch(m->state){

    case AppStateWelcome:
        if(ev->key==InputKeyOk){
            m->fi=0; m->lo=CMD_MIN; m->hi=CMD_MAX; m->round=1;
            m->scan_prog=CMD_MIN; m->pass=0;
            m->state=m->binary_mode?AppStateSending:AppStateScanning;
            redraw=true; do_start=true;
            sfi=0; slo=CMD_MIN; shi=CMD_MAX;
            smode=m->binary_mode?ThreadBinary:ThreadScan;
        } else if(ev->key==InputKeyUp||ev->key==InputKeyDown){
            m->binary_mode=!m->binary_mode; redraw=true;
        } else if(ev->key==InputKeyBack){
            view_commit_model(app->view,false);
            view_dispatcher_stop(app->vd); return true;
        } else handled=false;
        break;

    /* ── Binary: Sending ── */
    case AppStateSending:
        if(ev->key==InputKeyBack){
            app->abort=true; m->done[m->fi]=true; redraw=true;
            /* EvtDone will advance */
        } else handled=false;
        break;

    /* ── Binary: Asking ── */
    case AppStateAsking: {
        uint8_t mid=(m->lo+m->hi)/2;
        if(ev->key==InputKeyUp){
            m->hi=mid; m->round++; m->state=AppStateSending; redraw=true;
            do_start=true; sfi=m->fi; slo=m->lo; shi=m->hi; smode=ThreadBinary;
        } else if(ev->key==InputKeyDown){
            m->lo=(uint8_t)(mid+1); m->round++;
            if(m->lo>m->hi){ m->codes[m->fi]=-1; m->done[m->fi]=true; m->state=AppStateNotFound; }
            else { m->state=AppStateSending; do_start=true; sfi=m->fi; slo=m->lo; shi=m->hi; smode=ThreadBinary; }
            redraw=true;
        } else if(ev->key==InputKeyRight){
            app->resending=true; m->state=AppStateSending; redraw=true;
            do_start=true; sfi=m->fi; slo=m->lo; shi=m->hi; smode=ThreadBinary;
        } else if(ev->key==InputKeyBack){
            m->done[m->fi]=true;
            bool need=advance_to_next(m); redraw=true;
            if(need){ do_start=true; sfi=m->fi; slo=m->lo; shi=m->hi;
                      smode=m->binary_mode?ThreadBinary:ThreadScan; }
        } else handled=false;
        break;
    }

    /* ── Binary: Single ── */
    case AppStateSingle:
        if(ev->key==InputKeyOk){
            m->codes[m->fi]=(int32_t)m->lo; m->done[m->fi]=true;
            m->found_count++; m->anim=0; m->state=AppStateFound; redraw=true;
            notification_message(app->notif,&sequence_single_vibro);
        } else if(ev->key==InputKeyDown){
            m->codes[m->fi]=-1; m->done[m->fi]=true; m->state=AppStateNotFound; redraw=true;
        } else if(ev->key==InputKeyRight){
            app->resending=true; m->state=AppStateSending; redraw=true;
            do_start=true; sfi=m->fi; slo=m->lo; shi=m->hi; smode=ThreadBinary;
        } else if(ev->key==InputKeyBack){
            m->done[m->fi]=true;
            bool need=advance_to_next(m); redraw=true;
            if(need){ do_start=true; sfi=m->fi; slo=m->lo; shi=m->hi;
                      smode=m->binary_mode?ThreadBinary:ThreadScan; }
        } else handled=false;
        break;

    /* ── Binary: Not Found ── */
    case AppStateNotFound:
        if(ev->key==InputKeyOk||ev->key==InputKeyBack){
            bool need=advance_to_next(m); redraw=true;
            if(need){ do_start=true; sfi=m->fi; slo=m->lo; shi=m->hi;
                      smode=m->binary_mode?ThreadBinary:ThreadScan; }
        } else handled=false;
        break;

    /* ── Scan: Scanning ── */
    case AppStateScanning:
        if(ev->key==InputKeyOk){
            m->capture=app->t_prog; m->state=AppStateCapture; redraw=true;
            if(app->thread_running){ app->abort=true; app->after_done=AfterNone; }
        } else if(ev->key==InputKeyDown||ev->key==InputKeyBack){
            m->done[m->fi]=true;
            if(app->thread_running){ app->abort=true; app->after_done=AfterSkip; redraw=true; }
            else {
                bool need=advance_to_next(m); redraw=true;
                if(need){ do_start=true; sfi=m->fi; slo=CMD_MIN; shi=CMD_MAX; smode=ThreadScan; }
            }
        } else handled=false;
        break;

    /* ── Scan: Capture ── */
    case AppStateCapture:
        if(ev->key==InputKeyOk){
            m->codes[m->fi]=(int32_t)m->capture; m->done[m->fi]=true;
            m->found_count++; m->anim=0; m->state=AppStateFound; redraw=true;
            notification_message(app->notif,&sequence_single_vibro);
        } else if(ev->key==InputKeyDown){
            uint8_t next=(m->capture<CMD_MAX)?m->capture+1:CMD_MIN;
            m->state=AppStateScanning; m->scan_prog=next; redraw=true;
            if(app->thread_running){ app->after_done=AfterResume; app->resume_from=next; }
            else { do_start=true; sfi=m->fi; slo=CMD_MIN; shi=CMD_MAX; smode=ThreadScan; }
        } else if(ev->key==InputKeyUp){
            if(m->capture>CMD_MIN) m->capture--;
            redraw=true;
            if(!app->thread_running){
                do_start=true; sfi=m->fi; slo=m->capture; shi=m->capture; smode=ThreadSingle;
            }
        } else if(ev->key==InputKeyRight){
            if(!app->thread_running){
                do_start=true; sfi=m->fi; slo=m->capture; shi=m->capture; smode=ThreadSingle;
            }
        } else if(ev->key==InputKeyBack){
            m->done[m->fi]=true;
            if(app->thread_running){ app->after_done=AfterSkip; }
            else {
                bool need=advance_to_next(m); redraw=true;
                if(need){ do_start=true; sfi=m->fi; slo=CMD_MIN; shi=CMD_MAX; smode=ThreadScan; }
            }
        } else handled=false;
        break;

    /* ── Found ── */
    case AppStateFound:
        if(ev->key==InputKeyOk||ev->key==InputKeyBack){
            bool need=advance_to_next(m); redraw=true;
            if(need){ do_start=true; sfi=m->fi; slo=CMD_MIN; shi=CMD_MAX;
                      smode=m->binary_mode?ThreadBinary:ThreadScan;
                      if(m->binary_mode){ slo=m->lo; shi=m->hi; } }
        } else handled=false;
        break;

    case AppStateDone: handled=false; break;

    case AppStateSaved:
        if(ev->key==InputKeyOk&&m->found_count>0){
            m->remote_fi=0;
            while(m->remote_fi<NUM_FUNCS&&m->codes[m->remote_fi]<0) m->remote_fi++;
            if(m->remote_fi<NUM_FUNCS){ m->state=AppStateRemote; redraw=true; }
            else { view_commit_model(app->view,false); view_dispatcher_stop(app->vd); return true; }
        } else if(ev->key==InputKeyBack){
            view_commit_model(app->view,false); view_dispatcher_stop(app->vd); return true;
        }
        break;

    case AppStateRemote: {
        if(ev->key==InputKeyLeft||ev->key==InputKeyRight||ev->key==InputKeyUp){
            int8_t dir=(ev->key==InputKeyRight)?1:-1;
            uint8_t nfi=m->remote_fi;
            for(uint8_t i=0;i<NUM_FUNCS;i++){
                nfi=(uint8_t)((nfi+NUM_FUNCS+dir)%NUM_FUNCS);
                if(m->codes[nfi]>=0) break;
            }
            m->remote_fi=nfi; redraw=true;
        } else if(ev->key==InputKeyOk){
            if(m->codes[m->remote_fi]>=0&&!app->thread_running){
                do_start=true; sfi=m->remote_fi;
                slo=(uint8_t)m->codes[m->remote_fi];
                shi=slo; smode=ThreadSingle;
                notification_message(app->notif,&sequence_single_vibro);
            }
        } else if(ev->key==InputKeyDown||ev->key==InputKeyBack){
            view_commit_model(app->view,false); view_dispatcher_stop(app->vd); return true;
        } else handled=false;
        break;
    }
    } /* switch */

    if(do_start){
        view_commit_model(app->view,redraw);
        start_ir_thread(app,sfi,slo,shi,smode);
        return true;
    }
    view_commit_model(app->view,redraw);
    return handled;
}

/* ── Custom event callback ──────────────────────────────────────── */
static bool custom_event_cb(void* ctx, uint32_t ev) {
    App* app=(App*)ctx;

    if(ev==EvtProgress){
        AppModel* m=(AppModel*)view_get_model(app->view);
        if(m->binary_mode||m->state==AppStateScanning||m->state==AppStateSending)
            m->send_prog=m->scan_prog=app->t_prog;
        else m->scan_prog=app->t_prog;
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

        if(app->resending){
            app->resending=false;
            AppModel* m=(AppModel*)view_get_model(app->view);
            m->state=(m->lo==m->hi)?AppStateSingle:AppStateAsking;
            view_commit_model(app->view,true);
            return true;
        }

        AfterPlan plan=app->after_done; app->after_done=AfterNone;

        if(plan==AfterResume){
            AppModel* m=(AppModel*)view_get_model(app->view);
            m->state=AppStateScanning; m->scan_prog=app->resume_from;
            view_commit_model(app->view,true);
            start_ir_thread(app,m->fi,CMD_MIN,CMD_MAX,ThreadScan);
            return true;
        }
        if(plan==AfterSkip){
            AppModel* m=(AppModel*)view_get_model(app->view);
            if(!m->done[m->fi]) m->done[m->fi]=true;
            bool need=advance_to_next(m);
            if(need){
                ThreadMode tm=m->binary_mode?ThreadBinary:ThreadScan;
                uint8_t lo=m->binary_mode?m->lo:CMD_MIN;
                uint8_t hi=m->binary_mode?m->hi:CMD_MAX;
                view_commit_model(app->view,true);
                start_ir_thread(app,m->fi,lo,hi,tm);
            } else {
                view_commit_model(app->view,true);
                save_from_model(app);
            }
            return true;
        }

        /* AfterNone — binary EvtDone or scan aborted for capture */
        AppModel* m=(AppModel*)view_get_model(app->view);

        if(m->state==AppStateDone){
            view_commit_model(app->view,true); save_from_model(app); return true;
        }
        /* Binary search advance */
        if(m->state==AppStateSending){
            if(app->abort&&m->done[m->fi]){
                app->abort=false;
                bool need=advance_to_next(m);
                if(need){
                    ThreadMode tm=m->binary_mode?ThreadBinary:ThreadScan;
                    uint8_t lo=m->binary_mode?m->lo:CMD_MIN;
                    uint8_t hi=m->binary_mode?m->hi:CMD_MAX;
                    view_commit_model(app->view,true);
                    start_ir_thread(app,m->fi,lo,hi,tm);
                    return true;
                }
            } else if(m->lo==m->hi){
                m->state=AppStateSingle;
                notification_message(app->notif,&sequence_single_vibro);
            } else {
                m->state=AppStateAsking;
                notification_message(app->notif,&sequence_single_vibro);
            }
        }

        if(m->state==AppStateDone){
            view_commit_model(app->view,true); save_from_model(app); return true;
        }
        view_commit_model(app->view, m->state!=AppStateRemote||true);
        return true;
    }
    return true;
}

/* ── Tick ───────────────────────────────────────────────────────── */
static void tick_cb(void* ctx) {
    App* app=(App*)ctx;
    AppModel* m=(AppModel*)view_get_model(app->view);
    m->anim++;
    if(m->state==AppStateDone){
        view_commit_model(app->view,true); save_from_model(app); return;
    }
    view_commit_model(app->view,true);
}

static bool nav_cb(void* ctx) {
    App* app=(App*)ctx; view_dispatcher_stop(app->vd); return true;
}

/* ── Entry point ────────────────────────────────────────────────── */
int32_t sony_ir_search_app(void* p) {
    UNUSED(p);
    App* app=malloc(sizeof(App)); memset(app,0,sizeof(App));

    app->view=view_alloc();
    view_allocate_model(app->view,ViewModelTypeLocking,sizeof(AppModel));
    view_set_context(app->view,app);
    view_set_draw_callback(app->view,draw_cb);
    view_set_input_callback(app->view,input_cb);

    AppModel* m=(AppModel*)view_get_model(app->view);
    m->state=AppStateWelcome; m->binary_mode=true; /* default: binary */
    m->lo=CMD_MIN; m->hi=CMD_MAX; m->round=1;
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
    free(app); return 0;
}
