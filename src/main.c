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
#define WALK_SPD   95.0f
#define FRICTION   1700.0f
#define FALL_HURT  980.0f

#define P_HP_MAX   100
#define P_RANGE    380.0f
#define P_DMG      34
#define PUMP       0.55f
#define IFRAMES    0.60f
#define SPIKE_DMG  12

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
typedef struct { float x,y,vx,vy; int face; int hp; int alive; float fireT; float hitFlash; const char*st; } Enemy;
typedef struct { int c,r; char kind; int alive; } Pickup;     // H K B *
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
    int   hp, ammo, keys, bombs, shards;
    int   dead; float deadT;
} P;

static Enemy  g_en[MAXEN];   static int g_enN=0;
static Pickup g_pk[MAXPK];   static int g_pkN=0;
static Npc    g_npc[MAXNPC]; static int g_npcN=0;
static Shot   g_shot[MAXSHOT]; static int g_shotHead=0;

// ---- World grid -------------------------------------------------------------
// Rooms load from levels/<area>/<room>.lvl (format in DESIGN.md). A built-in
// fallback boots if a file is missing.
static char g_tiles[64][128];      // '#' solid, '.' air
static int  g_alcove[64][128];     // shadow-cover cells
static int  g_spike[64][128];      // hazard cells
static int  g_bridge[64][128];     // bridge cells: solid only while a lever is thrown
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
typedef struct { int c,r,id; char target[48]; int targetSpawn; int locked; char key[12]; } Door;
typedef struct { int c,r; } Lever;
typedef struct { int c,r,hit; } CheckMark;

static Door      g_doors[MAXDOOR];   static int g_doorN=0;
static Lever     g_levers[MAXLEVER]; static int g_leverN=0;
static CheckMark g_cps[MAXCP];       static int g_cpN=0;
static struct { char color[12]; int count; } g_keys[MAXKEY]; static int g_keyN=0;

static char g_areaName[48]="Sunken Mines", g_roomName[48]="-", g_roomPath[160]="";
static char g_cpPath[160]=""; static float g_cpX=0,g_cpY=0; static int g_cpFace=1;   // checkpoint
static int  g_pendActive=0, g_pendSpawn=-1; static char g_pendTarget[48]="";          // deferred door
static int  g_areaClear=0;
static Vector2 g_cam={0,0};

static int  KeyCount(const char*c){ for(int i=0;i<g_keyN;i++) if(!strcmp(g_keys[i].color,c)) return g_keys[i].count; return 0; }
static void KeyAdd(const char*c){ for(int i=0;i<g_keyN;i++) if(!strcmp(g_keys[i].color,c)){ g_keys[i].count++; return; } if(g_keyN<MAXKEY){ snprintf(g_keys[g_keyN].color,12,"%s",c); g_keys[g_keyN].count=1; g_keyN++; } }
static int  KeyTotal(void){ int n=0; for(int i=0;i<g_keyN;i++) n+=g_keys[i].count; return n; }

// ---- Flags / dev ------------------------------------------------------------
static int g_noEnemies=0, g_god=0, g_demo=0, g_paused=0, g_overlay=0, g_hitboxes=0, g_selftest=0;
static int g_rate=24, g_maxFrames=0, g_shotFrame=0, g_startSpawn=-1;
static long g_frame=0; static int g_won=0;
static char g_roomStart[160]="levels/sunken_mines/entrance.lvl";

static char g_evlog[8][80]; static int g_evHead=0;
static void Ev(const char*fmt,...){ va_list ap; va_start(ap,fmt); vsnprintf(g_evlog[g_evHead],sizeof g_evlog[0],fmt,ap); va_end(ap); g_evHead=(g_evHead+1)%8; }

// ---- Input ------------------------------------------------------------------
typedef struct { int left,right,walk;            // held
                 int up,down,fireF,fireB,use,cycle; } Input; // edge

