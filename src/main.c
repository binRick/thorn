// THORN - a cinematic-platformer tribute to Blackthorne (1994), built on raylib 6.0.
//
// Original game: faithful to the *mechanics* of the genre - weighty, no-jump
// traversal; a pump-shotgun that fires forward AND over the shoulder; ducking
// into background shadow alcoves so bullets pass. Original naming/art. See DESIGN.md.
//
// Conventions shared with ../Chernobyl, ../Chernobyl2, ../uapd:
//   - run.sh builds + runs (always with --debug).
//   - --debug streams newline-delimited JSON to ./thorn-debug.log: a recurring
//     ~5 Hz "state" snapshot plus discrete events. Reconstruct any bug from it.
//   - ASCII-only DrawText (raylib's default font is ASCII).
//
// M1: rooms load from levels/<area>/<room>.lvl; doors form a room graph; keys
// open locked doors; levers extend bridges; checkpoints restore on death.
//
// Controls: A/D or arrows move - W/Up context (climb / shadow / use door / talk /
//           lever) - S/Down (climb down / leave shadow) - Shift walk
//           Space/J fire forward - K fire BACKWARD - P pause - ` or Tab overlay
//           R respawn(dev) - G god(dev) - H hitboxes(dev) - Esc quit
// CLI: --debug --headless --selftest --no-enemies --god --demo
//      --room PATH --spawn ID --rate N --frames N --shot N
// ----------------------------------------------------------------------------
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#ifdef __EMSCRIPTEN__
  #include <emscripten/emscripten.h>   // browser drives the main loop
#endif

#if defined(RAYLIB_VERSION_MAJOR) && RAYLIB_VERSION_MAJOR >= 6
  #define THORN_RAYLIB "6.0"
#else
  #define THORN_RAYLIB "5.x"
#endif

// ---- Debug JSON logging (mirrors the sibling projects) ----------------------
static int    g_debug    = 0;
static FILE  *g_dbg      = NULL;
static int    g_headless = 0;   // run the sim with no window (deterministic capture)
static double g_simTime  = 0;   // sim clock used for timestamps when headless
static double nowT(void){ return g_headless ? g_simTime : GetTime(); }

static void DebugLog(const char *ev, const char *fmt, ...) {
    if (!g_debug || !g_dbg) return;
    fprintf(g_dbg, "{\"t\":%.3f,\"ev\":\"%s\"", nowT(), ev);
    if (fmt && fmt[0]) { va_list ap; va_start(ap, fmt); fputc(',', g_dbg); vfprintf(g_dbg, fmt, ap); va_end(ap); }
    fprintf(g_dbg, "}\n"); fflush(g_dbg);
}
// Escape a string for JSON. Rotating buffers so several JStr() calls can appear
// in one printf without aliasing.
static const char *JStr(const char *s){
    static char bufs[4][96]; static int bi=0; char*b=bufs[bi=(bi+1)&3]; size_t j=0; if(!s)s="";
    for(size_t i=0;s[i]&&j<94;i++){char c=s[i]; if(c=='"'||c=='\\'){b[j++]='\\';b[j++]=c;} else if((unsigned char)c>=0x20)b[j++]=c;}
    b[j]=0; return b;
}

// ---- Config -----------------------------------------------------------------
#define SCREEN_W 1280
#define SCREEN_H 720
#define TILE     32
#define DT       (1.0f/120.0f)

#define PW 20.0f
#define PH 46.0f
#define EW 22.0f
#define EH 46.0f

#define GRAV       2000.0f
#define MAXFALL    1400.0f
#define ACCEL      1500.0f
#define RUN_SPD    210.0f
#define WALK_SPD   135.0f
#define FRICTION   1700.0f
#define FALL_HURT  980.0f
#define JUMP_V     540.0f          // jump impulse (~2.3 tiles high)

#define P_HP_MAX   100
#define P_RANGE    380.0f
#define P_DMG      34
#define PUMP       0.55f
#define IFRAMES    0.60f
#define SPIKE_DMG  12

#define MAG_MAX       6      // shells per magazine
#define RESERVE_START 12     // spare shells at game start
#define RELOAD_T      1.0f   // pump-reload time (blocks firing)
#define AMMO_BOX      8      // shells per ammo pickup
#define DMG_PER_POW   12     // damage added per power upgrade
#define SPD_PER_LVL   0.09f  // pump cooldown removed per speed upgrade
#define PUMP_MIN      0.22f  // fastest possible pump

#define BOMB_FUSE     1.4f
#define BOMB_RADIUS   76.0f
#define BOMB_DMG      55

#define E_HP       60
#define E_RANGE    470.0f
#define E_DMG      10
#define E_INTERVAL 1.5f
#define E_AGGRO    560.0f

#define MAXEN  8
#define MAXPK  24
#define MAXNPC 4
#define MAXSHOT 48

// ---- Entities ---------------------------------------------------------------
// type: 0=SKARL (stationary shooter) 1=BRUTE (advances) 2=SENTRY (uses cover)
typedef struct { float x,y,vx,vy; int face; int hp; int maxhp; int alive; int type; int phase; int onGround; int inCover; float coverT; float fireT; float hitFlash; const char*st; } Enemy;
typedef struct { int c,r; char kind; int alive; } Pickup;     // H K B * a u U
typedef struct { float x,y,vx,vy,fuse; int active; } Bomb;    // placed or thrown explosive
typedef struct { int c,r; int freed; } Npc;
typedef struct { float x1,y1,x2,y2; float age; int owner; } Shot; // owner 0=player 1=enemy

static struct {
    float x,y,vx,vy;
    int   face;
    int   onGround, inCover;
    float iframes, fireCD;
    float muzzle; int muzzleDir;
    float climbT; int climbDir;
    int   turning;
    int   hp, mag, reserve, keys, bombs, shards;
    int   gunPow, gunSpd; float reloadT;
    int   onLift;          // index of the lift the player is riding, or -1
    float hurtT;           // flinch-animation timer
    float meleeCD,meleeT;  // knife cooldown + swing timer
    float coyote,jumpBuf;  // forgiving-jump windows
    int   dead; float deadT;
} P;

static Enemy  g_en[MAXEN];   static int g_enN=0;
static Pickup g_pk[MAXPK];   static int g_pkN=0;
static Npc    g_npc[MAXNPC]; static int g_npcN=0;
static Shot   g_shot[MAXSHOT]; static int g_shotHead=0;
#define MAXBOMB 4
static Bomb   g_bomb[MAXBOMB];
static float  g_boomT=0, g_boomX=0, g_boomY=0;   // explosion FX timer/position
// Moving platform that carries the player. Oscillates base..base+amp along axis.
typedef struct { float bx,by,w,h; int axis; float amp,omega,phase; float x,y,dx,dy; } Lift;
#define MAXLIFT 4
static Lift   g_lifts[MAXLIFT]; static int g_liftN=0;

// ---- FX: particles, screen-shake, lighting (cosmetic; windowed-only) --------
typedef struct { float x,y,vx,vy,life,max,size; unsigned char r,g,b; int add,grav; } Part;
#define MAXPART 512
static Part g_part[MAXPART]; static int g_partHead=0;
static int   g_fx=1; static float g_shake=0;
static Texture2D g_tLight, g_tVign;   // soft radial light + vignette (built in InitSprites)
typedef struct { float x,y,life; int kind; char s[16]; } FloatTxt;   // rising labels: kind 0=pickup, 1=damage
#define MAXFLOATTXT 16
static FloatTxt g_float[MAXFLOATTXT]; static int g_floatHead=0;
static void FloatPush(float x,float y,int kind,const char*s){ if(!g_fx||g_headless) return; FloatTxt*f=&g_float[g_floatHead]; g_floatHead=(g_floatHead+1)%MAXFLOATTXT; f->x=x; f->y=y; f->life=1.1f; f->kind=kind; snprintf(f->s,sizeof f->s,"%s",s); }
static void FloatSpawn(float x,float y,const char*s){ FloatPush(x,y,0,s); }
static void FloatDmg(float x,float y,int dmg){ FloatPush(x,y,1,TextFormat("%d",dmg)); }
static void Emit(float x,float y,int n,float spd,float life,float size,Color c,int add,int grav){
    if(!g_fx||g_headless) return;
    for(int k=0;k<n;k++){ Part*p=&g_part[g_partHead]; g_partHead=(g_partHead+1)%MAXPART;
        float a=(rand()%628)/100.0f, s=spd*(0.35f+(rand()%100)/100.0f);
        p->x=x; p->y=y; p->vx=cosf(a)*s; p->vy=sinf(a)*s-(grav?spd*0.35f:0);
        p->life=p->max=life*(0.6f+(rand()%80)/100.0f); p->size=size;
        p->r=c.r; p->g=c.g; p->b=c.b; p->add=add; p->grav=grav; }
}
static void UpdateParticles(void){
    if(g_headless) return;
    if(g_shake>0){ g_shake-=DT*38.0f; if(g_shake<0) g_shake=0; }
    for(int i=0;i<MAXPART;i++){ Part*p=&g_part[i]; if(p->life<=0) continue;
        p->life-=DT; if(p->grav) p->vy+=900.0f*DT; p->x+=p->vx*DT; p->y+=p->vy*DT; p->vx*=0.985f; }
    for(int i=0;i<MAXFLOATTXT;i++) if(g_float[i].life>0){ g_float[i].life-=DT*1.25f; g_float[i].y-=22.0f*DT; }
}

// ---- World grid -------------------------------------------------------------
// Rooms load from levels/<area>/<room>.lvl (format in DESIGN.md). A built-in
// fallback boots if a file is missing.
static char g_tiles[64][128];      // '#' solid, '.' air
static int  g_alcove[64][128];     // shadow-cover cells
static int  g_spike[64][128];      // hazard cells
static int  g_bridge[64][128];     // bridge cells: solid only while a lever is thrown
static int  g_crack[64][128];      // cracked walls: solid, but a bomb opens them
static int  g_W=0, g_H=0;
static int  g_bridgeOn=0;          // M1: every lever in a room drives its bridge group

static int SolidAt(int c,int r){
    if (c<0 || c>=g_W || r<0) return 1;     // walls + ceiling
    if (r>=g_H) return 0;                    // open void below the level (fatal)
    if (g_bridge[r][c]) return g_bridgeOn;   // bridge extends when the lever is on
    return g_tiles[r][c]=='#';
}

// ---- Rooms: doors, levers/bridges, keys, checkpoints ------------------------
#define MAXDOOR 8
#define MAXLEVER 4
#define MAXCP   6
#define MAXKEY  6
typedef struct { int c,r,id; char target[48]; int targetSpawn; int locked; char key[12]; int needShards; } Door;
typedef struct { int c,r; } Lever;
typedef struct { int c,r,hit; } CheckMark;

static Door      g_doors[MAXDOOR];   static int g_doorN=0;
static Lever     g_levers[MAXLEVER]; static int g_leverN=0;
static CheckMark g_cps[MAXCP];       static int g_cpN=0;
static char      g_npcGift[MAXNPC][40];   // per-NPC gift ("ammo"/"key"/"bomb"/"hint <text>")
static struct { char color[12]; int count; } g_keys[MAXKEY]; static int g_keyN=0;

static char g_areaName[48]="Sunken Mines", g_roomName[48]="-", g_roomPath[160]="";
static float g_areaCardT=0;                       // "now entering <area>" banner timer
static char g_areaSeen[8][48]; static int g_areaSeenN=0;   // areas already announced this game
static char g_cpPath[160]=""; static float g_cpX=0,g_cpY=0; static int g_cpFace=1;   // checkpoint
static int  g_pendActive=0, g_pendSpawn=-1; static char g_pendTarget[48]="";          // deferred door
static int  g_areaClear=0, g_victory=0;
static char g_curPassword[16]="";          // password of the current room (if any)
static char g_msg[96]=""; static float g_msgT=0;   // transient on-screen banner (hints, passwords)
static Vector2 g_cam={0,0}; static int g_camInit=0;   // g_camInit=0 -> snap (set on room load)

static int  KeyCount(const char*c){ for(int i=0;i<g_keyN;i++) if(!strcmp(g_keys[i].color,c)) return g_keys[i].count; return 0; }
static void KeyAdd(const char*c){ for(int i=0;i<g_keyN;i++) if(!strcmp(g_keys[i].color,c)){ g_keys[i].count++; return; } if(g_keyN<MAXKEY){ snprintf(g_keys[g_keyN].color,12,"%s",c); g_keys[g_keyN].count=1; g_keyN++; } }
static int  KeyTotal(void){ int n=0; for(int i=0;i<g_keyN;i++) n+=g_keys[i].count; return n; }
static void Msg(float secs,const char*fmt,...){ va_list ap; va_start(ap,fmt); vsnprintf(g_msg,sizeof g_msg,fmt,ap); va_end(ap); g_msgT=secs; }

// ---- Flags / dev ------------------------------------------------------------
static int g_noEnemies=0, g_god=0, g_demo=0, g_paused=0, g_overlay=0, g_hitboxes=0, g_selftest=0;
static int g_rate=24, g_maxFrames=0, g_shotFrame=0, g_startSpawn=-1;
static long g_frame=0; static int g_won=0;
enum { SCENE_TITLE, SCENE_PLAY }; static int g_scene=SCENE_TITLE, g_menuSel=0, g_skiptitle=0;
static int g_pauseSel=0; static const char*g_pauseItems[3]={"Resume","Restart checkpoint","Quit to title"};
static int g_diff=1; static const char*g_diffName[3]={"Easy","Normal","Hard"}; static const float g_diffMul[3]={0.6f,1.0f,1.5f};
static char g_roomStart[160]="levels/sunken_mines/entrance.lvl";
static const struct { const char*code; const char*path; } g_pwTable[] = {   // --password
    {"MINE","levels/sunken_mines/entrance.lvl"}, {"MIRE","levels/the_mire/entrance.lvl"},
    {"ASH","levels/the_ashlands/entrance.lvl"},  {"KEEP","levels/the_usurpers_keep/entrance.lvl"},
};

static char g_evlog[8][80]; static int g_evHead=0;
static void Ev(const char*fmt,...){ va_list ap; va_start(ap,fmt); vsnprintf(g_evlog[g_evHead],sizeof g_evlog[0],fmt,ap); va_end(ap); g_evHead=(g_evHead+1)%8; }

// ---- Input ------------------------------------------------------------------
typedef struct { int left,right,walk;            // held
                 int up,down,fireF,fireB,use,cycle,jump,melee,throwb; } Input; // edge

static Input KeyInput(void){
    Input in={0};
    in.left  = IsKeyDown(KEY_LEFT)||IsKeyDown(KEY_A);
    in.right = IsKeyDown(KEY_RIGHT)||IsKeyDown(KEY_D);
    in.walk  = IsKeyDown(KEY_LEFT_SHIFT)||IsKeyDown(KEY_RIGHT_SHIFT);
    in.up    = IsKeyPressed(KEY_UP)||IsKeyPressed(KEY_W);
    in.down  = IsKeyPressed(KEY_DOWN)||IsKeyPressed(KEY_S);
    in.fireF = IsKeyPressed(KEY_J)||IsKeyPressed(KEY_LEFT_CONTROL);
    in.fireB = IsKeyPressed(KEY_K);
    in.jump  = IsKeyPressed(KEY_SPACE);
    in.melee = IsKeyPressed(KEY_V);
    in.throwb= IsKeyPressed(KEY_T);
    in.use   = IsKeyPressed(KEY_E)||IsKeyPressed(KEY_ENTER);
    in.cycle = IsKeyPressed(KEY_Q);
    return in;
}
static Input DemoInput(void){
    static double fT=0,uT=0,bT=0; double ft=GetFrameTime();
    Input in={0}; in.right=1; fT+=ft; uT+=ft; bT+=ft;
    if(fT>0.8){ in.fireF=1; fT=0; }
    if(uT>1.3){ in.up=1;    uT=0; }
    if(bT>3.7){ in.fireB=1; bT=0; }
    return in;
}
static Input DemoFrameInput(long f){
    // Walk right and tap "up" often so the capture exercises levers, doors and
    // climbs as the bot passes them. Deterministic (a function of frame count).
    Input in={0}; in.right=1; in.walk=1;   // hold Shift = run (covers ground like before the invert)
    if(f%96==1)  in.fireF=1;
    if(f%22==3)  in.up=1;     // ~5.5 Hz: throws levers / uses doors / climbs in passing
    if(f%440==220) in.fireB=1;
    if(f%80==10)   in.melee=1; // occasional knife swing
    if(f%520==200) in.throwb=1; // occasional bomb toss
    if(f%600==90)  in.use=1;  // occasionally drop a bomb (exercises the explosive path)
    if(f%140==30)  in.jump=1; // occasional jump (exercises the airborne path)
    return in;
}

// ---- Helpers ----------------------------------------------------------------
static inline float clampf(float v,float a,float b){ return v<a?a:(v>b?b:v); }
static inline int   sgn(float v){ return v>0?1:(v<0?-1:0); }
static inline float pcx(void){ return P.x+PW*0.5f; }
static inline float pcy(void){ return P.y+PH*0.5f; }
static inline float gunY(void){ return P.y+PH*0.42f; }

static int LineClear(float x1,float x2,float y){
    int r=(int)floorf(y/TILE), a=(int)floorf(fminf(x1,x2)/TILE), b=(int)floorf(fmaxf(x1,x2)/TILE);
    for(int c=a;c<=b;c++) if(SolidAt(c,r)) return 0;
    return 1;
}