static Input KeyInput(void){
    Input in={0};
    in.left  = IsKeyDown(KEY_LEFT)||IsKeyDown(KEY_A);
    in.right = IsKeyDown(KEY_RIGHT)||IsKeyDown(KEY_D);
    in.walk  = IsKeyDown(KEY_LEFT_SHIFT)||IsKeyDown(KEY_RIGHT_SHIFT);
    in.up    = IsKeyPressed(KEY_UP)||IsKeyPressed(KEY_W);
    in.down  = IsKeyPressed(KEY_DOWN)||IsKeyPressed(KEY_S);
    in.fireF = IsKeyPressed(KEY_SPACE)||IsKeyPressed(KEY_J);
    in.fireB = IsKeyPressed(KEY_K);
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
    Input in={0}; in.right=1;
    if(f%96==1)  in.fireF=1;
    if(f%22==3)  in.up=1;     // ~5.5 Hz: throws levers / uses doors / climbs in passing
    if(f%440==220) in.fireB=1;
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

static void ParseRoom(const char*text,int spawnDoor){
    for(int r=0;r<64;r++) for(int c=0;c<128;c++){ g_tiles[r][c]='.'; g_alcove[r][c]=g_spike[r][c]=g_bridge[r][c]=0; }
    g_enN=g_pkN=g_npcN=g_doorN=g_leverN=g_cpN=0; g_bridgeOn=0; g_W=g_H=0;
    int spawnC=-1,spawnR=-1;
    struct { int used; char target[48]; int spawn; int locked; char key[12]; } dm[10];
    for(int i=0;i<10;i++){ dm[i].used=0; dm[i].target[0]=0; dm[i].spawn=0; dm[i].locked=0; dm[i].key[0]=0; }

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
                        if(got>=5&&!strcmp(lw,"lock")){ dm[id].locked=1; snprintf(dm[id].key,12,"%s",cl);} } }
                continue;   // @lever ignored in M1 (grid 'L' + 'b' cells drive the bridge)
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
                case 'b': g_bridge[r][c]=1; break;
                case 'S': g_alcove[r][c]=1; break;
                case '^': g_spike[r][c]=1; break;
                case 'C': if(g_cpN<MAXCP) g_cps[g_cpN++]=(CheckMark){c,r,0}; break;
                case 'P': spawnC=c; spawnR=r; break;
                case 'g': if(g_enN<MAXEN){ Enemy*e=&g_en[g_enN++]; e->x=c*TILE+(TILE-EW)/2; e->y=feet-EH; e->vx=e->vy=0; e->face=-1; e->hp=E_HP; e->alive=1; e->fireT=E_INTERVAL*0.5f; e->hitFlash=0; e->st="IDLE"; } break;
                case 'n': if(g_npcN<MAXNPC) g_npc[g_npcN++]=(Npc){c,r,0}; break;
                case 'H': case 'B': case '*': case 'K': if(g_pkN<MAXPK) g_pk[g_pkN++]=(Pickup){c,r,t,1}; break;
                case 'L': if(g_leverN<MAXLEVER) g_levers[g_leverN++]=(Lever){c,r}; break;
                case '0':case '1':case '2':case '3':case '4':case '5':case '6':case '7':case '8':case '9':{
                    int id=t-'0'; if(g_doorN<MAXDOOR){ Door*D=&g_doors[g_doorN++]; D->c=c; D->r=r; D->id=id;
                        if(dm[id].used){ snprintf(D->target,48,"%s",dm[id].target); D->targetSpawn=dm[id].spawn; D->locked=dm[id].locked; snprintf(D->key,12,"%s",dm[id].key); }
                        else { D->target[0]=0; D->targetSpawn=0; D->locked=0; D->key[0]=0; } }
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
        P.vx=P.vy=0; P.onGround=1; P.inCover=0; P.climbT=0; P.dead=0; P.deadT=0; P.iframes=0; P.muzzle=0;
    }
}

static void LoadRoom(const char*path,int spawnDoor,int setCheckpoint){
    static char buf[1<<15];
    FILE*f=fopen(path,"r");
    if(f){ size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f); ParseRoom(buf,spawnDoor); }
    else { ParseRoom(FALLBACK_ROOM,spawnDoor); DebugLog("warn","\"msg\":\"room file missing; using fallback\",\"path\":\"%s\"",JStr(path)); }
    snprintf(g_roomPath,sizeof g_roomPath,"%s",path);
    if(setCheckpoint){ snprintf(g_cpPath,sizeof g_cpPath,"%s",g_roomPath); g_cpX=P.x; g_cpY=P.y; g_cpFace=P.face; }
    DebugLog("level","\"area\":\"%s\",\"room\":\"%s\",\"w\":%d,\"h\":%d,\"enemies\":%d,\"doors\":%d,\"pickups\":%d,\"levers\":%d",
             JStr(g_areaName),JStr(g_roomName),g_W,g_H,g_enN,g_doorN,g_pkN,g_leverN);
}

static void RespawnAtCheckpoint(void){
    P.hp=P_HP_MAX; P.dead=0; P.deadT=0; P.iframes=0; P.vx=P.vy=0;
    if(g_cpPath[0]){ LoadRoom(g_cpPath,-2,0); P.x=g_cpX; P.y=g_cpY; P.face=g_cpFace; P.onGround=1; P.inCover=0; P.climbT=0; }
    DebugLog("respawn","\"room\":\"%s\",\"x\":%.1f,\"y\":%.1f",JStr(g_roomName),P.x,P.y); Ev("respawn @ %s",g_roomName);
}