// Canonicalize a path (collapse "a/b/../c" -> "a/c") so the same room reached by
// different relative paths shares one key for the collected-set + checkpoints.
static void NormalizePath(char*out,size_t n,const char*in){
    char tmp[256]; snprintf(tmp,sizeof tmp,"%s",in);
    const char* parts[80]; int np=0; char* tok=strtok(tmp,"/");
    while(tok){ if(!strcmp(tok,".")){} else if(!strcmp(tok,"..")){ if(np>0&&strcmp(parts[np-1],"..")) np--; else parts[np++]=tok; } else parts[np++]=tok; tok=strtok(NULL,"/"); }
    out[0]=0; for(int i=0;i<np;i++){ if(i&&strlen(out)<n-1) strncat(out,"/",n-strlen(out)-1); strncat(out,parts[i],n-strlen(out)-1); }
}
// One-time pickups/NPCs: remember (room,cell) so they don't respawn on re-entry.
#define MAXCOLL 256
static struct { char room[80]; int c,r; } g_coll[MAXCOLL]; static int g_collN=0;
static int  IsCollected(const char*room,int c,int r){ for(int i=0;i<g_collN;i++) if(g_coll[i].c==c&&g_coll[i].r==r&&!strcmp(g_coll[i].room,room)) return 1; return 0; }
static void MarkCollected(const char*room,int c,int r){ if(g_collN<MAXCOLL&&!IsCollected(room,c,r)){ snprintf(g_coll[g_collN].room,80,"%s",room); g_coll[g_collN].c=c; g_coll[g_collN].r=r; g_collN++; } }

static void ResolveRoomPath(char*out,size_t n,const char*base,const char*target){
    char dir[160]; snprintf(dir,sizeof dir,"%s",base);
    char*s=strrchr(dir,'/'); if(s)*s=0; else snprintf(dir,sizeof dir,".");
    snprintf(out,n,"%s/%s.lvl",dir,target);
}

// Built-in fallback room (used if a .lvl file is missing) - the v0.1 slice room.
static const char *FALLBACK_ROOM =
    "@area Sunken Mines\n@room fallback\n@door 0 exit 0\n"
    "############################################\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#..........................................#\n"
    "#................................g..*..0...#\n"
    "#.P.S.H..n..g..^^^...L..B..K..###########..#\n"
    "##########################################.#\n"
    "##########################################.#\n"
    "##########################################.#\n";

static void AddEnemy(int c, float feet, int type){
    if(g_enN>=MAXEN) return; Enemy*e=&g_en[g_enN++];
    e->x=c*TILE+(TILE-EW)/2; e->y=feet-EH; e->vx=e->vy=0; e->face=-1;
    e->type=type; e->hp = type==3?300 : type==1?110 : type==2?45 : E_HP;   // boss/brute tanky, sentry fragile
    e->maxhp=e->hp;
    e->alive=1; e->phase=0; e->onGround=1; e->inCover=0; e->coverT=0;
    e->fireT=E_INTERVAL*0.5f; e->hitFlash=0; e->st="IDLE";
}

static void ParseRoom(const char*text,int spawnDoor){
    for(int r=0;r<64;r++) for(int c=0;c<128;c++){ g_tiles[r][c]='.'; g_alcove[r][c]=g_spike[r][c]=g_bridge[r][c]=g_crack[r][c]=0; }
    g_enN=g_pkN=g_npcN=g_doorN=g_leverN=g_cpN=g_liftN=0; g_bridgeOn=0; g_W=g_H=0;
    for(int i=0;i<MAXBOMB;i++) g_bomb[i].active=0; g_boomT=0;
    for(int i=0;i<MAXNPC;i++) g_npcGift[i][0]=0; g_curPassword[0]=0;
    int spawnC=-1,spawnR=-1, npcMeta=0;
    struct { int used; char target[48]; int spawn; int locked; char key[12]; int needShards; } dm[10];
    for(int i=0;i<10;i++){ dm[i].used=0; dm[i].target[0]=0; dm[i].spawn=0; dm[i].locked=0; dm[i].key[0]=0; dm[i].needShards=0; }

    static char rows[64][128]; int nrows=0; int inGrid=0;
    const char*p=text; char line[256];
    while(*p){
        int L=0; while(*p && *p!='\n' && L<255){ line[L++]=*p++; } line[L]=0; if(*p=='\n')p++;
        while(L>0 && (line[L-1]=='\r'||line[L-1]==' ')) line[--L]=0;
        if(!inGrid){
            if(line[0]=='@'){
                if(!strncmp(line,"@area ",6)) snprintf(g_areaName,sizeof g_areaName,"%s",line+6);
                else if(!strncmp(line,"@room ",6)) snprintf(g_roomName,sizeof g_roomName,"%s",line+6);
                else if(!strncmp(line,"@door ",6)){ int id=0,sp=0; char tg[48]="",lw[12]="",cl[12]="";
                    int got=sscanf(line+6,"%d %47s %d %11s %11s",&id,tg,&sp,lw,cl);
                    if(id>=0&&id<10&&got>=3){ dm[id].used=1; snprintf(dm[id].target,48,"%s",tg); dm[id].spawn=sp;
                        if(got>=5&&!strcmp(lw,"lock")){ dm[id].locked=1; snprintf(dm[id].key,12,"%s",cl);}
                        else if(got>=5&&!strcmp(lw,"shards")){ dm[id].needShards=atoi(cl);} } }
                else if(!strncmp(line,"@lift ",6)){ int c=0,r=0,w=2; char ax[8]="v"; float rng=4,per=3;
                    if(sscanf(line+6,"%d %d %d %7s %f %f",&c,&r,&w,ax,&rng,&per)>=6 && g_liftN<MAXLIFT){ Lift*L=&g_lifts[g_liftN++];
                        L->bx=c*TILE; L->by=r*TILE; L->w=w*TILE; L->h=14; L->axis=ax[0]=='v'?1:0; L->amp=rng*TILE; L->omega=6.2831853f/(per>0.1f?per:0.1f); L->phase=0; L->x=L->bx; L->y=L->by; L->dx=L->dy=0; } }
                else if(!strncmp(line,"@npc ",5)){ if(npcMeta<MAXNPC){ snprintf(g_npcGift[npcMeta],40,"%s",line+5); npcMeta++; } }   // gift for the Nth 'n'
                else if(!strncmp(line,"@password ",10)) snprintf(g_curPassword,sizeof g_curPassword,"%s",line+10);
                continue;   // @lever is documentation; grid 'L'/'b' drive the bridge
            }
            if(line[0]==0 || line[0]==';') continue;
            inGrid=1;
        }
        if(line[0] && nrows<64){ snprintf(rows[nrows],128,"%s",line); nrows++; }
    }
    g_H=nrows; for(int r=0;r<nrows;r++){ int len=(int)strlen(rows[r]); if(len>g_W) g_W=len; }
    if(g_W>127) g_W=127;

    for(int r=0;r<g_H;r++){
        int len=(int)strlen(rows[r]);
        for(int c=0;c<g_W;c++){
            char t = c<len?rows[r][c]:'.';
            float feet=(r+1)*TILE;
            switch(t){
                case '#': g_tiles[r][c]='#'; break;
                case 'x': g_tiles[r][c]='#'; g_crack[r][c]=1; break;   // cracked (bombable) wall
                case 'b': g_bridge[r][c]=1; break;
                case 'S': g_alcove[r][c]=1; break;
                case '^': g_spike[r][c]=1; break;
                case 'C': if(g_cpN<MAXCP) g_cps[g_cpN++]=(CheckMark){c,r,0}; break;
                case 'P': spawnC=c; spawnR=r; break;
                case 'g': AddEnemy(c,feet,0); break;   // SKARL: stationary shooter
                case 'G': AddEnemy(c,feet,1); break;   // BRUTE: advancing melee
                case 's': AddEnemy(c,feet,2); break;   // SENTRY: shoots, then ducks into cover
                case 'M': AddEnemy(c,feet,3); break;   // MALDRAK: the boss
                case 'n': if(g_npcN<MAXNPC) g_npc[g_npcN++]=(Npc){c,r,IsCollected(g_roomPath,c,r)?1:0}; break;
                case 'H': case 'B': case '*': case 'K': case 'a': case 'u': case 'U': if(g_pkN<MAXPK && !IsCollected(g_roomPath,c,r)) g_pk[g_pkN++]=(Pickup){c,r,t,1}; break;
                case 'L': if(g_leverN<MAXLEVER) g_levers[g_leverN++]=(Lever){c,r}; break;
                case '0':case '1':case '2':case '3':case '4':case '5':case '6':case '7':case '8':case '9':{
                    int id=t-'0'; if(g_doorN<MAXDOOR){ Door*D=&g_doors[g_doorN++]; D->c=c; D->r=r; D->id=id;
                        if(dm[id].used){ snprintf(D->target,48,"%s",dm[id].target); D->targetSpawn=dm[id].spawn; D->locked=dm[id].locked; snprintf(D->key,12,"%s",dm[id].key); D->needShards=dm[id].needShards; }
                        else { D->target[0]=0; D->targetSpawn=0; D->locked=0; D->key[0]=0; D->needShards=0; } }
                } break;
                default: break;
            }
        }
    }

    if(spawnDoor==-2){ /* keep current position (caller restores, e.g. checkpoint) */ }
    else {
        int placed=0;
        if(spawnDoor>=0) for(int i=0;i<g_doorN;i++) if(g_doors[i].id==spawnDoor){ Door*D=&g_doors[i];
            P.x=D->c*TILE+(TILE-PW)/2; P.y=(D->r+1)*TILE-PH; P.face=(D->c<g_W/2)?1:-1; placed=1; break; }
        if(!placed){ if(spawnC<0){ spawnC=2; spawnR=g_H-4; } P.x=spawnC*TILE+(TILE-PW)/2; P.y=(spawnR+1)*TILE-PH; P.face=1; }
        P.vx=P.vy=0; P.onGround=1; P.inCover=0; P.climbT=0; P.dead=0; P.deadT=0; P.iframes=0; P.muzzle=0; P.onLift=-1;
    }
}

static void LoadRoom(const char*path,int spawnDoor,int setCheckpoint){
    static char buf[1<<15];
    NormalizePath(g_roomPath,sizeof g_roomPath,path);   // canonical key (set before parse)
    FILE*f=fopen(path,"r");
    if(f){ size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f); ParseRoom(buf,spawnDoor); }
    else { ParseRoom(FALLBACK_ROOM,spawnDoor); DebugLog("warn","\"msg\":\"room file missing; using fallback\",\"path\":\"%s\"",JStr(path)); }
    if(setCheckpoint){ snprintf(g_cpPath,sizeof g_cpPath,"%s",g_roomPath); g_cpX=P.x; g_cpY=P.y; g_cpFace=P.face; }
    DebugLog("level","\"area\":\"%s\",\"room\":\"%s\",\"w\":%d,\"h\":%d,\"enemies\":%d,\"doors\":%d,\"pickups\":%d,\"levers\":%d",
             JStr(g_areaName),JStr(g_roomName),g_W,g_H,g_enN,g_doorN,g_pkN,g_leverN);
    g_camInit=0;   // snap the camera to the hero on the first frame of the new room
    {   // first time in this area this game -> announce it with a title card
        int seen=0; for(int i=0;i<g_areaSeenN;i++) if(!strcmp(g_areaSeen[i],g_areaName)){ seen=1; break; }
        if(!seen){
            if(g_areaSeenN<8) snprintf(g_areaSeen[g_areaSeenN++],48,"%s",g_areaName);
            g_areaCardT=2.8f;
            DebugLog("areacard","\"area\":\"%s\"",JStr(g_areaName));
        }
    }
    if(g_curPassword[0]){   // area-entrance room: save progress + show the password
        FILE*sf=fopen("thorn-save.txt","w"); if(sf){ fprintf(sf,"%s\n%s\n",g_curPassword,g_roomPath); fclose(sf); }
        Msg(3.5f,"PASSWORD: %s  (progress saved)",g_curPassword);
        DebugLog("password","\"code\":\"%s\",\"path\":\"%s\"",JStr(g_curPassword),JStr(g_roomPath));
    }
}

static void RespawnAtCheckpoint(void){
    P.hp=P_HP_MAX; P.dead=0; P.deadT=0; P.iframes=0; P.vx=P.vy=0; P.mag=MAG_MAX; P.reloadT=0;
    if(g_cpPath[0]){ LoadRoom(g_cpPath,-2,0); P.x=g_cpX; P.y=g_cpY; P.face=g_cpFace; P.onGround=1; P.inCover=0; P.climbT=0; }
    DebugLog("respawn","\"room\":\"%s\",\"x\":%.1f,\"y\":%.1f",JStr(g_roomName),P.x,P.y); Ev("respawn @ %s",g_roomName);
}

static void NewGame(void){
    P.hp=P_HP_MAX; P.mag=MAG_MAX; P.reserve=(g_diff==0?18:g_diff==2?8:RESERVE_START); P.gunPow=0; P.gunSpd=0; P.reloadT=0;
    P.bombs=1; P.shards=0; P.keys=0; g_keyN=0; g_collN=0; g_won=0; g_areaClear=0; g_victory=0;
    g_areaSeenN=0; g_areaCardT=0;   // re-announce areas for a fresh playthrough
    LoadRoom(g_roomStart,g_startSpawn,1);
}
static void ContinueGame(void){   // resume from the saved area entrance (password system)
    char path[160]=""; FILE*sf=fopen("thorn-save.txt","r");
    if(sf){ char code[32]=""; if(fscanf(sf,"%31s %159s",code,path)!=2) path[0]=0; fclose(sf); }
    if(path[0]) snprintf(g_roomStart,sizeof g_roomStart,"%s",path);
    NewGame();
}

// Persist player-chosen options (difficulty, FX) so they stick across launches.
static void SaveOptions(void){
    FILE*f=fopen("options.txt","w");
    if(f){ fprintf(f,"diff %d\nfx %d\n",g_diff,g_fx); fclose(f); }
}
static void LoadOptions(void){
    FILE*f=fopen("options.txt","r"); if(!f) return;
    char k[16]; int v;
    while(fscanf(f,"%15s %d",k,&v)==2){
        if(!strcmp(k,"diff")&&v>=0&&v<=2) g_diff=v;
        else if(!strcmp(k,"fx")) g_fx=v?1:0;
    }
    fclose(f);
}

static void SpawnShot(float x1,float y1,float x2,float y2,int owner){
    g_shot[g_shotHead]=(Shot){x1,y1,x2,y2,0,owner}; g_shotHead=(g_shotHead+1)%MAXSHOT;
}

static const char* PStateName(void){
    if(P.dead) return "DEAD";
    if(P.climbT>0) return P.climbDir>0?"CLIMB_UP":"CLIMB_DOWN";
    if(P.inCover) return "COVER";
    if(P.reloadT>0) return "RELOAD";
    if(P.muzzle>0) return P.muzzleDir==P.face?"FIRE_FWD":"FIRE_BACK";
    if(!P.onGround) return P.vy<0?"JUMP":"FALL";
    if(P.turning) return "TURN";
    if(fabsf(P.vx)>WALK_SPD+10) return "RUN";
    if(fabsf(P.vx)>5) return "WALK";
    return "IDLE";
}

// ---- Combat -----------------------------------------------------------------
enum { SND_FIRE, SND_DRY, SND_RELOAD, SND_ENEMYFIRE, SND_HIT, SND_DEATH, SND_PICKUP, SND_LEVER, SND_BOMB, SND_UPGRADE, SND_JUMP, SND_MELEE, SND_STEP, SND_LAND, SND_PORTAL, SND_N };
static int   g_audio=0; static Sound g_snd[SND_N];   // filled by InitAudio() (Chunk E); no-op until then
static Music g_runMus, g_walkMus; static int g_runMusOk=0, g_walkMusOk=0;   // looping run/walk footstep samples
#define MAX_DEATHSND 8
static Sound g_deathSnd[MAX_DEATHSND]; static int g_deathSndN=0;   // enemy-death samples (random pick); 0 -> synth
static Sound g_hurtSnd[MAX_DEATHSND];  static int g_hurtSndN=0;     // player-hurt samples (random pick); 0 -> SND_HIT
static void SndPlay(int id){ if(g_audio && id>=0 && id<SND_N) PlaySound(g_snd[id]); }
static void PlayEnemyDeath(void){ if(!g_audio) return; if(g_deathSndN>0) PlaySound(g_deathSnd[rand()%g_deathSndN]); else SndPlay(SND_DEATH); }
static void PlayPlayerHurt(void){ if(!g_audio) return; if(g_hurtSndN>0) PlaySound(g_hurtSnd[rand()%g_hurtSndN]); else SndPlay(SND_HIT); }

static void Fire(int dir){
    if(P.fireCD>0 || P.reloadT>0) return;
    if(P.mag<=0){
        if(P.reserve>0){ P.reloadT=RELOAD_T; SndPlay(SND_RELOAD); DebugLog("reload","\"start\":true,\"reserve\":%d",P.reserve); Ev("reloading..."); }
        else { P.fireCD=0.25f; SndPlay(SND_DRY); DebugLog("fire","\"dir\":\"%s\",\"dry\":true",dir==P.face?"fwd":"back"); Ev("*click* - no shells"); }
        return;
    }
    int dmg = P_DMG + P.gunPow*DMG_PER_POW;
    float pump = fmaxf(PUMP_MIN, PUMP - P.gunSpd*SPD_PER_LVL);
    P.fireCD=pump; P.muzzle=0.09f; P.muzzleDir=dir; P.mag--; SndPlay(SND_FIRE);
    float mx = dir>0 ? P.x+PW : P.x, my = gunY();
    int hitIdx=-1; float best=P_RANGE;
    for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive||e->inCover) continue;   // enemies in cover are safe
        float ex=e->x+EW*0.5f, ey=e->y+EH*0.5f, d=(ex-mx)*dir;
        if(d>0 && d<best && fabsf(ey-my)<TILE*0.6f && LineClear(mx,ex,my)){ best=d; hitIdx=i; }
    }
    float endx = mx + dir*(hitIdx>=0?best:P_RANGE);
    SpawnShot(mx,my,endx,my,0);
    Emit(mx,my,7,360,0.16f,2.2f,(Color){255,220,130,255},1,0); Emit(P.x+PW*0.5f,gunY(),1,110,0.6f,1.6f,(Color){210,180,90,255},0,1);   // muzzle sparks + shell casing
    if(hitIdx>=0){ Enemy*e=&g_en[hitIdx]; e->hp-=dmg; e->hitFlash=0.12f; FloatDmg(e->x+EW*0.5f,e->y,dmg);
        if(e->hp<=0){ e->alive=0; e->st="DEAD"; PlayEnemyDeath(); Emit(e->x+EW*0.5f,e->y+EH*0.5f,16,300,0.6f,2.6f,(Color){170,30,30,255},0,1); g_shake=fmaxf(g_shake,7.0f); Ev("enemy %d killed",hitIdx); DebugLog("death","\"who\":\"enemy\",\"i\":%d,\"x\":%.1f,\"y\":%.1f",hitIdx,e->x,e->y); if(e->type==3){ g_victory=1; g_won=1; g_shake=20.0f; DebugLog("victory",""); Ev("THE USURPER FALLS"); } }
        else { SndPlay(SND_HIT); Emit(e->x+EW*0.5f,e->y+EH*0.5f,12,300,0.5f,2.3f,(Color){190,32,32,255},0,1); DebugLog("hit","\"who\":\"enemy\",\"i\":%d,\"dmg\":%d,\"hp\":%d",hitIdx,dmg,e->hp); }
    }
    DebugLog("fire","\"dir\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"face\":%d,\"dmg\":%d,\"mag\":%d,\"hit\":%s,\"target\":%d",
             dir==P.face?"fwd":"back", mx,my,P.face,dmg,P.mag, hitIdx>=0?"true":"false", hitIdx);
    if(P.mag==0 && P.reserve>0){ P.reloadT=RELOAD_T; SndPlay(SND_RELOAD); DebugLog("reload","\"start\":true,\"reserve\":%d",P.reserve); }   // auto-reload on empty
    Ev("fire %s%s", dir==P.face?"fwd":"back", hitIdx>=0?" HIT":"");
}

static void HurtPlayer(int dmg,const char*cause){
    if(g_god||P.iframes>0||P.dead) return;
    P.hp-=dmg; P.iframes=IFRAMES; P.hurtT=0.25f;
    Emit(pcx(),pcy(),8,240,0.4f,2.2f,(Color){200,60,60,255},0,1); g_shake=fmaxf(g_shake,5.0f); PlayPlayerHurt();
    DebugLog("hit","\"who\":\"player\",\"dmg\":%d,\"hp\":%d,\"cause\":\"%s\"",dmg,P.hp,cause);
    Ev("player hit -%d (%s)",dmg,cause);
    if(P.hp<=0){ P.hp=0; P.dead=1; P.deadT=0; DebugLog("death","\"who\":\"player\",\"x\":%.1f,\"y\":%.1f,\"cause\":\"%s\"",P.x,P.y,cause); Ev("player DIED (%s)",cause); }
}

// Knife: short-range melee in the facing direction (no ammo). V.
static void Melee(void){
    if(P.meleeCD>0||P.reloadT>0||P.dead) return;
    P.meleeCD=0.4f; P.meleeT=0.12f; SndPlay(SND_MELEE);
    int hit=-1;
    for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive||e->inCover) continue; float ex=e->x+EW*0.5f, ey=e->y+EH*0.5f;
        if((ex-pcx())*P.face>0 && fabsf(ex-pcx())<40.0f+EW*0.5f && fabsf(ey-pcy())<TILE*0.8f){ hit=i; break; } }
    if(hit>=0){ Enemy*e=&g_en[hit]; e->hp-=45; e->hitFlash=0.12f; g_shake=fmaxf(g_shake,6.0f); FloatDmg(e->x+EW*0.5f,e->y,45);
        Emit(e->x+EW*0.5f,e->y+EH*0.5f,12,300,0.5f,2.3f,(Color){190,32,32,255},0,1);
        if(e->hp<=0){ e->alive=0; e->st="DEAD"; PlayEnemyDeath(); DebugLog("death","\"who\":\"enemy\",\"i\":%d,\"cause\":\"melee\"",hit); if(e->type==3){ g_victory=1; g_won=1; DebugLog("victory",""); Ev("THE USURPER FALLS"); } }
        else DebugLog("hit","\"who\":\"enemy\",\"i\":%d,\"dmg\":45,\"cause\":\"melee\"",hit); }
    DebugLog("melee","\"x\":%.1f,\"face\":%d,\"hit\":%s",pcx(),P.face,hit>=0?"true":"false"); Ev("knife%s",hit>=0?" HIT":"");
}

// ---- Bombs & cracked walls --------------------------------------------------
static void ExplodeBomb(float bx,float by){
    int destroyed=0,hits=0;
    int c0=(int)floorf((bx-BOMB_RADIUS)/TILE), c1=(int)floorf((bx+BOMB_RADIUS)/TILE);
    int r0=(int)floorf((by-BOMB_RADIUS)/TILE), r1=(int)floorf((by+BOMB_RADIUS)/TILE);
    for(int r=r0;r<=r1;r++) for(int c=c0;c<=c1;c++){ if(c<0||r<0||c>=g_W||r>=g_H) continue;
        if(g_crack[r][c]){ float cx=c*TILE+TILE*0.5f, cy=r*TILE+TILE*0.5f;
            if((cx-bx)*(cx-bx)+(cy-by)*(cy-by) < BOMB_RADIUS*BOMB_RADIUS){ g_crack[r][c]=0; g_tiles[r][c]='.'; destroyed++; } }
    }
    for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive) continue; float ex=e->x+EW*0.5f,ey=e->y+EH*0.5f;
        if((ex-bx)*(ex-bx)+(ey-by)*(ey-by)<BOMB_RADIUS*BOMB_RADIUS){ e->hp-=BOMB_DMG; e->hitFlash=0.12f; hits++; FloatDmg(e->x+EW*0.5f,e->y,BOMB_DMG); Emit(e->x+EW*0.5f,e->y+EH*0.5f,12,320,0.5f,2.3f,(Color){190,32,32,255},0,1);
            if(e->hp<=0){ e->alive=0; e->st="DEAD"; PlayEnemyDeath(); DebugLog("death","\"who\":\"enemy\",\"i\":%d,\"cause\":\"bomb\"",i); if(e->type==3){ g_victory=1; g_won=1; DebugLog("victory",""); Ev("THE USURPER FALLS"); } } } }
    float pdx=pcx()-bx, pdy=pcy()-by; if(pdx*pdx+pdy*pdy<BOMB_RADIUS*BOMB_RADIUS) HurtPlayer(30,"bomb");
    g_boomT=0.35f; g_boomX=bx; g_boomY=by; SndPlay(SND_BOMB);
    Emit(bx,by,18,90,0.9f,5.0f,(Color){92,86,80,255},0,0); Emit(bx,by,22,420,0.5f,3.0f,(Color){255,180,80,255},1,0); Emit(bx,by,14,300,0.8f,2.4f,(Color){120,110,100,255},0,1); g_shake=16.0f;
    DebugLog("bomb","\"explode\":[%.0f,%.0f],\"destroyed\":%d,\"enemiesHit\":%d",bx,by,destroyed,hits); Ev("BOOM (-%d walls)",destroyed);
}
static int BombSlot(void){ for(int i=0;i<MAXBOMB;i++) if(!g_bomb[i].active) return i; return -1; }
static void PlaceBomb(void){
    if(P.bombs<=0) return; int slot=BombSlot(); if(slot<0) return;
    P.bombs--; g_bomb[slot]=(Bomb){pcx(), P.y+PH-8, 0,0, BOMB_FUSE, 1};
    DebugLog("bomb","\"place\":[%.0f,%.0f],\"fuse\":%.2f",g_bomb[slot].x,g_bomb[slot].y,(double)BOMB_FUSE); Ev("bomb placed");
}
static void ThrowBomb(void){   // lob in an arc in the facing direction
    if(P.bombs<=0) return; int slot=BombSlot(); if(slot<0) return;
    P.bombs--; g_bomb[slot]=(Bomb){pcx()+P.face*10, gunY(), P.face*270.0f, -250.0f, BOMB_FUSE, 1};
    SndPlay(SND_MELEE); DebugLog("bomb","\"throw\":[%.0f,%.0f],\"face\":%d",g_bomb[slot].x,g_bomb[slot].y,P.face); Ev("bomb thrown");
}
static void UpdateBombs(void){
    if(g_boomT>0) g_boomT-=DT;
    for(int i=0;i<MAXBOMB;i++) if(g_bomb[i].active){ Bomb*b=&g_bomb[i];
        b->vy=fminf(b->vy+GRAV*DT,MAXFALL);
        b->x+=b->vx*DT; if(SolidAt((int)floorf(b->x/TILE),(int)floorf(b->y/TILE))){ b->x-=b->vx*DT; b->vx=-b->vx*0.4f; }   // bounce off walls
        b->y+=b->vy*DT; int rr=(int)floorf(b->y/TILE), rc=(int)floorf(b->x/TILE);
        if(SolidAt(rc,rr)){ b->y=rr*TILE-0.01f; b->vy=0; b->vx*=0.7f; }   // rest on ground
        b->fuse-=DT; if(b->fuse<=0){ b->active=0; ExplodeBomb(b->x,b->y); }
    }
}

// ---- Player update ----------------------------------------------------------
static void PlayerMoveX(void){
    P.x += P.vx*DT;
    int r0=(int)floorf(P.y/TILE), r1=(int)floorf((P.y+PH-1)/TILE);
    if(P.vx>0){ int c=(int)floorf((P.x+PW)/TILE); for(int r=r0;r<=r1;r++) if(SolidAt(c,r)){ P.x=c*TILE-PW-0.01f; P.vx=0; break; } }
    else if(P.vx<0){ int c=(int)floorf(P.x/TILE); for(int r=r0;r<=r1;r++) if(SolidAt(c,r)){ P.x=(c+1)*TILE+0.01f; P.vx=0; break; } }
}
static void PlayerMoveY(void){
    float vy0=P.vy; P.y += P.vy*DT; P.onGround=0;
    int c0=(int)floorf(P.x/TILE), c1=(int)floorf((P.x+PW-1)/TILE);
    if(P.vy>0){ int r=(int)floorf((P.y+PH)/TILE); for(int c=c0;c<=c1;c++) if(SolidAt(c,r)){ P.y=r*TILE-PH-0.01f; P.vy=0; P.onGround=1; break; } }
    else if(P.vy<0){ int r=(int)floorf(P.y/TILE); for(int c=c0;c<=c1;c++) if(SolidAt(c,r)){ P.y=(r+1)*TILE+0.01f; P.vy=0; break; } }
    if(P.onGround && vy0>300.0f){   // landed from a real fall: thud + dust kick + impact shake
        SndPlay(SND_LAND);
        DebugLog("land","\"vy\":%.0f",vy0);
        float cx=P.x+PW*0.5f, fy=P.y+PH;
        Emit(cx,fy,(vy0>650?10:vy0>450?6:3),40.0f,0.5f,1.6f,(Color){170,160,140,255},0,0);   // dust (gated inside)
        if(vy0>520 && g_fx && !g_headless){ float s=(vy0-520)/90.0f; if(s>4)s=4; if(s>g_shake) g_shake=s; }
    }
    if(P.onGround && vy0>FALL_HURT){ int dmg=(int)((vy0-FALL_HURT)/45.0f); if(dmg>0){ DebugLog("fall","\"vy\":%.0f,\"dmg\":%d,\"fatal\":false",vy0,dmg); HurtPlayer(dmg,"fall"); } }
}

// ---- Moving lifts (carry physics) ------------------------------------------
// Step lifts and carry whoever rode one last frame; called before UpdatePlayer.
static void UpdateLifts(void){
    for(int i=0;i<g_liftN;i++){ Lift*L=&g_lifts[i]; L->phase+=L->omega*DT;
        float s=0.5f-0.5f*cosf(L->phase); float nx=L->bx, ny=L->by;   // s: 0..1..0 smooth
        if(L->axis) ny=L->by-s*L->amp; else nx=L->bx+s*L->amp;
        L->dx=nx-L->x; L->dy=ny-L->y; L->x=nx; L->y=ny; }
    if(P.onLift>=0 && P.onLift<g_liftN && !P.dead){ P.x+=g_lifts[P.onLift].dx; P.y+=g_lifts[P.onLift].dy; }
}
// Resolve the player against lift AABBs (solid + landing); called after tile
// collision. Sets P.onLift when the player is resting on a platform top.
static void ResolveLifts(void){
    P.onLift=-1;
    for(int i=0;i<g_liftN;i++){ Lift*L=&g_lifts[i];
        if(P.x+PW>L->x && P.x<L->x+L->w && P.y+PH>L->y && P.y<L->y+L->h){
            float penL=(P.x+PW)-L->x, penR=(L->x+L->w)-P.x, penT=(P.y+PH)-L->y, penB=(L->y+L->h)-P.y;
            float minH=penL<penR?penL:penR, minV=penT<penB?penT:penB;
            if(minV<=minH){ if(penT<penB){ P.y=L->y-PH-0.01f; if(P.vy>0)P.vy=0; P.onGround=1; P.onLift=i; } else { P.y=L->y+L->h+0.01f; if(P.vy<0)P.vy=0; } }
            else { if(penL<penR) P.x=L->x-PW-0.01f; else P.x=L->x+L->w+0.01f; P.vx=0; }
        }
    }
}