static void SpawnShot(float x1,float y1,float x2,float y2,int owner){
    g_shot[g_shotHead]=(Shot){x1,y1,x2,y2,0,owner}; g_shotHead=(g_shotHead+1)%MAXSHOT;
}

static const char* PStateName(void){
    if(P.dead) return "DEAD";
    if(P.climbT>0) return P.climbDir>0?"CLIMB_UP":"CLIMB_DOWN";
    if(P.inCover) return "COVER";
    if(P.muzzle>0) return P.muzzleDir==P.face?"FIRE_FWD":"FIRE_BACK";
    if(!P.onGround) return "FALL";
    if(P.turning) return "TURN";
    if(fabsf(P.vx)>WALK_SPD+10) return "RUN";
    if(fabsf(P.vx)>5) return "WALK";
    return "IDLE";
}

// ---- Combat -----------------------------------------------------------------
static void Fire(int dir){
    if(P.fireCD>0) return;
    P.fireCD=PUMP; P.muzzle=0.09f; P.muzzleDir=dir;
    float mx = dir>0 ? P.x+PW : P.x, my = gunY();
    int hitIdx=-1; float best=P_RANGE;
    if(P.ammo>0){
        for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive) continue;
            float ex=e->x+EW*0.5f, ey=e->y+EH*0.5f, d=(ex-mx)*dir;
            if(d>0 && d<best && fabsf(ey-my)<TILE*0.6f && LineClear(mx,ex,my)){ best=d; hitIdx=i; }
        }
    }
    float endx = mx + dir*(hitIdx>=0?best:P_RANGE);
    SpawnShot(mx,my,endx,my,0);
    if(P.ammo>0) P.ammo--;
    if(hitIdx>=0){ Enemy*e=&g_en[hitIdx]; e->hp-=P_DMG; e->hitFlash=0.12f;
        if(e->hp<=0){ e->alive=0; e->st="DEAD"; Ev("enemy %d killed",hitIdx); DebugLog("death","\"who\":\"enemy\",\"i\":%d,\"x\":%.1f,\"y\":%.1f",hitIdx,e->x,e->y); }
        else DebugLog("hit","\"who\":\"enemy\",\"i\":%d,\"dmg\":%d,\"hp\":%d",hitIdx,P_DMG,e->hp);
    }
    DebugLog("fire","\"dir\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"face\":%d,\"ammo\":%d,\"hit\":%s,\"target\":%d",
             dir==P.face?"fwd":"back", mx,my,P.face,P.ammo, hitIdx>=0?"true":"false", hitIdx);
    Ev("fire %s%s", dir==P.face?"fwd":"back", hitIdx>=0?" HIT":"");
}

static void HurtPlayer(int dmg,const char*cause){
    if(g_god||P.iframes>0||P.dead) return;
    P.hp-=dmg; P.iframes=IFRAMES;
    DebugLog("hit","\"who\":\"player\",\"dmg\":%d,\"hp\":%d,\"cause\":\"%s\"",dmg,P.hp,cause);
    Ev("player hit -%d (%s)",dmg,cause);
    if(P.hp<=0){ P.hp=0; P.dead=1; P.deadT=0; DebugLog("death","\"who\":\"player\",\"x\":%.1f,\"y\":%.1f,\"cause\":\"%s\"",P.x,P.y,cause); Ev("player DIED (%s)",cause); }
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
    if(P.onGround && vy0>FALL_HURT){ int dmg=(int)((vy0-FALL_HURT)/45.0f); if(dmg>0){ DebugLog("fall","\"vy\":%.0f,\"dmg\":%d,\"fatal\":false",vy0,dmg); HurtPlayer(dmg,"fall"); } }
}