static void UpdatePlayer(Input in){
    if(P.dead){ P.deadT+=DT; if(P.deadT>1.4f) RespawnAtCheckpoint(); return; }
    P.iframes=fmaxf(0,P.iframes-DT); P.fireCD=fmaxf(0,P.fireCD-DT); P.muzzle=fmaxf(0,P.muzzle-DT); P.hurtT=fmaxf(0,P.hurtT-DT);
    P.meleeCD=fmaxf(0,P.meleeCD-DT); P.meleeT=fmaxf(0,P.meleeT-DT);
    if(P.onGround) P.coyote=0.10f; else P.coyote=fmaxf(0,P.coyote-DT);   // grace after leaving a ledge
    if(in.jump) P.jumpBuf=0.12f; else P.jumpBuf=fmaxf(0,P.jumpBuf-DT);   // buffer a press just before landing
    if(P.reloadT>0){ P.reloadT-=DT; if(P.reloadT<=0){ int load=MAG_MAX-P.mag; if(load>P.reserve)load=P.reserve; P.mag+=load; P.reserve-=load; DebugLog("reload","\"done\":true,\"mag\":%d,\"reserve\":%d",P.mag,P.reserve); Ev("reloaded (%d)",P.mag); } }

    if(P.climbT>0){ P.climbT-=DT; return; }
    if(P.inCover){
        if(in.down||in.left||in.right){ P.inCover=0; DebugLog("cover","\"who\":\"player\",\"in\":false"); Ev("leave cover"); }
        else return;
    }

    int fr=(int)floorf((P.y+PH+1)/TILE);
    int ahead = P.face>0 ? (int)floorf((P.x+PW+1)/TILE) : (int)floorf((P.x-1)/TILE);
    int pcol=(int)floorf(pcx()/TILE);

    // Context UP: door > NPC > lever > shadow > climb-up
    if(in.up && P.onGround){
        int dd=-1; for(int i=0;i<g_doorN;i++) if(pcol==g_doors[i].c){ dd=i; break; }
        int handled=0;
        if(dd>=0){ Door*D=&g_doors[dd]; handled=1;
            if(D->locked && KeyCount(D->key)<=0){ DebugLog("door","\"id\":%d,\"locked\":true,\"need\":\"%s\"",D->id,JStr(D->key)); Msg(1.6f,"Locked - need a %s key",D->key); Ev("locked: need %s key",D->key); }
            else if(D->needShards>0 && P.shards<D->needShards){ DebugLog("door","\"id\":%d,\"needShards\":%d,\"have\":%d",D->id,D->needShards,P.shards); Msg(2.0f,"The Daystone gate needs %d shards (you have %d)",D->needShards,P.shards); Ev("gate %d/%d shards",P.shards,D->needShards); }
            else {
                if(D->locked){ DebugLog("door","\"id\":%d,\"unlocked\":\"%s\"",D->id,JStr(D->key)); Ev("unlocked %s door",D->key); }
                if(D->needShards>0){ DebugLog("door","\"id\":%d,\"gateOpen\":%d",D->id,D->needShards); Msg(1.6f,"The Daystone gate opens!"); }
                if(D->target[0]==0||!strcmp(D->target,"exit")){ g_areaClear=1; g_won=1; DebugLog("door","\"id\":%d,\"areaExit\":true",D->id); Ev("AREA CLEAR"); }
                else { snprintf(g_pendTarget,sizeof g_pendTarget,"%s",D->target); g_pendSpawn=D->targetSpawn; g_pendActive=1; SndPlay(SND_PORTAL);
                       DebugLog("door","\"id\":%d,\"to\":\"%s\",\"spawn\":%d",D->id,JStr(D->target),D->targetSpawn); }
            }
        }
        if(!handled){
            int didNpc=0; for(int i=0;i<g_npcN;i++) if(!g_npc[i].freed && pcol==g_npc[i].c){ g_npc[i].freed=1; didNpc=1; MarkCollected(g_roomPath,g_npc[i].c,g_npc[i].r); SndPlay(SND_PICKUP);
                const char*gift = g_npcGift[i][0]?g_npcGift[i]:"ammo";
                if(!strncmp(gift,"hint",4)) Msg(3.5f,"%s", gift[4]?gift+5:"...");
                else if(!strcmp(gift,"key")){ KeyAdd("gold"); P.keys=KeyTotal(); Msg(2.0f,"The Aurithi gives you a gold key"); }
                else if(!strcmp(gift,"bomb")){ P.bombs++; Msg(2.0f,"The Aurithi gives you a bomb"); }
                else { P.reserve+=8; Msg(2.0f,"The Aurithi shares shells"); }
                DebugLog("npc","\"id\":%d,\"gave\":\"%s\"",i,JStr(gift)); Ev("freed an Aurithi"); break; }
            if(!didNpc){
                int didLever=0; for(int i=0;i<g_leverN;i++) if(pcol==g_levers[i].c){ g_bridgeOn=!g_bridgeOn; didLever=1; SndPlay(SND_LEVER); DebugLog("lever","\"id\":%d,\"bridge\":%s",i,g_bridgeOn?"true":"false"); Ev("lever: bridge %s",g_bridgeOn?"out":"in"); break; }
                if(!didLever){
                    if(g_alcove[fr-1][pcol]){ P.inCover=1; P.vx=0; DebugLog("cover","\"who\":\"player\",\"in\":true"); Ev("enter cover"); }
                    else if(SolidAt(ahead,fr-1)&&!SolidAt(ahead,fr-2)&&!SolidAt(ahead,fr-3)){
                        P.y=(fr-1)*TILE-PH; P.x=ahead*TILE+(TILE-PW)/2; P.vx=P.vy=0; P.climbT=0.28f; P.climbDir=1;
                        DebugLog("climb","\"dir\":\"up\",\"x\":%.1f,\"y\":%.1f",P.x,P.y); Ev("climb up"); return;
                    }
                }
            }
        }
    }
    if(g_pendActive) return;   // a door fired; SimStep will load the new room

    // Context DOWN: climb down a one-tile ledge
    if(in.down && P.onGround && !P.inCover){
        if(!SolidAt(ahead,fr-1)&&!SolidAt(ahead,fr)&&SolidAt(ahead,fr+1)){
            P.y=(fr+1)*TILE-PH; P.x=ahead*TILE+(TILE-PW)/2; P.vx=P.vy=0; P.climbT=0.24f; P.climbDir=-1;
            DebugLog("climb","\"dir\":\"down\",\"x\":%.1f,\"y\":%.1f",P.x,P.y); Ev("climb down"); return;
        }
    }

    float target = in.walk?RUN_SPD:WALK_SPD; int d=in.right-in.left; P.turning=0;   // Shift = run; default = walk
    if(d!=0){
        if(d==P.face || fabsf(P.vx)<40){ P.face=d; P.vx+=d*ACCEL*DT; P.vx=clampf(P.vx,-target,target); }
        else { P.vx-=sgn(P.vx)*FRICTION*1.6f*DT; if(fabsf(P.vx)<40){P.vx=0;P.face=d;} P.turning=1; }
    } else { P.vx-=sgn(P.vx)*FRICTION*DT; if(fabsf(P.vx)<20)P.vx=0; }

    if(in.fireF) Fire(P.face);
    if(in.fireB) Fire(-P.face);
    if(in.use)   PlaceBomb();   // E: drop a bomb (blows cracked walls / clusters)
    if(in.melee) Melee();       // V: knife
    if(in.throwb) ThrowBomb();  // T: lob a bomb
    if(P.jumpBuf>0 && (P.coyote>0||P.onLift>=0) && P.climbT<=0){ P.vy=-JUMP_V; P.onGround=0; P.onLift=-1; P.jumpBuf=0; P.coyote=0; SndPlay(SND_JUMP); DebugLog("jump","\"x\":%.1f,\"y\":%.1f",P.x,P.y); Ev("jump"); }

    P.vy=fminf(P.vy+GRAV*DT,MAXFALL);
    PlayerMoveX(); PlayerMoveY(); ResolveLifts();
    { static float stepT=0; if(P.onGround&&!P.inCover&&fabsf(P.vx)>WALK_SPD*0.6f){ stepT-=DT; if(stepT<=0){ if(!(g_runMusOk||g_walkMusOk)) SndPlay(SND_STEP); stepT=0.30f; } } else stepT=0; }   // synth footsteps (fallback when no looping samples)

    int feetRow=(int)floorf((P.y+PH+1)/TILE)-1, ccol=(int)floorf(pcx()/TILE);
    if(P.onGround && feetRow>=0 && ccol>=0 && g_spike[feetRow][ccol]) HurtPlayer(SPIKE_DMG,"spike");
    if(P.y>g_H*TILE+80){ DebugLog("fall","\"fatal\":true"); if(g_god) RespawnAtCheckpoint(); else { P.hp=0; P.dead=1; P.deadT=0; DebugLog("death","\"who\":\"player\",\"cause\":\"pit\""); Ev("fell into the void"); } return; }

    for(int i=0;i<g_pkN;i++){ Pickup*p=&g_pk[i]; if(!p->alive) continue; if(p->c==ccol && p->r==feetRow){ p->alive=0; MarkCollected(g_roomPath,p->c,p->r);
        const char*nm="";
        switch(p->kind){ case 'H': P.hp=P.hp+30>P_HP_MAX?P_HP_MAX:P.hp+30; nm="health"; break;
                         case 'K': KeyAdd("gold"); P.keys=KeyTotal(); nm="gold key"; break;
                         case 'B': P.bombs++; nm="bomb"; break;
                         case '*': P.shards++; nm="shard"; break;
                         case 'a': P.reserve+=AMMO_BOX; nm="shells"; break;
                         case 'u': P.gunSpd++; nm="speed upgrade"; break;
                         case 'U': P.gunPow++; nm="power upgrade"; break; }
        SndPlay(p->kind=='u'||p->kind=='U'?SND_UPGRADE:SND_PICKUP);
        DebugLog("pickup","\"item\":\"%s\",\"x\":%d,\"y\":%d",nm,p->c*TILE,p->r*TILE); Ev("got %s",nm); FloatSpawn(p->c*TILE+TILE*0.5f,(float)(p->r*TILE),TextFormat("+%s",nm));
    } }
    for(int i=0;i<g_cpN;i++){ if(!g_cps[i].hit && g_cps[i].c==ccol && g_cps[i].r==feetRow){ g_cps[i].hit=1;
        snprintf(g_cpPath,sizeof g_cpPath,"%s",g_roomPath); g_cpX=P.x; g_cpY=P.y; g_cpFace=P.face;
        SndPlay(SND_UPGRADE); FloatSpawn(pcx(),P.y-6,"CHECKPOINT"); Emit(pcx(),pcy(),12,160,0.7f,2.0f,(Color){255,225,140,255},1,1);   // chime + flare
        DebugLog("checkpoint","\"x\":%.1f,\"y\":%.1f",P.x,P.y); Ev("checkpoint reached"); } }
}

// ---- Enemy / world update ---------------------------------------------------
// Gravity + tile collision for a moving enemy (the brute).
static void EnemyMove(Enemy*e){
    e->x += e->vx*DT;
    int r0=(int)floorf(e->y/TILE), r1=(int)floorf((e->y+EH-1)/TILE);
    if(e->vx>0){ int c=(int)floorf((e->x+EW)/TILE); for(int r=r0;r<=r1;r++) if(SolidAt(c,r)){ e->x=c*TILE-EW-0.01f; e->vx=0; break; } }
    else if(e->vx<0){ int c=(int)floorf(e->x/TILE); for(int r=r0;r<=r1;r++) if(SolidAt(c,r)){ e->x=(c+1)*TILE+0.01f; e->vx=0; break; } }
    e->vy=fminf(e->vy+GRAV*DT,MAXFALL); e->y+=e->vy*DT; e->onGround=0;
    int c0=(int)floorf(e->x/TILE), c1=(int)floorf((e->x+EW-1)/TILE);
    if(e->vy>0){ int r=(int)floorf((e->y+EH)/TILE); for(int c=c0;c<=c1;c++) if(SolidAt(c,r)){ e->y=r*TILE-EH-0.01f; e->vy=0; e->onGround=1; break; } }
    else if(e->vy<0){ int r=(int)floorf(e->y/TILE); for(int c=c0;c<=c1;c++) if(SolidAt(c,r)){ e->y=(r+1)*TILE+0.01f; e->vy=0; break; } }
}

static void UpdateEnemies(void){
    for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive) continue;
        e->hitFlash=fmaxf(0,e->hitFlash-DT); e->fireT-=DT;
        float ex=e->x+EW*0.5f, ey=e->y+EH*0.5f, dx=pcx()-ex;
        int aggro = fabsf(dx)<E_AGGRO && fabsf(pcy()-ey)<TILE*1.6f;
        if(aggro) e->face = dx>0?1:-1;

        if(e->type==2 && e->inCover){ e->coverT-=DT; if(e->coverT<=0){ e->inCover=0; DebugLog("cover","\"who\":\"enemy\",\"i\":%d,\"in\":false",i); } }

        if(e->type==1 || e->type==3){   // BRUTE/BOSS: advance toward the player, melee on contact
            if(e->type==3 && e->hp<120 && !e->phase){ e->phase=1; g_shake=fmaxf(g_shake,14.0f); DebugLog("enrage","\"i\":%d",i); Ev("MALDRAK enrages!"); }   // phase 2
            float spd = e->type==3 ? (e->phase?150.0f:95.0f) : 120.0f;
            e->vx = (aggro && fabsf(dx)>TILE*0.9f) ? (dx>0?1:-1)*spd : 0;
            EnemyMove(e);
            if(e->y>g_H*TILE+120){ e->alive=0; continue; }   // fell out
            ex=e->x+EW*0.5f; ey=e->y+EH*0.5f; dx=pcx()-ex;
            if(!g_noEnemies && aggro && fabsf(dx)<TILE*0.9f && fabsf(pcy()-ey)<TILE*0.9f) HurtPlayer((int)((e->type==3?18:12)*g_diffMul[g_diff]),"melee");
        }

        if((e->type==0||e->type==2) && !e->inCover){   // SKARL/SENTRY: pace when idle (turn at walls/ledges)
            if(e->face==0) e->face=1;
            if(!aggro){ int fr=(int)floorf((e->y+EH+1)/TILE); int ah=e->face>0?(int)floorf((e->x+EW+2)/TILE):(int)floorf((e->x-2)/TILE);
                if(SolidAt(ah,fr-1)||!SolidAt(ah,fr)) e->face=-e->face; e->vx=e->face*45.0f; }
            else e->vx=0;
            EnemyMove(e);
        }
        e->st = e->inCover?"COVER" : (aggro?"AIM": (fabsf(e->vx)>5?"PATROL":"IDLE"));
        if(g_noEnemies) continue;

        // Ranged fire (SKARL + SENTRY; brute is melee-only).
        if(e->type!=1 && !e->inCover && e->fireT<=0 && aggro && (dx*e->face)>0 && !P.dead){
            e->fireT = e->type==2?1.2f : e->type==3?(e->phase?0.55f:0.9f) : E_INTERVAL;
            float mx=e->face>0?e->x+EW:e->x, my=ey, endx=mx+e->face*E_RANGE;
            SpawnShot(mx,my,endx,my,1); SndPlay(SND_ENEMYFIRE);
            DebugLog("enemyfire","\"i\":%d,\"type\":%d,\"x\":%.1f,\"y\":%.1f,\"dir\":%d",i,e->type,mx,my,e->face);
            if(!P.inCover && fabsf(dx)<E_RANGE && fabsf(pcy()-my)<TILE*0.6f && LineClear(mx,pcx(),my)) HurtPlayer((int)((e->type==3?18:E_DMG)*g_diffMul[g_diff]),"shot");
            if(e->type==2){ e->inCover=1; e->coverT=1.0f; DebugLog("cover","\"who\":\"enemy\",\"i\":%d,\"in\":true",i); }   // sentry ducks after firing
        }
    }
}
static void UpdateShots(void){ for(int i=0;i<MAXSHOT;i++) if(g_shot[i].age>=0) g_shot[i].age+=DT; }
static void UpdateCam(void){
    float look = P.face * 96.0f;   // lead the camera in the direction the hero faces
    float tx=pcx()+look, ty=pcy();
    tx = (g_W*TILE<=SCREEN_W)? g_W*TILE*0.5f : clampf(tx,SCREEN_W*0.5f,g_W*TILE-SCREEN_W*0.5f);
    ty = (g_H*TILE<=SCREEN_H)? g_H*TILE*0.5f : clampf(ty,SCREEN_H*0.5f,g_H*TILE-SCREEN_H*0.5f);
    float dx=tx-SCREEN_W*0.5f, dy=ty-SCREEN_H*0.5f;
    if(!g_camInit){ g_cam.x=dx; g_cam.y=dy; g_camInit=1; }   // first frame in a room: snap
    else { g_cam.x += (dx-g_cam.x)*0.09f; g_cam.y += (dy-g_cam.y)*0.16f; }   // ease (x leads, y tracks tighter)
}

// One simulation step. Applies a deferred door transition (a door can't reload
// the room mid-update while we're iterating its entities).
// Per-area ambient particles for atmosphere — themed to the current area, spawned
// across the visible camera view. Purely cosmetic (skipped headless / when FX off).
static void AmbientFX(void){
    if(!g_fx||g_headless||P.dead) return;
    float vx=g_cam.x, vy=g_cam.y;
    if(strstr(g_areaName,"Ashland")){            // forge-fires: embers rising from below
        if(g_frame%6==0)  Emit(vx+rand()%SCREEN_W, vy+SCREEN_H-20-rand()%60, 1,10.0f,3.0f,1.4f,(Color){255,150,70,255},1,0);
    } else if(strstr(g_areaName,"Mire")){        // swamp: slow drifting green motes
        if(g_frame%9==0)  Emit(vx+rand()%SCREEN_W, vy+rand()%SCREEN_H,        1, 5.0f,4.5f,2.0f,(Color){120,180,110,255},0,0);
    } else if(strstr(g_areaName,"Keep")){        // cold keep: pale violet dust falling
        if(g_frame%12==0) Emit(vx+rand()%SCREEN_W, vy+10+rand()%40,           1, 6.0f,3.5f,1.3f,(Color){150,140,180,255},0,1);
    } else {                                      // Sunken Mines: ceiling drips + dust
        if(g_frame%14==0) Emit(vx+rand()%SCREEN_W, vy+8+rand()%30,            1, 7.0f,2.6f,1.4f,(Color){140,170,210,255},0,1);
        if(g_frame%22==0) Emit(vx+rand()%SCREEN_W, vy+rand()%SCREEN_H,        1, 3.0f,3.0f,1.1f,(Color){120,120,130,255},0,0);
    }
}
static void SimStep(Input in){
    UpdateLifts();
    if(g_msgT>0) g_msgT-=DT;
    UpdatePlayer(in);
    if(g_pendActive){
        g_pendActive=0;
        char path[160]; ResolveRoomPath(path,sizeof path,g_roomPath,g_pendTarget);
        DebugLog("transition","\"to\":\"%s\",\"spawn\":%d",JStr(g_pendTarget),g_pendSpawn);
        LoadRoom(path,g_pendSpawn,1);   // entering a room sets the checkpoint
        UpdateCam();
        return;
    }
    UpdateEnemies(); UpdateShots(); UpdateBombs(); UpdateParticles(); UpdateCam();
    AmbientFX();   // area-themed ambient particles (embers / motes / drips / dust)
}

// ---- Recurring state snapshot ----------------------------------------------
static void EmitState(void){
    if(!g_debug||!g_dbg) return;
    fprintf(g_dbg,"{\"t\":%.3f,\"ev\":\"state\",\"frame\":%ld,\"fps\":%d,\"room\":\"%s\",",nowT(),g_frame,g_headless?120:GetFPS(),JStr(g_roomName));
    fprintf(g_dbg,"\"p\":{\"x\":%.1f,\"y\":%.1f,\"vx\":%.1f,\"vy\":%.1f,\"face\":%d,\"st\":\"%s\",\"hp\":%d,\"mag\":%d,\"reserve\":%d,\"ground\":%s,\"cover\":%s},",
            P.x,P.y,P.vx,P.vy,P.face,PStateName(),P.hp,P.mag,P.reserve,P.onGround?"true":"false",P.inCover?"true":"false");
    fprintf(g_dbg,"\"inv\":{\"keys\":%d,\"bombs\":%d,\"shards\":%d,\"gunPow\":%d,\"gunSpd\":%d},\"bridge\":%s,",P.keys,P.bombs,P.shards,P.gunPow,P.gunSpd,g_bridgeOn?"true":"false");
    fprintf(g_dbg,"\"cam\":[%.1f,%.1f],\"enemies\":[",g_cam.x,g_cam.y);
    int first=1; for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i];
        fprintf(g_dbg,"%s{\"i\":%d,\"type\":%d,\"x\":%.0f,\"y\":%.0f,\"hp\":%d,\"st\":\"%s\",\"face\":%d}",first?"":",",i,e->type,e->x,e->y,e->hp,e->alive?e->st:"DEAD",e->face); first=0; }
    int liveShots=0; for(int i=0;i<MAXSHOT;i++) if(g_shot[i].age>=0&&g_shot[i].age<0.2f) liveShots++;
    fprintf(g_dbg,"],\"shots\":%d,\"onLift\":%d,\"lifts\":[",liveShots,P.onLift);
    for(int i=0;i<g_liftN;i++) fprintf(g_dbg,"%s%.0f",i?",":"",g_lifts[i].y);
    fprintf(g_dbg,"]}\n"); fflush(g_dbg);
}