static void UpdatePlayer(Input in){
    if(P.dead){ P.deadT+=DT; if(P.deadT>1.4f) RespawnAtCheckpoint(); return; }
    P.iframes=fmaxf(0,P.iframes-DT); P.fireCD=fmaxf(0,P.fireCD-DT); P.muzzle=fmaxf(0,P.muzzle-DT);

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
            if(D->locked && KeyCount(D->key)<=0){ DebugLog("door","\"id\":%d,\"locked\":true,\"need\":\"%s\"",D->id,JStr(D->key)); Ev("locked: need %s key",D->key); }
            else {
                if(D->locked){ DebugLog("door","\"id\":%d,\"unlocked\":\"%s\"",D->id,JStr(D->key)); Ev("unlocked %s door",D->key); }
                if(D->target[0]==0||!strcmp(D->target,"exit")){ g_areaClear=1; g_won=1; DebugLog("door","\"id\":%d,\"areaExit\":true",D->id); Ev("AREA CLEAR"); }
                else { snprintf(g_pendTarget,sizeof g_pendTarget,"%s",D->target); g_pendSpawn=D->targetSpawn; g_pendActive=1;
                       DebugLog("door","\"id\":%d,\"to\":\"%s\",\"spawn\":%d",D->id,JStr(D->target),D->targetSpawn); }
            }
        }
        if(!handled){
            int didNpc=0; for(int i=0;i<g_npcN;i++) if(!g_npc[i].freed && pcol==g_npc[i].c){ g_npc[i].freed=1; didNpc=1; P.ammo+=6; DebugLog("npc","\"id\":%d,\"gave\":\"ammo\"",i); Ev("freed an Aurithi (+ammo)"); break; }
            if(!didNpc){
                int didLever=0; for(int i=0;i<g_leverN;i++) if(pcol==g_levers[i].c){ g_bridgeOn=!g_bridgeOn; didLever=1; DebugLog("lever","\"id\":%d,\"bridge\":%s",i,g_bridgeOn?"true":"false"); Ev("lever: bridge %s",g_bridgeOn?"out":"in"); break; }
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

    float target = in.walk?WALK_SPD:RUN_SPD; int d=in.right-in.left; P.turning=0;
    if(d!=0){
        if(d==P.face || fabsf(P.vx)<40){ P.face=d; P.vx+=d*ACCEL*DT; P.vx=clampf(P.vx,-target,target); }
        else { P.vx-=sgn(P.vx)*FRICTION*1.6f*DT; if(fabsf(P.vx)<40){P.vx=0;P.face=d;} P.turning=1; }
    } else { P.vx-=sgn(P.vx)*FRICTION*DT; if(fabsf(P.vx)<20)P.vx=0; }

    if(in.fireF) Fire(P.face);
    if(in.fireB) Fire(-P.face);

    P.vy=fminf(P.vy+GRAV*DT,MAXFALL);
    PlayerMoveX(); PlayerMoveY();

    int feetRow=(int)floorf((P.y+PH+1)/TILE)-1, ccol=(int)floorf(pcx()/TILE);
    if(P.onGround && feetRow>=0 && ccol>=0 && g_spike[feetRow][ccol]) HurtPlayer(SPIKE_DMG,"spike");
    if(P.y>g_H*TILE+80){ DebugLog("fall","\"fatal\":true"); if(g_god) RespawnAtCheckpoint(); else { P.hp=0; P.dead=1; P.deadT=0; DebugLog("death","\"who\":\"player\",\"cause\":\"pit\""); Ev("fell into the void"); } return; }

    for(int i=0;i<g_pkN;i++){ Pickup*p=&g_pk[i]; if(!p->alive) continue; if(p->c==ccol && p->r==feetRow){ p->alive=0;
        const char*nm="";
        switch(p->kind){ case 'H': P.hp=P.hp+30>P_HP_MAX?P_HP_MAX:P.hp+30; nm="health"; break;
                         case 'K': KeyAdd("gold"); P.keys=KeyTotal(); nm="gold key"; break;
                         case 'B': P.bombs++; nm="bomb"; break;
                         case '*': P.shards++; nm="shard"; break; }
        DebugLog("pickup","\"item\":\"%s\",\"x\":%d,\"y\":%d",nm,p->c*TILE,p->r*TILE); Ev("got %s",nm);
    } }
    for(int i=0;i<g_cpN;i++){ if(!g_cps[i].hit && g_cps[i].c==ccol && g_cps[i].r==feetRow){ g_cps[i].hit=1;
        snprintf(g_cpPath,sizeof g_cpPath,"%s",g_roomPath); g_cpX=P.x; g_cpY=P.y; g_cpFace=P.face;
        DebugLog("checkpoint","\"x\":%.1f,\"y\":%.1f",P.x,P.y); Ev("checkpoint reached"); } }
}

// ---- Enemy / world update ---------------------------------------------------
static void UpdateEnemies(void){
    for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive) continue;
        e->hitFlash=fmaxf(0,e->hitFlash-DT);
        float ex=e->x+EW*0.5f, ey=e->y+EH*0.5f, dx=pcx()-ex;
        if(fabsf(dx)<E_AGGRO && fabsf(pcy()-ey)<TILE*1.5f){ e->face=dx>0?1:-1; e->st="AIM"; } else e->st="IDLE";
        e->fireT-=DT;
        if(g_noEnemies) continue;
        if(e->fireT<=0 && (dx*e->face)>0 && !P.dead){
            e->fireT=E_INTERVAL;
            float mx=e->face>0?e->x+EW:e->x, my=ey, endx=mx+e->face*E_RANGE;
            SpawnShot(mx,my,endx,my,1);
            DebugLog("enemyfire","\"i\":%d,\"x\":%.1f,\"y\":%.1f,\"dir\":%d",i,mx,my,e->face);
            if(!P.inCover && fabsf(dx)<E_RANGE && fabsf(pcy()-my)<TILE*0.6f && LineClear(mx,pcx(),my)) HurtPlayer(E_DMG,"shot");
        }
    }
}
static void UpdateShots(void){ for(int i=0;i<MAXSHOT;i++) if(g_shot[i].age>=0) g_shot[i].age+=DT; }
static void UpdateCam(void){
    float tx=pcx(), ty=pcy();
    tx = (g_W*TILE<=SCREEN_W)? g_W*TILE*0.5f : clampf(tx,SCREEN_W*0.5f,g_W*TILE-SCREEN_W*0.5f);
    ty = (g_H*TILE<=SCREEN_H)? g_H*TILE*0.5f : clampf(ty,SCREEN_H*0.5f,g_H*TILE-SCREEN_H*0.5f);
    g_cam.x=tx-SCREEN_W*0.5f; g_cam.y=ty-SCREEN_H*0.5f;
}

// One simulation step. Applies a deferred door transition (a door can't reload
// the room mid-update while we're iterating its entities).
static void SimStep(Input in){
    UpdatePlayer(in);
    if(g_pendActive){
        g_pendActive=0;
        char path[160]; ResolveRoomPath(path,sizeof path,g_roomPath,g_pendTarget);
        DebugLog("transition","\"to\":\"%s\",\"spawn\":%d",JStr(g_pendTarget),g_pendSpawn);
        LoadRoom(path,g_pendSpawn,1);   // entering a room sets the checkpoint
        UpdateCam();
        return;
    }
    UpdateEnemies(); UpdateShots(); UpdateCam();
}

// ---- Recurring state snapshot ----------------------------------------------
static void EmitState(void){
    if(!g_debug||!g_dbg) return;
    fprintf(g_dbg,"{\"t\":%.3f,\"ev\":\"state\",\"frame\":%ld,\"fps\":%d,\"room\":\"%s\",",nowT(),g_frame,g_headless?120:GetFPS(),JStr(g_roomName));
    fprintf(g_dbg,"\"p\":{\"x\":%.1f,\"y\":%.1f,\"vx\":%.1f,\"vy\":%.1f,\"face\":%d,\"st\":\"%s\",\"hp\":%d,\"ammo\":%d,\"ground\":%s,\"cover\":%s},",
            P.x,P.y,P.vx,P.vy,P.face,PStateName(),P.hp,P.ammo,P.onGround?"true":"false",P.inCover?"true":"false");
    fprintf(g_dbg,"\"inv\":{\"keys\":%d,\"bombs\":%d,\"shards\":%d},\"bridge\":%s,",P.keys,P.bombs,P.shards,g_bridgeOn?"true":"false");
    fprintf(g_dbg,"\"cam\":[%.1f,%.1f],\"enemies\":[",g_cam.x,g_cam.y);
    int first=1; for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i];
        fprintf(g_dbg,"%s{\"i\":%d,\"x\":%.0f,\"y\":%.0f,\"hp\":%d,\"st\":\"%s\",\"face\":%d}",first?"":",",i,e->x,e->y,e->hp,e->alive?e->st:"DEAD",e->face); first=0; }
    int liveShots=0; for(int i=0;i<MAXSHOT;i++) if(g_shot[i].age>=0&&g_shot[i].age<0.2f) liveShots++;
    fprintf(g_dbg,"],\"shots\":%d}\n",liveShots); fflush(g_dbg);
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