// ---- Sprites (original pixel art, generated in code; no asset files) ---------
// 16x26 actor bitmaps with multi-tone shading (light source upper-left).
//   o=outline  B/b/d/D = cloth light/mid/dark/darkest (tinted per actor)
//   m/M/n = gun metal mid/light/dark   y=visor glow   k=boot   .=clear
// Hero = shared 18-row BODY + interchangeable 10-row LEG poses (for a walk cycle).
// ---- Procedural sprites -----------------------------------------------------
// Original art: a parametric armored figure drawn from primitives into a
// material-id buffer, then shaded (rim light on the lit edge, dark contour on
// the shadow edge, top-down form shading, emissive visor) for a modern metallic
// look. No external or third-party sprite data.
#define SPR_W 40
#define SPR_H 56
#define SPR_SCALE 1.5f    // draw actors bigger than the hitbox, feet-anchored (fills headroom)
#define SPR_PI 3.14159265f
static unsigned char g_idbuf[SPR_H][SPR_W];
static Color shade(Color c,float f){
    int r=(int)(c.r*f),g=(int)(c.g*f),b=(int)(c.b*f);
    return (Color){ (unsigned char)(r>255?255:r<0?0:r),(unsigned char)(g>255?255:g<0?0:g),(unsigned char)(b>255?255:b<0?0:b),255 };
}
// material ids: 1 armor 2 dark 3 skin/hand 4 visor(cyan) 5 gun 6 boot 7 lit-accent 8 enemy-eye(red) 9 muzzle-flash
static Color matCol(int id,Color cloth){
    switch(id){
        case 1: return cloth;
        case 2: return shade(cloth,0.55f);
        case 3: return (Color){214,170,140,255};
        case 4: return (Color){140,236,240,255};
        case 5: return (Color){150,158,172,255};
        case 6: return (Color){52,46,42,255};
        case 7: return shade(cloth,1.22f);
        case 8: return (Color){255,104,82,255};
        case 9: return (Color){255,224,150,255};
        case 10:return (Color){208,56,56,255};
        default:return (Color){0,0,0,0};
    }
}
static void fillRect(int x0,int y0,int x1,int y1,int id){
    if(x0<0)x0=0; if(y0<0)y0=0; if(x1>SPR_W-1)x1=SPR_W-1; if(y1>SPR_H-1)y1=SPR_H-1;
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++) g_idbuf[y][x]=(unsigned char)id;
}
static void fillDisc(int cx,int cy,int rx,int ry,int id){
    for(int y=-ry;y<=ry;y++) for(int x=-rx;x<=rx;x++){
        float u=rx?(float)x/rx:0,v=ry?(float)y/ry:0; if(u*u+v*v<=1.08f){ int px=cx+x,py=cy+y;
            if(px>=0&&px<SPR_W&&py>=0&&py<SPR_H) g_idbuf[py][px]=(unsigned char)id; } }
}
// thick capsule segment between two joints (limb)
static void limb(int x0,int y0,int x1,int y1,int r,int id){
    int dx=x1-x0,dy=y1-y0,n=(abs(dx)>abs(dy)?abs(dx):abs(dy))+1;
    for(int i=0;i<=n;i++) fillDisc(x0+dx*i/n,y0+dy*i/n,r,r,id);
}
// Shade g_idbuf: dark contour, rim light on the lit edge, top-down form shade, emissive bits.
static Image ShadeBuf(Color cloth){
    Image im=GenImageColor(SPR_W,SPR_H,(Color){0,0,0,0});
    const Color RIM={210,226,255,255};
    for(int y=0;y<SPR_H;y++) for(int x=0;x<SPR_W;x++){
        int id=g_idbuf[y][x];
        int up=y>0?g_idbuf[y-1][x]:0, dn=y<SPR_H-1?g_idbuf[y+1][x]:0;
        int lf=x>0?g_idbuf[y][x-1]:0, rt=x<SPR_W-1?g_idbuf[y][x+1]:0;
        if(!id){ if(up||dn||lf||rt) ImageDrawPixel(&im,x,y,(Color){15,13,19,255}); continue; }
        Color base=matCol(id,cloth);
        if(id==4||id==8||id==9){ Color hot=id==4?(Color){205,250,252,255}:id==8?(Color){255,182,150,255}:(Color){255,255,236,255}; ImageDrawPixel(&im,x,y,(lf==id&&rt==id)?hot:base); continue; }
        if(dn==0||rt==0){ ImageDrawPixel(&im,x,y,shade(base,0.45f)); continue; }
        if(up==0||lf==0){ ImageDrawPixel(&im,x,y,(Color){(unsigned char)((base.r+2*RIM.r)/3),(unsigned char)((base.g+2*RIM.g)/3),(unsigned char)((base.b+2*RIM.b)/3),255}); continue; }
        float f=1.13f-0.40f*((float)y/SPR_H)+0.06f*(1.0f-(float)x/SPR_W);
        ImageDrawPixel(&im,x,y,shade(base,f));
    }
    return im;
}
// Hero rig (facing +x). frame 0 = idle, 1..6 = run cycle. mode 0 run/ready, 1 aim, 2 aim+flash.
// lean shifts the upper body (hurt = negative), reach = climb pose.
static void BuildHero(int frame,int mode,int lean,int reach){
    memset(g_idbuf,0,sizeof g_idbuf);
    int cx=19, groundY=54;
    int run = frame>0;
    float ph = run ? (float)(frame-1)/6.0f*2*SPR_PI : 0;
    int bob = run ? -(int)(1.5f*fabsf(sinf(ph))) : 0;
    int lf  = (run?2:0) + lean;
    int shY=17+bob, hipY=35+bob;
    int hx=cx+lf;
    // legs (behind torso)
    for(int sde=0;sde<2;sde++){
        int hipx=cx+(sde?3:-3);
        float a=ph+(sde?SPR_PI:0); float sw=run?sinf(a):0;
        int footx=hipx+(int)(14*sw);
        int footy=(int)(groundY-(run?10.0f*fmaxf(0,sinf(a+0.6f)):0));
        int kneex=(hipx+footx)/2+(sw>0?2:1), kneey=(hipY+footy)/2+1;
        limb(hipx,hipY,kneex,kneey,3,1);
        limb(kneex,kneey,footx,footy,2,1);
        fillRect(footx-2,footy-1,footx+4,footy+1,6);
    }
    // backpack
    fillRect(cx-9,shY+1,cx-6,hipY-5,2); fillRect(cx-9,shY,cx-7,shY+1,7);
    // torso
    fillRect(cx-6,shY,cx+6,hipY,1);
    fillRect(cx-1,shY+2,cx,hipY-3,2);                       // center seam
    fillRect(cx-5,shY+3,cx-2,shY+8,2); fillRect(cx+2,shY+3,cx+5,shY+8,7);  // pecs
    fillRect(cx-6,shY+11,cx+6,shY+11,2);                    // ab line
    fillRect(cx-6,hipY-3,cx+6,hipY,2); fillRect(cx-2,hipY-2,cx+2,hipY-1,7);// belt+buckle
    fillRect(cx-6,shY,cx-4,shY+2,7); fillRect(cx+4,shY,cx+6,shY+2,7);      // shoulder pads
    // head: face + bandana (run-and-gun commando)
    fillDisc(hx,shY-8,4,6,3);                               // skin head/face
    fillRect(hx-4,shY-14,hx+4,shY-11,2);                    // hair
    fillRect(hx-2,shY-11,hx+4,shY-10,2);
    fillRect(hx-4,shY-9,hx+4,shY-8,10);                     // headband
    fillRect(hx+2,shY-9,hx+4,shY-8,7);                      // headband shine
    fillRect(hx+1,shY-6,hx+2,shY-5,2);                      // eye
    fillRect(hx-4,shY-3,hx+2,shY-2,2);                      // jaw shadow
    // arms + gun
    int shRx=cx+5, shLx=cx-5;
    if(reach){
        limb(cx-4,shY+1,cx-6,shY-9,2,1); limb(cx+4,shY+1,cx+6,shY-9,2,1);   // both arms up
        fillDisc(cx-6,shY-10,2,2,1); fillDisc(cx+6,shY-10,2,2,1);
    } else if(mode>=1){
        int handx=cx+13, handy=shY+3;
        limb(shRx,shY+1,cx+10,shY+2,3,1); limb(cx+10,shY+2,handx,handy,2,3); // upper arm + bare forearm
        limb(shLx,shY+2,cx+5,shY+5,2,1);                    // support arm
        fillRect(handx-4,handy-2,handx+8,handy+1,5);        // rifle body+barrel
        fillRect(handx-6,handy-1,handx-3,handy+1,2);        // stock
        fillRect(handx-5,handy+1,handx-3,handy+4,5);        // grip
        fillRect(handx+1,handy-3,handx+3,handy-2,2);        // sight
        fillDisc(handx-1,handy,2,2,1);                      // front hand
        if(mode==2){ fillDisc(handx+10,handy,3,3,9); fillRect(handx+10,handy-1,handx+13,handy+1,9); }
    } else {
        // near arm holds the rifle ready, swinging slightly with the stride
        float a=ph; float sw=run?sinf(a):0;
        int handx=cx+9+(int)(3*sw), handy=shY+8+(int)(2*sw);
        int elbx=(shRx+handx)/2+1, elby=(shY+2+handy)/2;
        limb(shRx,shY+1,elbx,elby,2,1); limb(elbx,elby,handx,handy,2,3);   // bare forearm
        fillRect(handx-2,handy-1,handx+8,handy,5); fillRect(handx+6,handy-2,handx+8,handy-1,5);
        fillRect(handx-4,handy,handx-2,handy+2,5);          // grip
        fillDisc(handx-1,handy,2,2,1);                      // gloved hand
    }
}
static Image HeroFrame(Color cloth,int frame,int mode,int lean,int reach){ BuildHero(frame,mode,lean,reach); return ShadeBuf(cloth); }
// Enemy rig: distinct per-type silhouettes, 2-frame step, gun aimed forward.
static void BuildEnemy(int type,int frame){
    memset(g_idbuf,0,sizeof g_idbuf);
    int cx=19, groundY=54, sw=frame?2:-2;
    int tw,shY,hipY,hr;
    if(type==1){ tw=10; shY=15; hipY=34; hr=4; }       // brute: wide
    else if(type==2){ tw=5; shY=16; hipY=35; hr=3; }   // sentry: lean/tall
    else if(type==3){ tw=8; shY=15; hipY=35; hr=5; }   // maldrak: big
    else { tw=7; shY=16; hipY=34; hr=4; }              // skarl
    // legs
    for(int sde=0;sde<2;sde++){
        int hipx=cx+(sde?tw/2:-tw/2); int dir=sde?sw:-sw;
        int footx=hipx+dir, footy=groundY-(dir>0?3:0);
        int kneex=(hipx+footx)/2+1, kneey=(hipY+footy)/2;
        limb(hipx,hipY,kneex,kneey,3,1); limb(kneex,kneey,footx,footy,2,1);
        fillRect(footx-2,footy-1,footx+4,footy+1,6);
    }
    fillRect(cx-tw-3,shY+1,cx-tw,hipY-4,2);                 // back
    fillRect(cx-tw,shY,cx+tw,hipY,1);                       // torso
    fillRect(cx-1,shY+2,cx,hipY-3,2);
    fillRect(cx-tw+1,shY+3,cx-2,shY+8,2); fillRect(cx+2,shY+3,cx+tw-1,shY+8,7);
    fillRect(cx-tw,hipY-3,cx+tw,hipY,2);
    fillRect(cx-tw,shY,cx-tw+2,shY+2,7); fillRect(cx+tw-2,shY,cx+tw,shY+2,7); // shoulders
    // head
    int hy=shY-hr-3;
    fillDisc(cx,hy,hr,hr+1,1);
    fillRect(cx-hr+1,hy-1,cx+hr,hy,8);                      // red glare
    // gun forward
    int handx=cx+tw+4, handy=shY+3, gl=(type==2?9:type==3?7:5);
    limb(cx+tw-2,shY+1,handx,handy,2,1);
    fillRect(handx-3,handy-1,handx+gl,handy+1,5);
    fillDisc(handx-1,handy,2,2,1);
    // rear arm
    limb(cx-tw+1,shY+2,cx-tw-1,hipY-6,2,1);
    // type extras
    if(type==3){ fillRect(cx-hr,hy-hr-2,cx-hr+1,hy-hr+1,1); fillRect(cx+hr-1,hy-hr-2,cx+hr,hy-hr+1,1);   // horns
                 fillRect(cx-tw-1,shY-1,cx-tw+2,shY+1,7); fillRect(cx+tw-2,shY-1,cx+tw+1,shY+1,7); }     // pauldrons
    if(type==1){ fillRect(cx-tw,shY-1,cx-tw+3,shY+1,7); fillRect(cx+tw-3,shY-1,cx+tw,shY+1,7); }         // brute slabs
}
static Image EnemyFrame(Color cloth,int type,int frame){ BuildEnemy(type,frame); return ShadeBuf(cloth); }
static int iclamp(int v,int a,int b){ return v<a?a:(v>b?b:v); }
static Image ImgStone(Color base,int cracked){
    int n=TILE; Image im=GenImageColor(n,n,base);
    for(int y=0;y<n;y++) for(int x=0;x<n;x++){
        int v=((x*5+y*9+((x^y)*3))%13)-6, top=(y<3)?16:0, bot=(y>n-3)?-16:0, s=v+top+bot;
        ImageDrawPixel(&im,x,y,(Color){(unsigned char)iclamp(base.r+s,0,255),(unsigned char)iclamp(base.g+s,0,255),(unsigned char)iclamp(base.b+s,0,255),255});
    }
    ImageDrawRectangleLines(&im,(Rectangle){0,0,n,n},1,(Color){0,0,0,50});
    if(cracked){ ImageDrawLine(&im,5,2,11,13,(Color){18,14,10,255}); ImageDrawLine(&im,11,3,7,15,(Color){18,14,10,255}); ImageDrawLine(&im,4,9,13,7,(Color){18,14,10,200}); }
    return im;
}
static Image ImgBridge(void){
    int n=TILE; Image im=GenImageColor(n,n,(Color){120,86,50,255});
    for(int x=0;x<n;x+=6) ImageDrawLine(&im,x,0,x,n,(Color){80,56,32,255});
    ImageDrawRectangle(&im,0,0,n,3,(Color){152,116,72,255});
    return im;
}

static Texture2D g_tHeroIdle,g_tHeroWalk,g_tEnemy[4],g_tStone,g_tCrack,g_tBridge;
static int g_sprites=0;
// Drop-in sprite strips: external PNGs in assets/sprites/ (horizontal frame
// strips). Loaded if present, else the generated art is the fallback.
typedef struct { Texture2D tex; int frames; float fps; int ok; } Strip;
static Strip g_sHeroIdle,g_sHeroWalk,g_sHeroFire,g_sHeroHurt,g_sHeroClimb,g_sEnemy[4];
static Strip LoadStrip(const char*path,int frames,float fps){
    Strip s; s.tex=(Texture2D){0}; s.frames=frames<1?1:frames; s.fps=fps; s.ok=0;
    if(FileExists(path)){ s.tex=LoadTexture(path); if(s.tex.id>0){ SetTextureFilter(s.tex,TEXTURE_FILTER_POINT); s.ok=1; } }
    return s;
}
static int AnimF(Strip*s){ return s->frames>1 ? (int)(GetTime()*s->fps) : 0; }   // current frame index
static void InitSprites(void){
    Image im;
    im=HeroFrame((Color){92,150,235,255},0,0,0,0); g_tHeroIdle=LoadTextureFromImage(im); UnloadImage(im);
    im=HeroFrame((Color){92,150,235,255},2,0,0,0); g_tHeroWalk=LoadTextureFromImage(im); UnloadImage(im);
    im=EnemyFrame((Color){200,72,72,255},0,0);  g_tEnemy[0]=LoadTextureFromImage(im); UnloadImage(im);
    im=EnemyFrame((Color){170,70,60,255},1,0);  g_tEnemy[1]=LoadTextureFromImage(im); UnloadImage(im);
    im=EnemyFrame((Color){156,90,196,255},2,0); g_tEnemy[2]=LoadTextureFromImage(im); UnloadImage(im);
    im=EnemyFrame((Color){164,44,64,255},3,0);  g_tEnemy[3]=LoadTextureFromImage(im); UnloadImage(im);   // MALDRAK
    im=ImgStone((Color){58,54,64,255},0); g_tStone=LoadTextureFromImage(im); UnloadImage(im);
    im=ImgStone((Color){78,64,52,255},1); g_tCrack=LoadTextureFromImage(im); UnloadImage(im);
    im=ImgBridge(); g_tBridge=LoadTextureFromImage(im); UnloadImage(im);
    { int n=128; Image im2=GenImageColor(n,n,(Color){0,0,0,0});   // soft radial light
      for(int y=0;y<n;y++) for(int x=0;x<n;x++){ float dx=(x-n/2)/(float)(n/2),dy=(y-n/2)/(float)(n/2),d=sqrtf(dx*dx+dy*dy),v=1.0f-d; if(v<0)v=0; v=v*v; ImageDrawPixel(&im2,x,y,(Color){255,255,255,(unsigned char)(v*255)}); }
      g_tLight=LoadTextureFromImage(im2); SetTextureFilter(g_tLight,TEXTURE_FILTER_BILINEAR); UnloadImage(im2); }
    { int W=SCREEN_W/6,H=SCREEN_H/6; Image im2=GenImageColor(W,H,(Color){0,0,0,0});   // vignette
      for(int y=0;y<H;y++) for(int x=0;x<W;x++){ float dx=(x-W/2)/(float)(W/2),dy=(y-H/2)/(float)(H/2),d=sqrtf(dx*dx+dy*dy),v=(d-0.62f)/0.5f; if(v<0)v=0; if(v>1)v=1; ImageDrawPixel(&im2,x,y,(Color){0,0,0,(unsigned char)(v*175)}); }
      g_tVign=LoadTextureFromImage(im2); SetTextureFilter(g_tVign,TEXTURE_FILTER_BILINEAR); UnloadImage(im2); }
    // Drop-in sprite strips (override the generated art if the PNGs are present).
    g_sHeroIdle=LoadStrip("assets/sprites/hero_idle.png",2,4.0f);     // breathe
    g_sHeroWalk=LoadStrip("assets/sprites/hero_walk.png",6,13.0f);    // walk cycle
    g_sHeroFire=LoadStrip("assets/sprites/hero_fire.png",2,16.0f);     // recoil/aim
    g_sHeroHurt=LoadStrip("assets/sprites/hero_hurt.png",1,1.0f);     // flinch
    g_sHeroClimb=LoadStrip("assets/sprites/hero_climb.png",2,6.0f);   // reach cycle
    g_sEnemy[0]=LoadStrip("assets/sprites/skarl.png",2,3.0f);
    g_sEnemy[1]=LoadStrip("assets/sprites/brute.png",2,3.5f);
    g_sEnemy[2]=LoadStrip("assets/sprites/sentry.png",2,3.0f);
    g_sEnemy[3]=LoadStrip("assets/sprites/maldrak.png",2,2.5f);
    g_sprites=1;
}
// Export the sprites to a PNG (no window needed) for visual review.
static int DumpSprites(void){
    Color blue={92,150,235,255};
    Image a[12]; int n=0;
    a[n++]=HeroFrame(blue,0,0,0,0);      // idle
    a[n++]=HeroFrame(blue,1,0,0,0);      // walk A
    a[n++]=HeroFrame(blue,3,0,0,0);      // walk C
    a[n++]=HeroFrame(blue,0,2,0,0);      // fire (muzzle flash)
    a[n++]=HeroFrame(blue,0,0,-2,0);     // hurt (head back)
    a[n++]=HeroFrame(blue,0,0,0,1);      // climb
    a[n++]=EnemyFrame((Color){200,72,72,255},0,0);   // skarl
    a[n++]=EnemyFrame((Color){170,70,60,255},1,0);   // brute
    a[n++]=EnemyFrame((Color){156,90,196,255},2,0);  // sentry
    a[n++]=EnemyFrame((Color){164,44,64,255},3,0);   // maldrak
    a[n++]=ImgStone((Color){58,54,64,255},0);
    a[n++]=ImgStone((Color){78,64,52,255},1);
    Image sheet=GenImageColor(1640,220,(Color){26,26,34,255});
    int x=12, sc=3;
    for(int i=0;i<n;i++){ ImageDraw(&sheet,a[i],(Rectangle){0,0,(float)a[i].width,(float)a[i].height},(Rectangle){(float)x,20,(float)a[i].width*sc,(float)a[i].height*sc},WHITE); x+=a[i].width*sc+10; UnloadImage(a[i]); }
    ExportImage(sheet,"thorn-sprites.png"); UnloadImage(sheet);
    printf("wrote thorn-sprites.png\n"); return 0;
}
// Compose `n` frames horizontally into one strip PNG; yoff[i] bobs a frame down.
static void StripExport(const char*path,Image*frames,int n,const int*yoff){
    int fw=frames[0].width, fh=frames[0].height;
    Image strip=GenImageColor(fw*n,fh,(Color){0,0,0,0});
    for(int i=0;i<n;i++) ImageDraw(&strip,frames[i],(Rectangle){0,0,(float)fw,(float)fh},(Rectangle){(float)(i*fw),(float)(yoff?yoff[i]:0),(float)fw,(float)fh},WHITE);
    ExportImage(strip,path); UnloadImage(strip);
}
// Generate the bundled (original, CC0) sprite strips into assets/sprites/ — these
// are what the drop-in loader reads; replace them with any CC0 pack to reskin.
static int GenAssets(void){
    Color blue={92,150,235,255}; int bob[2]={0,1};
    { Image f0=HeroFrame(blue,0,0,0,0),f1=HeroFrame(blue,0,0,0,0); Image fr[2]={f0,f1}; StripExport("assets/sprites/hero_idle.png",fr,2,bob); UnloadImage(f0); UnloadImage(f1); }
    { Image w[6]; for(int k=0;k<6;k++) w[k]=HeroFrame(blue,k+1,0,0,0); StripExport("assets/sprites/hero_walk.png",w,6,NULL); for(int k=0;k<6;k++) UnloadImage(w[k]); }
    { Image f0=HeroFrame(blue,0,1,0,0),f1=HeroFrame(blue,0,2,0,0); Image fr[2]={f0,f1}; StripExport("assets/sprites/hero_fire.png",fr,2,NULL); UnloadImage(f0); UnloadImage(f1); }
    { Image f=HeroFrame(blue,0,0,-2,0); Image fr[1]={f}; StripExport("assets/sprites/hero_hurt.png",fr,1,NULL); UnloadImage(f); }
    { Image c0=HeroFrame(blue,0,0,0,1),c1=HeroFrame(blue,0,0,0,1); Image fr[2]={c0,c1}; int yo[2]={0,1}; StripExport("assets/sprites/hero_climb.png",fr,2,yo); UnloadImage(c0); UnloadImage(c1); }
    const char*ef[4]={"skarl","brute","sentry","maldrak"};
    Color ec[4]={{200,72,72,255},{170,70,60,255},{156,90,196,255},{164,44,64,255}};
    for(int i=0;i<4;i++){ char p[128]; snprintf(p,sizeof p,"assets/sprites/%s.png",ef[i]);
        Image g0=EnemyFrame(ec[i],i,0),g1=EnemyFrame(ec[i],i,1); Image fr[2]={g0,g1}; StripExport(p,fr,2,NULL); UnloadImage(g0); UnloadImage(g1); }
    printf("wrote assets/sprites/*.png\n"); return 0;
}
// Draw an actor texture into AABB (ax,ay,aw,ah), feet-aligned, flipped by face.
static void DrawActorTex(Texture2D t,float ax,float ay,float aw,float ah,int face,float alpha,int flash){
    float scale=ah*SPR_SCALE/(float)t.height, dw=t.width*scale;
    Rectangle src={0,0,(float)(face>0?t.width:-t.width),(float)t.height};
    Rectangle dst={ax+aw*0.5f-dw*0.5f, ay+ah-(t.height*scale), dw, t.height*scale};
    DrawTexturePro(t,src,dst,(Vector2){0,0},0,(Color){255,255,255,(unsigned char)(alpha*255)});
    if(flash){ BeginBlendMode(BLEND_ADDITIVE); DrawTexturePro(t,src,dst,(Vector2){0,0},0,(Color){150,150,160,255}); EndBlendMode(); }
}