static void DrawWorld(void){
    Camera2D cam={ .offset={SCREEN_W*0.5f,SCREEN_H*0.5f}, .target={g_cam.x+SCREEN_W*0.5f,g_cam.y+SCREEN_H*0.5f}, .rotation=0, .zoom=1 };
    BeginMode2D(cam);
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_alcove[r][c]){
        DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){14,16,26,255});
        DrawRectangleLinesEx((Rectangle){c*TILE+3,r*TILE+3,TILE-6,TILE-6},2,(Color){40,44,70,255});
    }
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_tiles[r][c]=='#'){
        DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){58,54,64,255});
        DrawRectangle(c*TILE,r*TILE,TILE,4,(Color){92,86,104,255});
    }
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_bridge[r][c]){
        if(g_bridgeOn){ DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){120,86,50,255}); DrawRectangle(c*TILE,r*TILE,TILE,4,(Color){160,120,70,255}); }
        else DrawRectangleLinesEx((Rectangle){c*TILE+2,r*TILE+2,TILE-4,TILE-4},1,(Color){90,66,40,170}); // ghost
    }
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_spike[r][c])
        for(int k=0;k<4;k++) DrawTriangle((Vector2){c*TILE+k*8.0f,(r+1)*TILE},(Vector2){c*TILE+k*8.0f+4,(r+1)*TILE-14},(Vector2){c*TILE+k*8.0f+8,(r+1)*TILE},(Color){180,60,60,255});
    for(int i=0;i<g_pkN;i++){ Pickup*p=&g_pk[i]; if(!p->alive) continue; float x=p->c*TILE+TILE*0.5f, y=(p->r+1)*TILE-16;
        if(p->kind=='H'){ DrawRectangle((int)x-3,(int)y-9,6,18,(Color){70,220,90,255}); DrawRectangle((int)x-9,(int)y-3,18,6,(Color){70,220,90,255}); }
        else if(p->kind=='K'){ DrawCircle((int)x-4,(int)y,6,(Color){235,205,70,255}); DrawRectangle((int)x,(int)y-2,12,4,(Color){235,205,70,255}); }
        else if(p->kind=='B'){ DrawCircle((int)x,(int)y,8,(Color){50,50,60,255}); DrawRectangle((int)x-1,(int)y-12,2,5,(Color){200,120,40,255}); }
        else if(p->kind=='*'){ DrawPoly((Vector2){x,y},4,9,45,(Color){90,220,235,255}); }
    }
    for(int i=0;i<g_leverN;i++){ float x=g_levers[i].c*TILE+TILE*0.5f,y=(g_levers[i].r+1)*TILE; DrawRectangle((int)x-2,(int)y-10,4,10,(Color){120,120,130,255}); DrawCircle((int)(x+(g_bridgeOn?7:-7)),(int)y-12,5,g_bridgeOn?(Color){90,220,120,255}:(Color){220,90,90,255}); }
    for(int i=0;i<g_cpN;i++){ float x=g_cps[i].c*TILE+TILE*0.5f,y=(g_cps[i].r+1)*TILE; Color cc=g_cps[i].hit?(Color){90,200,120,220}:(Color){80,90,110,150}; DrawRectangle((int)x-2,(int)y-22,4,22,cc); DrawTriangle((Vector2){x+2,y-22},(Vector2){x+13,y-18},(Vector2){x+2,y-14},cc); }
    for(int i=0;i<g_doorN;i++){ Door*D=&g_doors[i]; float x=D->c*TILE,y=(D->r+1)*TILE; int locked=D->locked&&KeyCount(D->key)<=0;
        Color fr=locked?(Color){180,150,60,255}:(Color){90,62,40,255};
        DrawRectangle((int)x+4,(int)y-TILE-12,TILE-8,TILE+12,fr);
        DrawRectangle((int)x+8,(int)y-TILE-8,TILE-16,TILE+8,(Color){26,20,16,255});
        if(locked) DrawText("L",(int)x+13,(int)y-TILE+2,18,(Color){235,205,70,255});
    }
    for(int i=0;i<g_npcN;i++){ Npc*n=&g_npc[i]; float x=n->c*TILE+(TILE-EW)/2,y=(n->r+1)*TILE-EH; DrawFigure(x,y,EW,EH,1,n->freed?(Color){120,200,140,255}:(Color){150,150,160,255},0,1,0); }
    for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive){ DrawRectangle((int)e->x,(int)(e->y+EH-8),(int)EW,8,(Color){90,30,30,200}); continue; }
        Color col = e->hitFlash>0?(Color){255,255,255,255}:(Color){200,70,70,255};
        DrawFigure(e->x,e->y,EW,EH,e->face,col,0,e->face,0);
        DrawRectangle((int)e->x,(int)e->y-7,(int)EW,4,(Color){40,40,40,255});
        DrawRectangle((int)e->x,(int)e->y-7,(int)(EW*e->hp/(float)E_HP),4,(Color){90,220,90,255});
    }
    for(int i=0;i<MAXSHOT;i++){ Shot*s=&g_shot[i]; if(s->age<0||s->age>0.09f) continue; Color c=s->owner?(Color){255,150,60,255}:(Color){120,230,255,255}; DrawLineEx((Vector2){s->x1,s->y1},(Vector2){s->x2,s->y2},3,c); }
    if(!P.dead){ Color pc = P.iframes>0?(Color){255,255,255,255}:(Color){90,150,235,255}; DrawFigure(P.x,P.y,PW,PH,P.face,pc,P.muzzle>0,P.muzzleDir,P.inCover); }
    else DrawRectangle((int)P.x,(int)(P.y+PH-8),(int)PW,8,(Color){120,40,40,220});
    if(g_hitboxes){ DrawRectangleLinesEx((Rectangle){P.x,P.y,PW,PH},1,GREEN); for(int i=0;i<g_enN;i++) if(g_en[i].alive) DrawRectangleLinesEx((Rectangle){g_en[i].x,g_en[i].y,EW,EH},1,RED); }
    EndMode2D();
}