// Draw one frame of a sprite strip into the actor AABB (see Strip above).
static void DrawActorStrip(Strip*s,int frame,float ax,float ay,float aw,float ah,int face,float alpha,int flash){
    int fw=s->tex.width/s->frames; if(s->frames>1) frame%=s->frames; else frame=0;
    float scale=ah*SPR_SCALE/(float)s->tex.height, dw=fw*scale;
    Rectangle src={(float)(frame*fw),0,(float)(face>0?fw:-fw),(float)s->tex.height};
    Rectangle dst={ax+aw*0.5f-dw*0.5f, ay+ah-(s->tex.height*scale), dw, s->tex.height*scale};
    DrawTexturePro(s->tex,src,dst,(Vector2){0,0},0,(Color){255,255,255,(unsigned char)(alpha*255)});
    if(flash){ BeginBlendMode(BLEND_ADDITIVE); DrawTexturePro(s->tex,src,dst,(Vector2){0,0},0,(Color){150,150,160,255}); EndBlendMode(); }
}

// ---- Rendering --------------------------------------------------------------
static void DrawFigure(float x,float y,float w,float h,int face,Color body,int firing,int dir,int dim){
    Color head=(Color){body.r,body.g,body.b,255};
    if(dim){ body.a=120; head.a=120; }
    float cx=x+w*0.5f;
    DrawRectangle((int)(x+w*0.18f),(int)(y+h*0.24f),(int)(w*0.64f),(int)(h*0.56f),body);
    DrawRectangle((int)(x+w*0.22f),(int)(y+h*0.80f),(int)(w*0.22f),(int)(h*0.20f),body);
    DrawRectangle((int)(x+w*0.56f),(int)(y+h*0.80f),(int)(w*0.22f),(int)(h*0.20f),body);
    DrawCircle((int)cx,(int)(y+h*0.14f),w*0.30f,head);
    float by=y+h*0.42f; float bx=face>0?x+w*0.6f:x+w*0.4f; float blen=w*1.1f;
    DrawRectangle((int)(face>0?bx:bx-blen),(int)by,(int)blen,5,(Color){40,44,52,dim?120:255});
    if(firing) DrawCircle((int)(dir>0?x+w+8:x-8),(int)(y+h*0.42f+2),8,(Color){255,230,120,235});
}

static Color AreaTint(void){
    if(strstr(g_areaName,"Mire"))     return (Color){34,64,44,255};   // murky green
    if(strstr(g_areaName,"Ashlands")) return (Color){86,52,30,255};   // warm ash
    if(strstr(g_areaName,"Keep"))     return (Color){34,30,52,255};   // cold violet-dark
    return (Color){30,34,50,255};                                      // mines blue-grey
}
static void DrawParallax(void){
    Color t=AreaTint();
    DrawRectangleGradientV(0,0,SCREEN_W,SCREEN_H,(Color){t.r/3,t.g/3,t.b/3,255},(Color){t.r/7,t.g/7,t.b/7,255});
    int o1=(int)g_cam.x/5; for(int x=-200;x<SCREEN_W+200;x+=180){ int gx=x-(o1%180); int h=130+((x/180)*53)%90; DrawRectangle(gx,SCREEN_H-260-h,150,500,(Color){t.r/2,t.g/2,t.b/2,90}); }
    int o2=(int)(g_cam.x*0.45f); for(int x=-140;x<SCREEN_W+140;x+=130){ int gx=x-(o2%130); int h=90+((x/130)*37)%80; DrawRectangle(gx,SCREEN_H-170-h,110,420,(Color){t.r/2,t.g/2,t.b/2,130}); }
}
static void DrawParticles(void){
    for(int i=0;i<MAXPART;i++){ Part*p=&g_part[i]; if(p->life<=0||p->add) continue; float a=p->life/p->max; float s=p->size*(0.5f+a*0.7f);
        DrawRectangle((int)(p->x-s),(int)(p->y-s),(int)(s*2+1),(int)(s*2+1),(Color){p->r,p->g,p->b,(unsigned char)(a*255)}); }
    BeginBlendMode(BLEND_ADDITIVE);
    for(int i=0;i<MAXPART;i++){ Part*p=&g_part[i]; if(p->life<=0||!p->add) continue; float a=p->life/p->max; float s=p->size*(0.5f+a*0.9f);
        DrawRectangle((int)(p->x-s),(int)(p->y-s),(int)(s*2+1),(int)(s*2+1),(Color){p->r,p->g,p->b,(unsigned char)(a*255)}); }
    EndBlendMode();
}
static void DrawFloats(void){ for(int i=0;i<MAXFLOATTXT;i++){ FloatTxt*f=&g_float[i]; if(f->life<=0) continue; unsigned char a=(unsigned char)((f->life>1?1:f->life)*255); int sz=f->kind?15:12; Color c=f->kind?(Color){255,160,100,a}:(Color){240,235,180,a}; int w=MeasureText(f->s,sz); DrawText(f->s,(int)(f->x-w/2),(int)f->y,sz,c); } }
static void Light(float wx,float wy,float rad,Color c){ DrawTexturePro(g_tLight,(Rectangle){0,0,128,128},(Rectangle){wx-g_cam.x-rad,wy-g_cam.y-rad,rad*2,rad*2},(Vector2){0,0},0,c); }
static void DrawLighting(void){
    Color t=AreaTint();
    DrawRectangle(0,0,SCREEN_W,SCREEN_H,(Color){t.r,t.g,t.b,46});   // subtle area colour grade
    BeginBlendMode(BLEND_ADDITIVE);
    if(!P.dead) Light(pcx(),pcy(),175,(Color){255,224,176,120});                              // player glow
    if(P.muzzle>0) Light(P.muzzleDir>0?P.x+PW:P.x,gunY(),120,(Color){255,235,150,210});        // muzzle flash
    for(int i=0;i<g_enN;i++) if(g_en[i].alive&&g_en[i].type==3) Light(g_en[i].x+EW*0.5f,g_en[i].y+EH*0.5f,150,(Color){235,90,110,110}); // boss aura
    if(g_boomT>0){ float k=g_boomT/0.35f; Light(g_boomX,g_boomY,300,(Color){255,170,80,(unsigned char)(230*k)}); }
    EndBlendMode();
    DrawTexturePro(g_tVign,(Rectangle){0,0,(float)g_tVign.width,(float)g_tVign.height},(Rectangle){0,0,SCREEN_W,SCREEN_H},(Vector2){0,0},0,WHITE);
}

static void DrawWorld(void){
    if(g_fx) DrawParallax();
    float shx=0,shy=0; if(g_fx&&g_shake>0){ shx=((rand()%200)/100.0f-1.0f)*g_shake; shy=((rand()%200)/100.0f-1.0f)*g_shake; }
    Camera2D cam={ .offset={SCREEN_W*0.5f+shx,SCREEN_H*0.5f+shy}, .target={g_cam.x+SCREEN_W*0.5f,g_cam.y+SCREEN_H*0.5f}, .rotation=0, .zoom=1 };
    BeginMode2D(cam);
    int vc0=(int)(g_cam.x/TILE)-1, vc1=(int)((g_cam.x+SCREEN_W)/TILE)+1;   // only draw on-screen tiles
    int vr0=(int)(g_cam.y/TILE)-1, vr1=(int)((g_cam.y+SCREEN_H)/TILE)+1;
    if(vc0<0)vc0=0; if(vr0<0)vr0=0; if(vc1>g_W-1)vc1=g_W-1; if(vr1>g_H-1)vr1=g_H-1;
    for(int r=vr0;r<=vr1;r++) for(int c=vc0;c<=vc1;c++) if(g_alcove[r][c]){
        DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){14,16,26,255});
        DrawRectangleLinesEx((Rectangle){c*TILE+3,r*TILE+3,TILE-6,TILE-6},2,(Color){40,44,70,255});
    }
    for(int r=vr0;r<=vr1;r++) for(int c=vc0;c<=vc1;c++) if(g_tiles[r][c]=='#' && !g_crack[r][c]){
        if(g_sprites) DrawTexture(g_tStone,c*TILE,r*TILE,WHITE);
        else { DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){58,54,64,255}); DrawRectangle(c*TILE,r*TILE,TILE,4,(Color){92,86,104,255}); }
    }
    for(int r=vr0;r<=vr1;r++) for(int c=vc0;c<=vc1;c++) if(g_crack[r][c]){   // cracked walls read as breakable
        if(g_sprites) DrawTexture(g_tCrack,c*TILE,r*TILE,WHITE);
        else { DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){78,64,52,255});
            DrawLineEx((Vector2){c*TILE+7,r*TILE+3},(Vector2){c*TILE+15,r*TILE+TILE-5},2,(Color){28,22,18,255});
            DrawLineEx((Vector2){c*TILE+22,r*TILE+5},(Vector2){c*TILE+13,r*TILE+19},2,(Color){28,22,18,255}); }
    }
    for(int r=vr0;r<=vr1;r++) for(int c=vc0;c<=vc1;c++) if(g_bridge[r][c]){
        if(g_bridgeOn){ if(g_sprites) DrawTexture(g_tBridge,c*TILE,r*TILE,WHITE); else { DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){120,86,50,255}); DrawRectangle(c*TILE,r*TILE,TILE,4,(Color){160,120,70,255}); } }
        else DrawRectangleLinesEx((Rectangle){c*TILE+2,r*TILE+2,TILE-4,TILE-4},1,(Color){90,66,40,170}); // ghost
    }
    for(int r=vr0;r<=vr1;r++) for(int c=vc0;c<=vc1;c++) if(g_spike[r][c])
        for(int k=0;k<4;k++) DrawTriangle((Vector2){c*TILE+k*8.0f,(r+1)*TILE},(Vector2){c*TILE+k*8.0f+4,(r+1)*TILE-14},(Vector2){c*TILE+k*8.0f+8,(r+1)*TILE},(Color){180,60,60,255});
    for(int i=0;i<g_liftN;i++){ Lift*L=&g_lifts[i]; DrawRectangle((int)L->x,(int)L->y,(int)L->w,(int)L->h,(Color){100,104,120,255}); DrawRectangle((int)L->x,(int)L->y,(int)L->w,3,(Color){150,156,175,255}); }
    for(int i=0;i<g_pkN;i++){ Pickup*p=&g_pk[i]; if(!p->alive) continue; float x=p->c*TILE+TILE*0.5f, y=(p->r+1)*TILE-20;
        if(p->kind=='H'){ DrawRectangle((int)x-4,(int)y-13,9,26,(Color){70,220,90,255}); DrawRectangle((int)x-13,(int)y-4,26,9,(Color){70,220,90,255}); }
        else if(p->kind=='K'){ DrawCircle((int)x-6,(int)y,9,(Color){235,205,70,255}); DrawCircle((int)x-6,(int)y,4,(Color){40,34,16,255}); DrawRectangle((int)x-2,(int)y-3,18,6,(Color){235,205,70,255}); DrawRectangle((int)x+13,(int)y+3,5,6,(Color){235,205,70,255}); }
        else if(p->kind=='B'){ DrawCircle((int)x,(int)y,12,(Color){50,50,60,255}); DrawCircle((int)x-3,(int)y-3,4,(Color){90,90,104,255}); DrawRectangle((int)x-2,(int)y-19,3,8,(Color){200,120,40,255}); }
        else if(p->kind=='*'){ DrawPoly((Vector2){x,y},4,13,45,(Color){90,220,235,255}); DrawPolyLines((Vector2){x,y},4,13,45,(Color){205,250,255,255}); }
        else if(p->kind=='a'){ DrawRectangle((int)x-10,(int)y-7,20,15,(Color){170,135,70,255}); DrawRectangleLines((int)x-10,(int)y-7,20,15,(Color){90,70,40,255}); DrawRectangle((int)x-10,(int)y-7,20,4,(Color){200,165,95,255}); }
        else if(p->kind=='u'){ DrawPoly((Vector2){x,y},3,15,-90,(Color){120,170,255,255}); }   // speed upgrade
        else if(p->kind=='U'){ DrawPoly((Vector2){x,y},3,15,-90,(Color){255,120,120,255}); }   // power upgrade
    }
    for(int i=0;i<g_leverN;i++){ float x=g_levers[i].c*TILE+TILE*0.5f,y=(g_levers[i].r+1)*TILE; DrawRectangle((int)x-2,(int)y-10,4,10,(Color){120,120,130,255}); DrawCircle((int)(x+(g_bridgeOn?7:-7)),(int)y-12,5,g_bridgeOn?(Color){90,220,120,255}:(Color){220,90,90,255}); }
    for(int i=0;i<g_cpN;i++){ float x=g_cps[i].c*TILE+TILE*0.5f,y=(g_cps[i].r+1)*TILE; Color cc=g_cps[i].hit?(Color){90,200,120,220}:(Color){80,90,110,150}; DrawRectangle((int)x-2,(int)y-22,4,22,cc); DrawTriangle((Vector2){x+2,y-22},(Vector2){x+13,y-18},(Vector2){x+2,y-14},cc); }
    for(int i=0;i<g_doorN;i++){ Door*D=&g_doors[i]; float x=D->c*TILE,y=(D->r+1)*TILE; int locked=D->locked&&KeyCount(D->key)<=0;
        Color fr=locked?(Color){180,150,60,255}:(Color){96,66,42,255};
        int dh=TILE*2+8;                                              // doorway a touch taller than the hero
        DrawRectangle((int)x+1,(int)y-dh,TILE-2,dh,fr);               // frame
        DrawRectangle((int)x+1,(int)y-dh,TILE-2,4,shade(fr,1.45f));   // lintel highlight
        DrawRectangle((int)x+5,(int)y-dh+5,TILE-10,dh-5,(Color){24,18,14,255}); // dark opening
        if(locked) DrawText("L",(int)x+11,(int)y-dh/2-8,20,(Color){235,205,70,255});
    }
    for(int i=0;i<g_npcN;i++){ Npc*n=&g_npc[i]; float nw=EW*1.5f,nh=EH*1.5f,ncx=n->c*TILE+TILE*0.5f,ngy=(n->r+1)*TILE; DrawFigure(ncx-nw*0.5f,ngy-nh,nw,nh,1,n->freed?(Color){120,200,140,255}:(Color){150,150,160,255},0,1,0); }
    for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive){ DrawRectangle((int)e->x,(int)(e->y+EH-8),(int)EW,8,(Color){90,30,30,200}); continue; }
        if(g_sprites){ float sc=e->type==3?1.7f:e->type==1?1.14f:1.0f, eh=EH*sc, ew=EW*sc, ax=e->x+EW*0.5f-ew*0.5f, ay=e->y+EH-eh, al=e->inCover?0.45f:1.0f;
            if(g_sEnemy[e->type].ok) DrawActorStrip(&g_sEnemy[e->type],AnimF(&g_sEnemy[e->type]),ax,ay,ew,eh,e->face,al,e->hitFlash>0);
            else DrawActorTex(g_tEnemy[e->type],ax,ay,ew,eh,e->face,al,e->hitFlash>0); }
        else { Color col = e->hitFlash>0?(Color){255,255,255,255} : e->type==1?(Color){150,55,55,255} : e->type==2?(Color){150,80,185,255} : (Color){200,70,70,255}; DrawFigure(e->x,e->y,EW,EH,e->face,col,0,e->face,e->inCover); }
        if(!e->inCover && e->hp<e->maxhp){ float bw=e->type==3?EW*1.7f:EW, bx=e->x+EW*0.5f-bw*0.5f;   // bar only once damaged
            DrawRectangle((int)bx,(int)e->y-7,(int)bw,4,(Color){40,40,40,255});
            DrawRectangle((int)bx,(int)e->y-7,(int)(bw*e->hp/(float)e->maxhp),4,e->type==3?(Color){230,90,90,255}:(Color){90,220,90,255}); }
    }
    for(int i=0;i<MAXSHOT;i++){ Shot*s=&g_shot[i]; if(s->age<0||s->age>0.09f) continue; Color c=s->owner?(Color){255,150,60,255}:(Color){120,230,255,255}; DrawLineEx((Vector2){s->x1,s->y1},(Vector2){s->x2,s->y2},3,c); }
    for(int i=0;i<MAXBOMB;i++) if(g_bomb[i].active){ Bomb*b=&g_bomb[i]; DrawCircle((int)b->x,(int)b->y-6,9,(Color){40,40,46,255}); int blink=((int)(b->fuse*8))&1; DrawCircle((int)b->x,(int)b->y-16,3,blink?(Color){255,80,60,255}:(Color){110,40,30,255}); }
    if(g_boomT>0){ float k=1.0f-(g_boomT/0.35f); float rad=BOMB_RADIUS*k; DrawCircleLines((int)g_boomX,(int)g_boomY,rad,(Color){255,180,60,(unsigned char)(220*(1-k))}); DrawCircle((int)g_boomX,(int)g_boomY,rad*0.5f,(Color){255,140,40,(unsigned char)(120*(1-k))}); }
    if(!P.dead){
        if(g_sprites){ float a=P.inCover?0.5f:(P.iframes>0?0.5f:1.0f); int moving=fabsf(P.vx)>5; Strip*s=NULL;
            if(P.climbT>0 && g_sHeroClimb.ok) s=&g_sHeroClimb;
            else if(P.hurtT>0 && g_sHeroHurt.ok) s=&g_sHeroHurt;
            else if(P.muzzle>0 && g_sHeroFire.ok) s=&g_sHeroFire;
            else if(moving && g_sHeroWalk.ok) s=&g_sHeroWalk;
            else if(g_sHeroIdle.ok) s=&g_sHeroIdle;
            if(s) DrawActorStrip(s,AnimF(s),P.x,P.y,PW,PH,P.face,a,0);
            else { Texture2D ht=(moving && ((int)(GetTime()*8)&1))?g_tHeroWalk:g_tHeroIdle; DrawActorTex(ht,P.x,P.y,PW,PH,P.face,a,0); }
            if(P.muzzle>0) DrawCircle((int)(P.muzzleDir>0?P.x+PW+6:P.x-6),(int)(P.y+PH*0.42f+2),8,(Color){255,230,120,235}); }
        else { Color pc = P.iframes>0?(Color){255,255,255,255}:(Color){90,150,235,255}; DrawFigure(P.x,P.y,PW,PH,P.face,pc,P.muzzle>0,P.muzzleDir,P.inCover); }
        if(P.meleeT>0){ float mx=P.face>0?P.x+PW:P.x; DrawLineEx((Vector2){mx,P.y+PH*0.30f},(Vector2){mx+P.face*24,P.y+PH*0.55f},3,(Color){235,240,255,225}); DrawLineEx((Vector2){mx,P.y+PH*0.58f},(Vector2){mx+P.face*22,P.y+PH*0.36f},2,(Color){200,220,255,150}); }   // knife slash
    } else DrawRectangle((int)P.x,(int)(P.y+PH-8),(int)PW,8,(Color){120,40,40,220});
    if(g_hitboxes){ DrawRectangleLinesEx((Rectangle){P.x,P.y,PW,PH},1,GREEN); for(int i=0;i<g_enN;i++) if(g_en[i].alive) DrawRectangleLinesEx((Rectangle){g_en[i].x,g_en[i].y,EW,EH},1,RED); }
    if(g_fx){ DrawParticles(); DrawFloats(); }   // world-space, on top of actors
    EndMode2D();
    if(g_fx) DrawLighting();    // screen-space atmosphere
}

static void Bar(int x,int y,int w,int h,float frac,Color fg){ DrawRectangle(x,y,w,h,(Color){30,30,36,220}); DrawRectangle(x,y,(int)(w*clampf(frac,0,1)),h,fg); DrawRectangleLines(x,y,w,h,(Color){10,10,12,255}); }

// "NOW ENTERING <area>" banner — fades in/out over its lifetime (cosmetic).
static void DrawAreaCard(void){
    if(g_areaCardT<=0) return;
    float a=1.0f;
    if(g_areaCardT>2.4f) a=(2.8f-g_areaCardT)/0.4f;        // fade in
    else if(g_areaCardT<0.6f) a=g_areaCardT/0.6f;          // fade out
    if(a<0)a=0; if(a>1)a=1;
    unsigned char A=(unsigned char)(a*255);
    int cy=(int)(SCREEN_H*0.34f);
    DrawRectangle(0,cy-44,SCREEN_W,88,(Color){0,0,0,(unsigned char)(a*150)});
    Color gold={210,185,120,A};
    DrawRectangle(0,cy-44,SCREEN_W,2,gold);
    DrawRectangle(0,cy+42,SCREEN_W,2,gold);
    const char*kick="N O W   E N T E R I N G";
    int kw=MeasureText(kick,16); DrawText(kick,SCREEN_W/2-kw/2,cy-32,16,(Color){150,160,180,A});
    int nw=MeasureText(g_areaName,46);
    DrawText(g_areaName,SCREEN_W/2-nw/2+2,cy-8+2,46,(Color){0,0,0,(unsigned char)(a*160)});   // drop shadow
    DrawText(g_areaName,SCREEN_W/2-nw/2,cy-8,46,(Color){235,225,200,A});
}
static void DrawHUD(void){
    if(P.hurtT>0 && P.hp>0) DrawRectangle(0,0,SCREEN_W,SCREEN_H,(Color){200,40,40,(unsigned char)(P.hurtT/0.25f*110)});   // damage flash
    if(g_sprites && P.hp>0 && P.hp<30 && !P.dead){ float p=0.45f+0.45f*sinf((float)GetTime()*6.0f);   // low-HP danger pulse
        DrawTexturePro(g_tVign,(Rectangle){0,0,(float)g_tVign.width,(float)g_tVign.height},(Rectangle){0,0,SCREEN_W,SCREEN_H},(Vector2){0,0},0,(Color){200,30,30,(unsigned char)(p*120)}); }
    DrawRectangle(0,0,SCREEN_W,40,(Color){0,0,0,150});
    DrawText("THORN",14,11,20,(Color){200,210,235,255});
    Bar(110,12,180,16,P.hp/(float)P_HP_MAX,(Color){210,70,70,255});
    DrawText(TextFormat("HP %d",P.hp),116,12,16,RAYWHITE);
    DrawText(TextFormat("SHELLS %d/%d%s",P.mag,P.reserve,P.reloadT>0?" RELOAD":""),300,12,18,(Color){235,225,160,255});
    DrawText(TextFormat("BOMB %d  SHARD %d  KEY %d  GUN P%d S%d",P.bombs,P.shards,P.keys,P.gunPow,P.gunSpd),470,12,18,(Color){180,200,210,255});
    DrawText(TextFormat("%s / %s",g_areaName,g_roomName),SCREEN_W-380,12,18,(Color){150,160,180,255});
    DrawText(TextFormat("STATE %s",PStateName()),14,SCREEN_H-26,18,(Color){150,170,190,255});
    DrawText("A/D move  SPACE jump  J fire  K back  V knife  E place/T throw bomb  W use  ` debug",286,SCREEN_H-26,15,(Color){90,100,115,255});
    { int fps=GetFPS(); const char*ft=TextFormat("%d FPS",fps); int fw=MeasureText(ft,18);
      Color fc = fps>=60?(Color){120,210,120,255} : fps>=30?(Color){220,210,120,255} : (Color){220,110,110,255};
      DrawText(ft,SCREEN_W-fw-12,SCREEN_H-26,18,fc); }
    if(g_msgT>0){ int w=MeasureText(g_msg,22); DrawRectangle(0,50,SCREEN_W,30,(Color){0,0,0,150}); DrawText(g_msg,SCREEN_W/2-w/2,55,22,(Color){235,225,160,255}); }
    if(g_won){ const char*m=g_victory?"THE USURPER FALLS":g_areaClear?"AREA CLEAR":"LEVEL CLEAR"; Color col=g_victory?(Color){235,210,90,255}:(Color){120,230,140,255}; int w=MeasureText(m,52); DrawRectangle(0,SCREEN_H/2-50,SCREEN_W,100,(Color){0,0,0,170}); DrawText(m,SCREEN_W/2-w/2,SCREEN_H/2-26,52,col); }
    if(P.dead){ const char*m="YOU DIED"; int w=MeasureText(m,52); DrawText(m,SCREEN_W/2-w/2,SCREEN_H/2-26,52,(Color){220,80,80,255}); }
    if(g_paused){
        DrawRectangle(0,0,SCREEN_W,SCREEN_H,(Color){0,0,0,150});
        const char*m="PAUSED"; int w=MeasureText(m,44); DrawText(m,SCREEN_W/2-w/2,SCREEN_H/2-120,44,RAYWHITE);
        for(int i=0;i<3;i++){ int sel=(i==g_pauseSel); int fs=28; int iw=MeasureText(g_pauseItems[i],fs); int yy=SCREEN_H/2-30+i*46;
            if(sel){ DrawRectangle(SCREEN_W/2-iw/2-18,yy-6,iw+36,fs+12,(Color){210,185,120,40}); DrawText(">",SCREEN_W/2-iw/2-34,yy,fs,(Color){210,185,120,255}); }
            DrawText(g_pauseItems[i],SCREEN_W/2-iw/2,yy,fs,sel?(Color){235,225,200,255}:(Color){150,160,175,255}); }
        const char*h="Up/Down select    Enter confirm    P resume"; int hw=MeasureText(h,18); DrawText(h,SCREEN_W/2-hw/2,SCREEN_H/2+130,18,(Color){130,140,155,255}); }
}

static void DrawOverlay(void){
    if(!g_overlay) return;
    int x=SCREEN_W-330,y=48,w=318;
    DrawRectangle(x,y,w,266,(Color){0,0,0,180}); DrawRectangleLines(x,y,w,266,(Color){80,90,110,255});
    DrawText("-- DEBUG (live snapshot) --",x+10,y+8,16,(Color){150,220,150,255});
    DrawText(TextFormat("frame %ld  fps %d  rate %d",g_frame,GetFPS(),g_rate),x+10,y+30,16,RAYWHITE);
    DrawText(TextFormat("room %s  bridge %d",g_roomName,g_bridgeOn),x+10,y+50,16,RAYWHITE);
    DrawText(TextFormat("pos %.0f,%.0f vel %.0f,%.0f",P.x,P.y,P.vx,P.vy),x+10,y+70,16,RAYWHITE);
    DrawText(TextFormat("face %d  st %s  grnd %d cov %d",P.face,PStateName(),P.onGround,P.inCover),x+10,y+90,16,RAYWHITE);
    DrawText(TextFormat("keys %d bombs %d shards %d",P.keys,P.bombs,P.shards),x+10,y+110,16,(Color){200,200,120,255});
    DrawText("recent events:",x+10,y+134,16,(Color){150,200,220,255});
    for(int i=0;i<8;i++){ int idx=(g_evHead+i)%8; if(g_evlog[idx][0]) DrawText(g_evlog[idx],x+10,y+154+i*13,12,(Color){170,180,190,255}); }
}

static void DrawTitle(void){
    DrawRectangleGradientV(0,0,SCREEN_W,SCREEN_H,(Color){18,22,36,255},(Color){6,7,11,255});
    if(g_fx){ BeginBlendMode(BLEND_ADDITIVE); DrawTexturePro(g_tLight,(Rectangle){0,0,128,128},(Rectangle){SCREEN_W/2-360,40,720,360},(Vector2){0,0},0,(Color){70,90,150,90}); EndBlendMode(); }
    const char*ti="THORN"; int ts=128, tw=MeasureText(ti,ts);
    DrawText(ti,SCREEN_W/2-tw/2+4,154,ts,(Color){8,10,16,255});
    DrawText(ti,SCREEN_W/2-tw/2,150,ts,(Color){120,162,236,255});
    const char*sub="a cinematic platformer"; int sw=MeasureText(sub,24);
    DrawText(sub,SCREEN_W/2-sw/2,292,24,(Color){150,160,188,255});
    const char*items[3]={"New Game","Continue","Quit"};
    for(int i=0;i<3;i++){ int sel=(i==g_menuSel),fs=32,w=MeasureText(items[i],fs); int yy=388+i*52;
        Color c=sel?(Color){240,226,150,255}:(Color){140,150,172,255};
        if(sel) DrawText(">",SCREEN_W/2-w/2-34,yy,fs,c);
        DrawText(items[i],SCREEN_W/2-w/2,yy,fs,c); }
    const char*d=TextFormat("Difficulty:  < %s >",g_diffName[g_diff]); int dw=MeasureText(d,22); DrawText(d,SCREEN_W/2-dw/2,556,22,(Color){210,185,120,255});
    const char*h="Up/Down select    Left/Right difficulty    Enter start    Esc quit"; int hw=MeasureText(h,18);
    DrawText(h,SCREEN_W/2-hw/2,SCREEN_H-54,18,(Color){95,105,122,255});
    DrawText("original tribute to Blackthorne (1994)  -  CC0 art",14,SCREEN_H-26,14,(Color){66,74,88,255});
    DrawText(TextFormat("%d FPS",GetFPS()),SCREEN_W-78,SCREEN_H-26,18,(Color){90,140,90,255});
}