static void Bar(int x,int y,int w,int h,float frac,Color fg){ DrawRectangle(x,y,w,h,(Color){30,30,36,220}); DrawRectangle(x,y,(int)(w*clampf(frac,0,1)),h,fg); DrawRectangleLines(x,y,w,h,(Color){10,10,12,255}); }

static void DrawHUD(void){
    DrawRectangle(0,0,SCREEN_W,40,(Color){0,0,0,150});
    DrawText("THORN",14,11,20,(Color){200,210,235,255});
    Bar(110,12,180,16,P.hp/(float)P_HP_MAX,(Color){210,70,70,255});
    DrawText(TextFormat("HP %d",P.hp),116,12,16,RAYWHITE);
    DrawText(TextFormat("SHELLS %d",P.ammo),310,12,18,(Color){235,225,160,255});
    DrawText(TextFormat("KEY %d  BOMB %d  SHARD %d",P.keys,P.bombs,P.shards),450,12,18,(Color){180,200,210,255});
    DrawText(TextFormat("%s / %s",g_areaName,g_roomName),SCREEN_W-380,12,18,(Color){150,160,180,255});
    DrawText(TextFormat("STATE %s",PStateName()),14,SCREEN_H-26,18,(Color){150,170,190,255});
    DrawText("A/D move  W use/climb  S down  SPACE fire  K back-fire  ` debug",360,SCREEN_H-26,16,(Color){90,100,115,255});
    if(g_won){ const char*m=g_areaClear?"AREA CLEAR":"LEVEL CLEAR"; int w=MeasureText(m,52); DrawRectangle(0,SCREEN_H/2-50,SCREEN_W,100,(Color){0,0,0,160}); DrawText(m,SCREEN_W/2-w/2,SCREEN_H/2-26,52,(Color){120,230,140,255}); }
    if(P.dead){ const char*m="YOU DIED"; int w=MeasureText(m,52); DrawText(m,SCREEN_W/2-w/2,SCREEN_H/2-26,52,(Color){220,80,80,255}); }
    if(g_paused){ const char*m="PAUSED"; int w=MeasureText(m,40); DrawText(m,SCREEN_W/2-w/2,SCREEN_H/2-20,40,RAYWHITE); }
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

// ---- Self-test: validate the Sunken Mines room graph (no window) ------------
static int RunSelfTest(void){
    const char* names[]={"entrance","shaft","cavern","depths"}; int N=4;
    int mask[8]={0}; char path[160];
    for(int i=0;i<N;i++){ snprintf(path,sizeof path,"levels/sunken_mines/%s.lvl",names[i]);
        FILE*f=fopen(path,"r"); int existed=(f!=NULL); if(f)fclose(f);
        LoadRoom(path,-1,0);
        for(int d=0;d<g_doorN;d++) mask[i]|=(1<<g_doors[d].id);
        DebugLog("selftest","\"room\":\"%s\",\"file\":%s,\"doors\":%d,\"levers\":%d,\"enemies\":%d,\"pickups\":%d,\"checkpoints\":%d",
                 names[i], existed?"true":"false", g_doorN,g_leverN,g_enN,g_pkN,g_cpN);
    }
    int errors=0;
    for(int i=0;i<N;i++){ snprintf(path,sizeof path,"levels/sunken_mines/%s.lvl",names[i]); LoadRoom(path,-1,0);
        for(int d=0;d<g_doorN;d++){ Door*D=&g_doors[d];
            if(D->target[0]==0||!strcmp(D->target,"exit")) continue;
            int t=-1; for(int k=0;k<N;k++) if(!strcmp(names[k],D->target)){ t=k; break; }
            if(t<0){ DebugLog("selftest","\"error\":\"target room not found\",\"room\":\"%s\",\"door\":%d,\"target\":\"%s\"",names[i],D->id,JStr(D->target)); errors++; }
            else if(!(mask[t]&(1<<D->targetSpawn))){ DebugLog("selftest","\"error\":\"target spawn door missing\",\"room\":\"%s\",\"door\":%d,\"target\":\"%s\",\"spawn\":%d",names[i],D->id,JStr(D->target),D->targetSpawn); errors++; }
        }
    }
    DebugLog("selftest","\"done\":true,\"rooms\":%d,\"errors\":%d",N,errors);
    fprintf(stderr,"selftest: %d rooms checked, %d errors\n",N,errors);
    return errors;
}

// ---- main -------------------------------------------------------------------
int main(int argc,char**argv){
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
    }
    if(g_debug){ g_dbg=fopen("thorn-debug.log","w"); if(!g_dbg) g_dbg=stderr; }
    for(int i=0;i<MAXSHOT;i++) g_shot[i].age=-1;

    if(g_selftest){ int e=RunSelfTest(); if(g_dbg&&g_dbg!=stderr) fclose(g_dbg); return e?1:0; }

    if(!g_headless){
        SetTraceLogLevel(LOG_WARNING);
        SetConfigFlags(FLAG_MSAA_4X_HINT);
        InitWindow(SCREEN_W,SCREEN_H,"Thorn - cinematic platformer (raylib " THORN_RAYLIB ")");
        SetTargetFPS(120);
        if(!IsWindowReady()){
            DebugLog("error","\"msg\":\"window-init-failed\"");
            fprintf(stderr,"Thorn: display unavailable (window init failed). Use './build/thorn --headless --frames N'.\n");
            if(g_dbg && g_dbg!=stderr) fclose(g_dbg);
            return 1;
        }
    }

    DebugLog("boot","\"raylib\":\"%s\",\"build\":\"0.2.0\",\"headless\":%s",THORN_RAYLIB,g_headless?"true":"false");
    DebugLog("window","\"w\":%d,\"h\":%d,\"monitor\":%d",SCREEN_W,SCREEN_H,g_headless?0:GetCurrentMonitor());

    // New game: stats, then the first room (LoadRoom logs its own "level" event).
    P.hp=P_HP_MAX; P.ammo=20; P.bombs=0; P.shards=0; P.keys=0; g_keyN=0;
    LoadRoom(g_roomStart,g_startSpawn,1);

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

    float acc=0; int shotDone=0; int running=1;
    while(running && !WindowShouldClose()){
        if(IsKeyPressed(KEY_GRAVE)||IsKeyPressed(KEY_TAB)) g_overlay=!g_overlay;
        if(IsKeyPressed(KEY_P)){ g_paused=!g_paused; DebugLog("pause","\"paused\":%s",g_paused?"true":"false"); }
        if(IsKeyPressed(KEY_G)){ g_god=!g_god; DebugLog("mode","\"god\":%s",g_god?"true":"false"); }
        if(IsKeyPressed(KEY_H)) g_hitboxes=!g_hitboxes;
        if(IsKeyPressed(KEY_N)){ g_noEnemies=!g_noEnemies; DebugLog("mode","\"noEnemies\":%s",g_noEnemies?"true":"false"); }
        if(IsKeyPressed(KEY_R)){ RespawnAtCheckpoint(); g_won=0; g_areaClear=0; }

        Input in = g_demo?DemoInput():KeyInput();

        float ft=GetFrameTime(); if(ft>0.05f) ft=0.05f;
        if(!g_paused && !g_won){
            acc+=ft; int steps=0;
            while(acc>=DT && steps<5){
                SimStep(in);
                in.up=in.down=in.fireF=in.fireB=in.use=in.cycle=0;   // consume edges after first sub-step
                acc-=DT; steps++; g_frame++;
                if((g_rate>0 && g_frame%g_rate==0) || g_rate<=0) EmitState();
                if(g_maxFrames && g_frame>=g_maxFrames) running=0;
                if(g_won) break;   // stop sub-stepping once the area is cleared
            }
        } else UpdateCam();

        BeginDrawing();
        ClearBackground((Color){20,22,30,255});
        DrawWorld(); DrawHUD(); DrawOverlay();
        EndDrawing();

        if(g_shotFrame && g_frame>=g_shotFrame && !shotDone){ TakeScreenshot("thorn-shot.png"); shotDone=1; DebugLog("shot","\"file\":\"thorn-shot.png\",\"frame\":%ld",g_frame); }
    }

    DebugLog("shutdown","\"frames\":%ld",g_frame);
    if(g_dbg && g_dbg!=stderr) fclose(g_dbg);
    CloseWindow();
    return 0;
}