// ---- Audio (procedural: synthesized in code, no asset files) ----------------
// wf: 0=sine 1=square 2=noise. A simple decaying envelope shapes each one-shot.
static Sound GenTone(float dur,float f0,float f1,int wf,float vol){
    int sr=22050; int n=(int)(sr*dur); if(n<1)n=1; short*d=(short*)malloc((size_t)n*sizeof(short));
    float ph=0;
    for(int i=0;i<n;i++){ float t=(float)i/n; float f=f0+(f1-f0)*t; ph+=f/sr;
        float s = (wf==2)?((rand()%2001)/1000.0f-1.0f) : (wf==1)?((sinf(ph*6.2831853f)>=0)?1.0f:-1.0f) : sinf(ph*6.2831853f);
        d[i]=(short)(s*expf(-3.2f*t)*vol*30000.0f);
    }
    Wave w; w.frameCount=(unsigned)n; w.sampleRate=(unsigned)sr; w.sampleSize=16; w.channels=1; w.data=d;
    Sound snd=LoadSoundFromWave(w); free(d); return snd;
}
static void InitAudio(void){
    InitAudioDevice(); if(!IsAudioDeviceReady()) return;
    // Shotgun: use the supplied sample if present, else fall back to the synth.
    g_snd[SND_FIRE]      = FileExists("assets/sounds/shotgun_fire.mp3") ? LoadSound("assets/sounds/shotgun_fire.mp3") : GenTone(0.18f,220,80,2,0.9f);
    g_snd[SND_DRY]       = GenTone(0.05f,420,300,1,0.4f);
    g_snd[SND_RELOAD]    = GenTone(0.12f,180,140,1,0.5f);
    g_snd[SND_ENEMYFIRE] = GenTone(0.16f,160, 70,2,0.7f);
    g_snd[SND_HIT]       = GenTone(0.12f,150, 60,1,0.7f);
    g_snd[SND_DEATH]     = GenTone(0.40f,300, 60,0,0.7f);
    g_snd[SND_PICKUP]    = GenTone(0.16f,600,1000,0,0.6f);
    g_snd[SND_LEVER]     = GenTone(0.12f,200,120,1,0.6f);
    g_snd[SND_BOMB]      = GenTone(0.50f,120, 40,2,1.0f);
    g_snd[SND_UPGRADE]   = GenTone(0.30f,500,900,0,0.6f);
    g_snd[SND_JUMP]      = FileExists("assets/sounds/jump.mp3") ? LoadSound("assets/sounds/jump.mp3") : GenTone(0.14f,300,520,0,0.4f);
    g_snd[SND_MELEE]     = GenTone(0.10f,600,180,2,0.5f);
    g_snd[SND_STEP]      = GenTone(0.05f,110, 70,2,0.22f);
    g_snd[SND_LAND]      = FileExists("assets/sounds/land.mp3")   ? LoadSound("assets/sounds/land.mp3")   : GenTone(0.10f,150,55,2,0.40f);
    g_snd[SND_PORTAL]    = FileExists("assets/sounds/portal.mp3") ? LoadSound("assets/sounds/portal.mp3") : GenTone(0.5f,300,1400,0,0.6f);
    // Looping footstep samples (run / Shift-walk).
    if(FileExists("assets/sounds/run_concrete.mp3")){ g_runMus =LoadMusicStream("assets/sounds/run_concrete.mp3"); g_runMus.looping=true;  SetMusicVolume(g_runMus,0.60f);  g_runMusOk=1; }
    if(FileExists("assets/sounds/walk.mp3")){         g_walkMus=LoadMusicStream("assets/sounds/walk.mp3");        g_walkMus.looping=true; SetMusicVolume(g_walkMus,0.55f); g_walkMusOk=1; }
    // Enemy-death samples: assets/sounds/enemy_death_<n>.mp3, picked at random (drop in more to vary deaths).
    for(int i=0;i<MAX_DEATHSND;i++){ char p[64]; snprintf(p,sizeof p,"assets/sounds/enemy_death_%d.mp3",i); if(!FileExists(p)) break; g_deathSnd[g_deathSndN++]=LoadSound(p); }
    for(int i=0;i<MAX_DEATHSND;i++){ char p[64]; snprintf(p,sizeof p,"assets/sounds/player_hurt_%d.mp3",i); if(!FileExists(p)) break; g_hurtSnd[g_hurtSndN++]=LoadSound(p); }
    g_audio=1;
}

// Loop the concrete-running / walking samples while the hero moves on the ground
// (continuous Music streams, not per-step blips). Windowed only.
static void UpdateFootstepAudio(void){
    if(!g_audio || g_headless) return;
    int moving  = P.onGround && !P.inCover && !P.dead && !g_paused && !g_won && fabsf(P.vx) > 14.0f;
    int running = moving && fabsf(P.vx) > WALK_SPD + 10.0f;
    int walking = moving && !running;
    if(g_runMusOk){  if(running){ if(!IsMusicStreamPlaying(g_runMus))  PlayMusicStream(g_runMus);  UpdateMusicStream(g_runMus);  } else if(IsMusicStreamPlaying(g_runMus))  StopMusicStream(g_runMus); }
    if(g_walkMusOk){ if(walking){ if(!IsMusicStreamPlaying(g_walkMus)) PlayMusicStream(g_walkMus); UpdateMusicStream(g_walkMus); } else if(IsMusicStreamPlaying(g_walkMus)) StopMusicStream(g_walkMus); }
}

// ---- Self-test: validate the whole multi-area room graph (no window) --------
static int RunSelfTest(void){
    static const char* paths[] = {
        "levels/sunken_mines/entrance.lvl","levels/sunken_mines/shaft.lvl","levels/sunken_mines/cavern.lvl","levels/sunken_mines/depths.lvl",
        "levels/the_mire/entrance.lvl","levels/the_mire/mire.lvl","levels/the_mire/sink.lvl",
        "levels/the_ashlands/entrance.lvl","levels/the_ashlands/ash.lvl","levels/the_ashlands/forge.lvl",
        "levels/the_usurpers_keep/entrance.lvl","levels/the_usurpers_keep/hall.lvl","levels/the_usurpers_keep/throne.lvl",
    };
    int NP=(int)(sizeof(paths)/sizeof(paths[0])), errors=0, totalDoors=0, totalShards=0, bosses=0;
    static char dOwner[160][160], dTarget[160][48]; static int dSpawn[160]; int nd=0;
    for(int i=0;i<NP;i++){ FILE*f=fopen(paths[i],"r"); if(!f){ DebugLog("selftest","\"error\":\"missing room\",\"path\":\"%s\"",JStr(paths[i])); errors++; continue; } fclose(f);
        LoadRoom(paths[i],-1,0);
        for(int k=0;k<g_pkN;k++) if(g_pk[k].kind=='*') totalShards++;
        for(int k=0;k<g_enN;k++) if(g_en[k].type==3) bosses++;
        DebugLog("selftest","\"room\":\"%s\",\"doors\":%d,\"enemies\":%d,\"pickups\":%d",JStr(paths[i]),g_doorN,g_enN,g_pkN);
        for(int d=0;d<g_doorN;d++) if(nd<160){ snprintf(dOwner[nd],160,"%s",paths[i]); snprintf(dTarget[nd],48,"%s",g_doors[d].target); dSpawn[nd]=g_doors[d].targetSpawn; nd++; totalDoors++; }
    }
    for(int k=0;k<nd;k++){ if(dTarget[k][0]==0||!strcmp(dTarget[k],"exit")) continue;
        char tp[220]; ResolveRoomPath(tp,sizeof tp,dOwner[k],dTarget[k]);
        FILE*tf=fopen(tp,"r"); if(!tf){ DebugLog("selftest","\"error\":\"door target not found\",\"from\":\"%s\",\"target\":\"%s\"",JStr(dOwner[k]),JStr(dTarget[k])); errors++; continue; } fclose(tf);
        LoadRoom(tp,-1,0); int found=0; for(int d=0;d<g_doorN;d++) if(g_doors[d].id==dSpawn[k]){ found=1; break; }
        if(!found){ DebugLog("selftest","\"error\":\"spawn door missing\",\"target\":\"%s\",\"spawn\":%d",JStr(tp),dSpawn[k]); errors++; }
    }
    DebugLog("selftest","\"done\":true,\"rooms\":%d,\"doors\":%d,\"shards\":%d,\"bosses\":%d,\"errors\":%d",NP,totalDoors,totalShards,bosses,errors);
    fprintf(stderr,"selftest: %d rooms, %d doors, %d shards, %d boss(es), %d errors\n",NP,totalDoors,totalShards,bosses,errors);
    return errors;
}

// One windowed-loop iteration, factored out so the browser (emscripten) can drive
// it via emscripten_set_main_loop while desktop calls it from a while-loop. The
// behaviour is identical on desktop — `continue` simply becomes `return`.
static float g_acc=0; static int g_shotDone=0; static int g_running=1;
static void Frame(void){
    if(g_scene==SCENE_TITLE){
        if(IsKeyPressed(KEY_UP)||IsKeyPressed(KEY_W))   g_menuSel=(g_menuSel+2)%3;
        if(IsKeyPressed(KEY_DOWN)||IsKeyPressed(KEY_S)) g_menuSel=(g_menuSel+1)%3;
        if(IsKeyPressed(KEY_LEFT)||IsKeyPressed(KEY_A)) { g_diff=(g_diff+2)%3; SaveOptions(); }
        if(IsKeyPressed(KEY_RIGHT)||IsKeyPressed(KEY_D)){ g_diff=(g_diff+1)%3; SaveOptions(); }
        if(IsKeyPressed(KEY_ENTER)||IsKeyPressed(KEY_SPACE)){
            if(g_menuSel==0){ NewGame(); g_scene=SCENE_PLAY; }
            else if(g_menuSel==1){ ContinueGame(); g_scene=SCENE_PLAY; }
            else g_running=0;
        }
        BeginDrawing(); ClearBackground((Color){10,11,16,255}); DrawTitle(); EndDrawing();
        return;
    }
    UpdateFootstepAudio();   // loop run/walk samples while the hero moves
    if(IsKeyPressed(KEY_GRAVE)||IsKeyPressed(KEY_TAB)) g_overlay=!g_overlay;
    if(IsKeyPressed(KEY_P)){ g_paused=!g_paused; g_pauseSel=0; DebugLog("pause","\"paused\":%s",g_paused?"true":"false"); }
    if(g_paused){   // self-contained pause menu (Resume / Restart checkpoint / Quit to title)
        if(IsKeyPressed(KEY_UP)||IsKeyPressed(KEY_W))   g_pauseSel=(g_pauseSel+2)%3;
        if(IsKeyPressed(KEY_DOWN)||IsKeyPressed(KEY_S)) g_pauseSel=(g_pauseSel+1)%3;
        int go=IsKeyPressed(KEY_ENTER)||IsKeyPressed(KEY_SPACE);
        if(IsKeyPressed(KEY_BACKSPACE)){ g_pauseSel=2; go=1; }   // legacy shortcut: backspace -> title
        if(go){
            if(g_pauseSel==1){ RespawnAtCheckpoint(); g_won=0; g_areaClear=0; g_paused=0; DebugLog("pause","\"action\":\"restart\""); }
            else if(g_pauseSel==2){ g_scene=SCENE_TITLE; g_paused=0; g_menuSel=0; DebugLog("pause","\"action\":\"title\""); return; }
            else { g_paused=0; DebugLog("pause","\"paused\":false"); }
        }
        BeginDrawing(); ClearBackground((Color){20,22,30,255}); DrawWorld(); DrawHUD(); DrawOverlay(); EndDrawing();
        return;
    }
    if(IsKeyPressed(KEY_G)){ g_god=!g_god; DebugLog("mode","\"god\":%s",g_god?"true":"false"); }
    if(IsKeyPressed(KEY_H)) g_hitboxes=!g_hitboxes;
    if(IsKeyPressed(KEY_F)){ g_fx=!g_fx; SaveOptions(); DebugLog("mode","\"fx\":%s",g_fx?"true":"false"); }
    if(IsKeyPressed(KEY_N)){ g_noEnemies=!g_noEnemies; DebugLog("mode","\"noEnemies\":%s",g_noEnemies?"true":"false"); }
    if(IsKeyPressed(KEY_R)){ RespawnAtCheckpoint(); g_won=0; g_areaClear=0; }

    Input in = g_demo?DemoInput():KeyInput();

    float ft=GetFrameTime(); if(ft>0.05f) ft=0.05f;
    if(g_areaCardT>0) g_areaCardT-=ft;
    if(!g_paused && !g_won){
        g_acc+=ft; int steps=0;
        while(g_acc>=DT && steps<5){
            SimStep(in);
            in.up=in.down=in.fireF=in.fireB=in.use=in.cycle=in.jump=in.melee=in.throwb=0;   // consume edges after first sub-step
            g_acc-=DT; steps++; g_frame++;
            if((g_rate>0 && g_frame%g_rate==0) || g_rate<=0) EmitState();
            if(g_maxFrames && g_frame>=g_maxFrames) g_running=0;
            if(g_won) break;   // stop sub-stepping once the area is cleared
        }
    } else UpdateCam();

    BeginDrawing();
    ClearBackground((Color){20,22,30,255});
    DrawWorld(); DrawHUD(); DrawAreaCard(); DrawOverlay();
    EndDrawing();

    if(g_shotFrame && g_frame>=g_shotFrame && !g_shotDone){ TakeScreenshot("thorn-shot.png"); g_shotDone=1; DebugLog("shot","\"file\":\"thorn-shot.png\",\"frame\":%ld",g_frame); }
#ifdef __EMSCRIPTEN__
    if(!g_running) emscripten_cancel_main_loop();
#endif
}

#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
extern void SetMacDockIcon(const char *path);   // src/macicon.m (Cocoa)
#endif

// ---- main -------------------------------------------------------------------
int main(int argc,char**argv){
    int dump=0,genassets=0,docontinue=0; char pwarg[16]="";
    LoadOptions();   // file defaults; CLI flags below override
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--debug")) g_debug=1;
        else if(!strcmp(argv[i],"--no-enemies")) g_noEnemies=1;
        else if(!strcmp(argv[i],"--god")) g_god=1;
        else if(!strcmp(argv[i],"--demo")) g_demo=1;
        else if(!strcmp(argv[i],"--headless")){ g_headless=1; g_debug=1; }
        else if(!strcmp(argv[i],"--selftest")){ g_selftest=1; g_headless=1; g_debug=1; }
        else if(!strcmp(argv[i],"--room")&&i+1<argc) snprintf(g_roomStart,sizeof g_roomStart,"%s",argv[++i]);
        else if(!strcmp(argv[i],"--spawn")&&i+1<argc) g_startSpawn=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--rate")&&i+1<argc) g_rate=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--frames")&&i+1<argc) g_maxFrames=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--shot")&&i+1<argc) g_shotFrame=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--dumpsprites")) dump=1;
        else if(!strcmp(argv[i],"--gen-assets")) genassets=1;
        else if(!strcmp(argv[i],"--skiptitle")) g_skiptitle=1;
        else if(!strcmp(argv[i],"--diff")&&i+1<argc){ g_diff=atoi(argv[++i]); g_diff=g_diff<0?0:g_diff>2?2:g_diff; }
        else if(!strcmp(argv[i],"--nofx")) g_fx=0;
        else if(!strcmp(argv[i],"--continue")) docontinue=1;
        else if(!strcmp(argv[i],"--password")&&i+1<argc) snprintf(pwarg,sizeof pwarg,"%s",argv[++i]);
    }
    if(dump) return DumpSprites();   // export thorn-sprites.png and exit (no window)
    if(genassets) return GenAssets();   // (re)generate assets/sprites/*.png and exit
    if(pwarg[0]) for(unsigned k=0;k<sizeof(g_pwTable)/sizeof(g_pwTable[0]);k++) if(!strcmp(pwarg,g_pwTable[k].code)){ snprintf(g_roomStart,sizeof g_roomStart,"%s",g_pwTable[k].path); break; }
    if(docontinue){ FILE*sf=fopen("thorn-save.txt","r"); if(sf){ char code[32]="",path[160]=""; if(fscanf(sf,"%31s %159s",code,path)==2 && path[0]) snprintf(g_roomStart,sizeof g_roomStart,"%s",path); fclose(sf); } }
    if(g_debug){ g_dbg=fopen("thorn-debug.log","w"); if(!g_dbg) g_dbg=stderr; }
    DebugLog("options","\"diff\":%d,\"fx\":%s",g_diff,g_fx?"true":"false");
    for(int i=0;i<MAXSHOT;i++) g_shot[i].age=-1;

    if(g_selftest){ int e=RunSelfTest(); if(g_dbg&&g_dbg!=stderr) fclose(g_dbg); return e?1:0; }

    if(!g_headless){
        SetTraceLogLevel(LOG_WARNING);
        /* no MSAA: sprites are point-filtered, so MSAA only burns fill rate */
        InitWindow(SCREEN_W,SCREEN_H,"Thorn - cinematic platformer (raylib " THORN_RAYLIB ")");
        if(!IsWindowReady()){   // no display (SSH/CI/sandbox): bail before touching the GL context
            DebugLog("error","\"msg\":\"window-init-failed\"");
            fprintf(stderr,"Thorn: display unavailable (window init failed). Use './build/thorn --headless --frames N'.\n");
            if(g_dbg && g_dbg!=stderr) fclose(g_dbg);
            return 1;
        }
        SetTargetFPS(120);
        InitAudio();      // needs a live device; safe no-op if it fails to init
        InitSprites();    // uploads generated textures; needs the GL context above
        if(FileExists("assets/thorn_icon.png")){          // window icon (Linux/Windows)
            Image ic=LoadImage("assets/thorn_icon.png");
            if(ic.data){ ImageFormat(&ic,PIXELFORMAT_UNCOMPRESSED_R8G8B8A8); SetWindowIcon(ic); UnloadImage(ic); }
        }
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
        SetMacDockIcon("assets/thorn_icon.png");          // Dock / Cmd-Tab (GLFW icon is a no-op on macOS)
#endif
    }

    DebugLog("boot","\"raylib\":\"%s\",\"build\":\"0.9.1\",\"headless\":%s",THORN_RAYLIB,g_headless?"true":"false");
    DebugLog("window","\"w\":%d,\"h\":%d,\"monitor\":%d",SCREEN_W,SCREEN_H,g_headless?0:GetCurrentMonitor());

    // New game: stats, then the first room (LoadRoom logs its own "level" event).
    if(g_headless || g_demo || g_skiptitle){ NewGame(); g_scene=SCENE_PLAY; }   // skip the title menu
    else g_scene=SCENE_TITLE;                                                    // windowed: show the menu

    if(g_headless){
        int running=1;
        while(running){
            SimStep(DemoFrameInput(g_frame));
            g_simTime+=DT; g_frame++;
            if(g_rate<=0 || g_frame%g_rate==0) EmitState();
            if(g_won){ DebugLog("win","\"frame\":%ld,\"area\":%s",g_frame,g_areaClear?"true":"false"); g_won=0; g_areaClear=0; }
            if(g_maxFrames && g_frame>=g_maxFrames) running=0;
        }
        DebugLog("shutdown","\"frames\":%ld",g_frame);
        if(g_dbg && g_dbg!=stderr) fclose(g_dbg);
        return 0;
    }

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(Frame, 0, 1);   // browser pumps frames; shutdown below is desktop-only
#else
    while(g_running && !WindowShouldClose()) Frame();   // Frame() is the loop body (see above)
#endif

    DebugLog("shutdown","\"frames\":%ld",g_frame);
    if(g_dbg && g_dbg!=stderr) fclose(g_dbg);
    if(g_audio){ for(int i=0;i<SND_N;i++) UnloadSound(g_snd[i]);
        for(int i=0;i<g_deathSndN;i++) UnloadSound(g_deathSnd[i]);
        for(int i=0;i<g_hurtSndN;i++) UnloadSound(g_hurtSnd[i]);
        if(g_runMusOk) UnloadMusicStream(g_runMus); if(g_walkMusOk) UnloadMusicStream(g_walkMus);
        CloseAudioDevice(); }
    CloseWindow();
    return 0;
}
