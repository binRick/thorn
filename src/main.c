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
typedef struct { float x,y,vx,vy; int face; int hp; int alive; int type; int onGround; int inCover; float coverT; float fireT; float hitFlash; const char*st; } Enemy;
typedef struct { int c,r; char kind; int alive; } Pickup;     // H K B * a u U
typedef struct { float x,y,fuse; int active; } Bomb;          // placed explosive
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
static char g_cpPath[160]=""; static float g_cpX=0,g_cpY=0; static int g_cpFace=1;   // checkpoint
static int  g_pendActive=0, g_pendSpawn=-1; static char g_pendTarget[48]="";          // deferred door
static int  g_areaClear=0, g_victory=0;
static char g_curPassword[16]="";          // password of the current room (if any)
static char g_msg[96]=""; static float g_msgT=0;   // transient on-screen banner (hints, passwords)
static Vector2 g_cam={0,0};

static int  KeyCount(const char*c){ for(int i=0;i<g_keyN;i++) if(!strcmp(g_keys[i].color,c)) return g_keys[i].count; return 0; }
static void KeyAdd(const char*c){ for(int i=0;i<g_keyN;i++) if(!strcmp(g_keys[i].color,c)){ g_keys[i].count++; return; } if(g_keyN<MAXKEY){ snprintf(g_keys[g_keyN].color,12,"%s",c); g_keys[g_keyN].count=1; g_keyN++; } }
static int  KeyTotal(void){ int n=0; for(int i=0;i<g_keyN;i++) n+=g_keys[i].count; return n; }
static void Msg(float secs,const char*fmt,...){ va_list ap; va_start(ap,fmt); vsnprintf(g_msg,sizeof g_msg,fmt,ap); va_end(ap); g_msgT=secs; }

// ---- Flags / dev ------------------------------------------------------------
static int g_noEnemies=0, g_god=0, g_demo=0, g_paused=0, g_overlay=0, g_hitboxes=0, g_selftest=0;
static int g_rate=24, g_maxFrames=0, g_shotFrame=0, g_startSpawn=-1;
static long g_frame=0; static int g_won=0;
static char g_roomStart[160]="levels/sunken_mines/entrance.lvl";
static const struct { const char*code; const char*path; } g_pwTable[] = {   // --password
    {"MINE","levels/sunken_mines/entrance.lvl"}, {"MIRE","levels/the_mire/entrance.lvl"},
    {"ASH","levels/the_ashlands/entrance.lvl"},  {"KEEP","levels/the_usurpers_keep/entrance.lvl"},
};

static char g_evlog[8][80]; static int g_evHead=0;
static void Ev(const char*fmt,...){ va_list ap; va_start(ap,fmt); vsnprintf(g_evlog[g_evHead],sizeof g_evlog[0],fmt,ap); va_end(ap); g_evHead=(g_evHead+1)%8; }

// ---- Input ------------------------------------------------------------------
typedef struct { int left,right,walk;            // held
                 int up,down,fireF,fireB,use,cycle,jump; } Input; // edge

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
    e->alive=1; e->onGround=1; e->inCover=0; e->coverT=0;
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
                case 'n': if(g_npcN<MAXNPC) g_npc[g_npcN++]=(Npc){c,r,0}; break;
                case 'H': case 'B': case '*': case 'K': case 'a': case 'u': case 'U': if(g_pkN<MAXPK) g_pk[g_pkN++]=(Pickup){c,r,t,1}; break;
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
    FILE*f=fopen(path,"r");
    if(f){ size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f); ParseRoom(buf,spawnDoor); }
    else { ParseRoom(FALLBACK_ROOM,spawnDoor); DebugLog("warn","\"msg\":\"room file missing; using fallback\",\"path\":\"%s\"",JStr(path)); }
    snprintf(g_roomPath,sizeof g_roomPath,"%s",path);
    if(setCheckpoint){ snprintf(g_cpPath,sizeof g_cpPath,"%s",g_roomPath); g_cpX=P.x; g_cpY=P.y; g_cpFace=P.face; }
    DebugLog("level","\"area\":\"%s\",\"room\":\"%s\",\"w\":%d,\"h\":%d,\"enemies\":%d,\"doors\":%d,\"pickups\":%d,\"levers\":%d",
             JStr(g_areaName),JStr(g_roomName),g_W,g_H,g_enN,g_doorN,g_pkN,g_leverN);
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
enum { SND_FIRE, SND_DRY, SND_RELOAD, SND_ENEMYFIRE, SND_HIT, SND_DEATH, SND_PICKUP, SND_LEVER, SND_BOMB, SND_UPGRADE, SND_JUMP, SND_N };
static int   g_audio=0; static Sound g_snd[SND_N];   // filled by InitAudio() (Chunk E); no-op until then
static void SndPlay(int id){ if(g_audio && id>=0 && id<SND_N) PlaySound(g_snd[id]); }

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
    if(hitIdx>=0){ Enemy*e=&g_en[hitIdx]; e->hp-=dmg; e->hitFlash=0.12f;
        if(e->hp<=0){ e->alive=0; e->st="DEAD"; SndPlay(SND_DEATH); Ev("enemy %d killed",hitIdx); DebugLog("death","\"who\":\"enemy\",\"i\":%d,\"x\":%.1f,\"y\":%.1f",hitIdx,e->x,e->y); if(e->type==3){ g_victory=1; g_won=1; DebugLog("victory",""); Ev("THE USURPER FALLS"); } }
        else { SndPlay(SND_HIT); DebugLog("hit","\"who\":\"enemy\",\"i\":%d,\"dmg\":%d,\"hp\":%d",hitIdx,dmg,e->hp); }
    }
    DebugLog("fire","\"dir\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"face\":%d,\"dmg\":%d,\"mag\":%d,\"hit\":%s,\"target\":%d",
             dir==P.face?"fwd":"back", mx,my,P.face,dmg,P.mag, hitIdx>=0?"true":"false", hitIdx);
    if(P.mag==0 && P.reserve>0){ P.reloadT=RELOAD_T; SndPlay(SND_RELOAD); DebugLog("reload","\"start\":true,\"reserve\":%d",P.reserve); }   // auto-reload on empty
    Ev("fire %s%s", dir==P.face?"fwd":"back", hitIdx>=0?" HIT":"");
}

static void HurtPlayer(int dmg,const char*cause){
    if(g_god||P.iframes>0||P.dead) return;
    P.hp-=dmg; P.iframes=IFRAMES;
    DebugLog("hit","\"who\":\"player\",\"dmg\":%d,\"hp\":%d,\"cause\":\"%s\"",dmg,P.hp,cause);
    Ev("player hit -%d (%s)",dmg,cause);
    if(P.hp<=0){ P.hp=0; P.dead=1; P.deadT=0; DebugLog("death","\"who\":\"player\",\"x\":%.1f,\"y\":%.1f,\"cause\":\"%s\"",P.x,P.y,cause); Ev("player DIED (%s)",cause); }
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
        if((ex-bx)*(ex-bx)+(ey-by)*(ey-by)<BOMB_RADIUS*BOMB_RADIUS){ e->hp-=BOMB_DMG; e->hitFlash=0.12f; hits++;
            if(e->hp<=0){ e->alive=0; e->st="DEAD"; DebugLog("death","\"who\":\"enemy\",\"i\":%d,\"cause\":\"bomb\"",i); if(e->type==3){ g_victory=1; g_won=1; DebugLog("victory",""); Ev("THE USURPER FALLS"); } } } }
    float pdx=pcx()-bx, pdy=pcy()-by; if(pdx*pdx+pdy*pdy<BOMB_RADIUS*BOMB_RADIUS) HurtPlayer(30,"bomb");
    g_boomT=0.35f; g_boomX=bx; g_boomY=by; SndPlay(SND_BOMB);
    DebugLog("bomb","\"explode\":[%.0f,%.0f],\"destroyed\":%d,\"enemiesHit\":%d",bx,by,destroyed,hits); Ev("BOOM (-%d walls)",destroyed);
}
static void PlaceBomb(void){
    if(P.bombs<=0) return;
    int slot=-1; for(int i=0;i<MAXBOMB;i++) if(!g_bomb[i].active){ slot=i; break; } if(slot<0) return;
    P.bombs--; g_bomb[slot]=(Bomb){pcx(), P.y+PH-8, BOMB_FUSE, 1};
    DebugLog("bomb","\"place\":[%.0f,%.0f],\"fuse\":%.2f",g_bomb[slot].x,g_bomb[slot].y,(double)BOMB_FUSE); Ev("bomb placed");
}
static void UpdateBombs(void){
    if(g_boomT>0) g_boomT-=DT;
    for(int i=0;i<MAXBOMB;i++) if(g_bomb[i].active){ g_bomb[i].fuse-=DT; if(g_bomb[i].fuse<=0){ g_bomb[i].active=0; ExplodeBomb(g_bomb[i].x,g_bomb[i].y); } }
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
    P.iframes=fmaxf(0,P.iframes-DT); P.fireCD=fmaxf(0,P.fireCD-DT); P.muzzle=fmaxf(0,P.muzzle-DT);
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
                else { snprintf(g_pendTarget,sizeof g_pendTarget,"%s",D->target); g_pendSpawn=D->targetSpawn; g_pendActive=1;
                       DebugLog("door","\"id\":%d,\"to\":\"%s\",\"spawn\":%d",D->id,JStr(D->target),D->targetSpawn); }
            }
        }
        if(!handled){
            int didNpc=0; for(int i=0;i<g_npcN;i++) if(!g_npc[i].freed && pcol==g_npc[i].c){ g_npc[i].freed=1; didNpc=1; SndPlay(SND_PICKUP);
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

    float target = in.walk?WALK_SPD:RUN_SPD; int d=in.right-in.left; P.turning=0;
    if(d!=0){
        if(d==P.face || fabsf(P.vx)<40){ P.face=d; P.vx+=d*ACCEL*DT; P.vx=clampf(P.vx,-target,target); }
        else { P.vx-=sgn(P.vx)*FRICTION*1.6f*DT; if(fabsf(P.vx)<40){P.vx=0;P.face=d;} P.turning=1; }
    } else { P.vx-=sgn(P.vx)*FRICTION*DT; if(fabsf(P.vx)<20)P.vx=0; }

    if(in.fireF) Fire(P.face);
    if(in.fireB) Fire(-P.face);
    if(in.use)   PlaceBomb();   // E: drop a bomb (blows cracked walls / clusters)
    if(in.jump && (P.onGround||P.onLift>=0)){ P.vy=-JUMP_V; P.onGround=0; P.onLift=-1; SndPlay(SND_JUMP); DebugLog("jump","\"x\":%.1f,\"y\":%.1f",P.x,P.y); Ev("jump"); }

    P.vy=fminf(P.vy+GRAV*DT,MAXFALL);
    PlayerMoveX(); PlayerMoveY(); ResolveLifts();

    int feetRow=(int)floorf((P.y+PH+1)/TILE)-1, ccol=(int)floorf(pcx()/TILE);
    if(P.onGround && feetRow>=0 && ccol>=0 && g_spike[feetRow][ccol]) HurtPlayer(SPIKE_DMG,"spike");
    if(P.y>g_H*TILE+80){ DebugLog("fall","\"fatal\":true"); if(g_god) RespawnAtCheckpoint(); else { P.hp=0; P.dead=1; P.deadT=0; DebugLog("death","\"who\":\"player\",\"cause\":\"pit\""); Ev("fell into the void"); } return; }

    for(int i=0;i<g_pkN;i++){ Pickup*p=&g_pk[i]; if(!p->alive) continue; if(p->c==ccol && p->r==feetRow){ p->alive=0;
        const char*nm="";
        switch(p->kind){ case 'H': P.hp=P.hp+30>P_HP_MAX?P_HP_MAX:P.hp+30; nm="health"; break;
                         case 'K': KeyAdd("gold"); P.keys=KeyTotal(); nm="gold key"; break;
                         case 'B': P.bombs++; nm="bomb"; break;
                         case '*': P.shards++; nm="shard"; break;
                         case 'a': P.reserve+=AMMO_BOX; nm="shells"; break;
                         case 'u': P.gunSpd++; nm="speed upgrade"; break;
                         case 'U': P.gunPow++; nm="power upgrade"; break; }
        SndPlay(p->kind=='u'||p->kind=='U'?SND_UPGRADE:SND_PICKUP);
        DebugLog("pickup","\"item\":\"%s\",\"x\":%d,\"y\":%d",nm,p->c*TILE,p->r*TILE); Ev("got %s",nm);
    } }
    for(int i=0;i<g_cpN;i++){ if(!g_cps[i].hit && g_cps[i].c==ccol && g_cps[i].r==feetRow){ g_cps[i].hit=1;
        snprintf(g_cpPath,sizeof g_cpPath,"%s",g_roomPath); g_cpX=P.x; g_cpY=P.y; g_cpFace=P.face;
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
            e->vx = (aggro && fabsf(dx)>TILE*0.9f) ? (dx>0?1:-1)*(e->type==3?95.0f:120.0f) : 0;
            EnemyMove(e);
            if(e->y>g_H*TILE+120){ e->alive=0; continue; }   // fell out
            ex=e->x+EW*0.5f; ey=e->y+EH*0.5f; dx=pcx()-ex;
            if(!g_noEnemies && aggro && fabsf(dx)<TILE*0.9f && fabsf(pcy()-ey)<TILE*0.9f) HurtPlayer(e->type==3?18:12,"melee");
        }

        e->st = e->inCover?"COVER" : (aggro?"AIM":"IDLE");
        if(g_noEnemies) continue;

        // Ranged fire (SKARL + SENTRY; brute is melee-only).
        if(e->type!=1 && !e->inCover && e->fireT<=0 && aggro && (dx*e->face)>0 && !P.dead){
            e->fireT = e->type==2?1.2f : e->type==3?0.9f : E_INTERVAL;
            float mx=e->face>0?e->x+EW:e->x, my=ey, endx=mx+e->face*E_RANGE;
            SpawnShot(mx,my,endx,my,1); SndPlay(SND_ENEMYFIRE);
            DebugLog("enemyfire","\"i\":%d,\"type\":%d,\"x\":%.1f,\"y\":%.1f,\"dir\":%d",i,e->type,mx,my,e->face);
            if(!P.inCover && fabsf(dx)<E_RANGE && fabsf(pcy()-my)<TILE*0.6f && LineClear(mx,pcx(),my)) HurtPlayer(e->type==3?18:E_DMG,"shot");
            if(e->type==2){ e->inCover=1; e->coverT=1.0f; DebugLog("cover","\"who\":\"enemy\",\"i\":%d,\"in\":true",i); }   // sentry ducks after firing
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
    UpdateEnemies(); UpdateShots(); UpdateBombs(); UpdateCam();
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
// Actor bitmaps: '.'=clear o=outline f=skin e=eye b/d=cloth (tinted per actor)
// m/n=gun metal k=boot. Rows are padded with clear if short.
static const char *SPR_HERO_IDLE[] = {
    "....oooo......","...obbbbo.....","...obbbbo.....","...obfffeo....",
    "...offffo.....","....obbo......","..obbbbbbo....",".obbddddbbo...",
    ".obbddddbbo...",".obbddddbbo...",".obbddddbmmmmn",".obbddddbo....",
    ".obbddddbo....",".obddddddo....",".obbkkbbo.....",".obb..bbo.....",
    ".obb..bbo.....",".odd..ddo.....",".odd..ddo.....",".okk..kko.....",
    ".okkk.kkko....","..ooo..ooo....",
};
static const char *SPR_HERO_WALK[] = {
    "....oooo......","...obbbbo.....","...obbbbo.....","...obfffeo....",
    "...offffo.....","....obbo......","..obbbbbbo....",".obbddddbbo...",
    ".obbddddbbo...",".obbddddbbo...",".obbddddbmmmmn",".obbddddbo....",
    ".obbddddbo....",".obddddddo....",".obbkkbbo.....","..obbkbbo....",
    "..obb.bbbo...","..odd..dddo..","..okk...kko..",".okk....kko..",
    ".okkk..okk...","..ooo...oo....",
};
static const char *SPR_GUARD[] = {
    "...oooooo.....","..obbbbbbo....","..obddddbo....","..obdeedbo....",
    "..obddddbo....","...obbbbo.....","..obbbbbbo....",".obddddddbo...",
    ".obddddddbmmmn",".obddddddbo...",".obddddddbo...",".obddddddbo...",
    ".obbddddbbo...",".obb..bbo.....",".obb..bbo.....",".obb..bbo.....",
    ".odd..ddo.....",".odd..ddo.....",".okk..kko.....",".okkk.kkko....",
    "..ooo..ooo....","..............",
};
#define SPR_ROWS 22

static Color SprPal(char k, Color cloth, Color clothDark){
    switch(k){
        case 'o': return (Color){18,16,24,255};
        case 'f': return (Color){214,170,138,255};
        case 'e': return (Color){95,205,235,255};
        case 'b': return cloth;
        case 'd': return clothDark;
        case 'm': return (Color){86,90,104,255};
        case 'n': return (Color){46,50,60,255};
        case 'k': return (Color){58,44,36,255};
        default:  return (Color){0,0,0,0};
    }
}
static Image ImgFromAscii(const char**rows,int h,Color cloth,Color clothDark){
    int w=(int)strlen(rows[0]); Image im=GenImageColor(w,h,(Color){0,0,0,0});
    for(int y=0;y<h;y++){ const char*R=rows[y]; int rw=(int)strlen(R);
        for(int x=0;x<w&&x<rw;x++){ Color c=SprPal(R[x],cloth,clothDark); if(c.a) ImageDrawPixel(&im,x,y,c); } }
    return im;
}
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
static void InitSprites(void){
    Color pb={92,150,235,255},pd={50,92,168,255};
    Color e0={200,72,72,255},e0d={138,40,40,255}, e1={150,58,58,255},e1d={92,30,30,255}, e2={152,84,186,255},e2d={96,48,122,255};
    Image im;
    im=ImgFromAscii(SPR_HERO_IDLE,SPR_ROWS,pb,pd); g_tHeroIdle=LoadTextureFromImage(im); UnloadImage(im);
    im=ImgFromAscii(SPR_HERO_WALK,SPR_ROWS,pb,pd); g_tHeroWalk=LoadTextureFromImage(im); UnloadImage(im);
    im=ImgFromAscii(SPR_GUARD,SPR_ROWS,e0,e0d); g_tEnemy[0]=LoadTextureFromImage(im); UnloadImage(im);
    im=ImgFromAscii(SPR_GUARD,SPR_ROWS,e1,e1d); g_tEnemy[1]=LoadTextureFromImage(im); UnloadImage(im);
    im=ImgFromAscii(SPR_GUARD,SPR_ROWS,e2,e2d); g_tEnemy[2]=LoadTextureFromImage(im); UnloadImage(im);
    Color e3={130,36,58,255},e3d={74,18,34,255};   // MALDRAK: dark crimson (drawn large)
    im=ImgFromAscii(SPR_GUARD,SPR_ROWS,e3,e3d); g_tEnemy[3]=LoadTextureFromImage(im); UnloadImage(im);
    im=ImgStone((Color){58,54,64,255},0); g_tStone=LoadTextureFromImage(im); UnloadImage(im);
    im=ImgStone((Color){78,64,52,255},1); g_tCrack=LoadTextureFromImage(im); UnloadImage(im);
    im=ImgBridge(); g_tBridge=LoadTextureFromImage(im); UnloadImage(im);
    g_sprites=1;
}
// Export the sprites to a PNG (no window needed) for visual review.
static int DumpSprites(void){
    Color pb={92,150,235,255},pd={50,92,168,255},e0={200,72,72,255},e0d={138,40,40,255},
          e1={150,58,58,255},e1d={92,30,30,255},e2={152,84,186,255},e2d={96,48,122,255};
    Image a[8]; int n=0;
    a[n++]=ImgFromAscii(SPR_HERO_IDLE,SPR_ROWS,pb,pd);
    a[n++]=ImgFromAscii(SPR_HERO_WALK,SPR_ROWS,pb,pd);
    a[n++]=ImgFromAscii(SPR_GUARD,SPR_ROWS,e0,e0d);
    a[n++]=ImgFromAscii(SPR_GUARD,SPR_ROWS,e1,e1d);
    a[n++]=ImgFromAscii(SPR_GUARD,SPR_ROWS,e2,e2d);
    a[n++]=ImgStone((Color){58,54,64,255},0);
    a[n++]=ImgStone((Color){78,64,52,255},1);
    a[n++]=ImgBridge();
    Image sheet=GenImageColor(800,140,(Color){28,28,38,255});
    int x=12, sc=4;
    for(int i=0;i<n;i++){ ImageDraw(&sheet,a[i],(Rectangle){0,0,(float)a[i].width,(float)a[i].height},(Rectangle){(float)x,18,(float)a[i].width*sc,(float)a[i].height*sc},WHITE); x+=a[i].width*sc+12; UnloadImage(a[i]); }
    ExportImage(sheet,"thorn-sprites.png"); UnloadImage(sheet);
    printf("wrote thorn-sprites.png\n"); return 0;
}
// Draw an actor texture into AABB (ax,ay,aw,ah), feet-aligned, flipped by face.
static void DrawActorTex(Texture2D t,float ax,float ay,float aw,float ah,int face,float alpha,int flash){
    float scale=ah/(float)t.height, dw=t.width*scale;
    Rectangle src={0,0,(float)(face>0?t.width:-t.width),(float)t.height};
    Rectangle dst={ax+aw*0.5f-dw*0.5f, ay+ah-(t.height*scale), dw, t.height*scale};
    DrawTexturePro(t,src,dst,(Vector2){0,0},0,(Color){255,255,255,(unsigned char)(alpha*255)});
    if(flash) DrawRectangleRec(dst,(Color){255,255,255,150});
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
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_tiles[r][c]=='#' && !g_crack[r][c]){
        if(g_sprites) DrawTexture(g_tStone,c*TILE,r*TILE,WHITE);
        else { DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){58,54,64,255}); DrawRectangle(c*TILE,r*TILE,TILE,4,(Color){92,86,104,255}); }
    }
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_crack[r][c]){   // cracked walls read as breakable
        if(g_sprites) DrawTexture(g_tCrack,c*TILE,r*TILE,WHITE);
        else { DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){78,64,52,255});
            DrawLineEx((Vector2){c*TILE+7,r*TILE+3},(Vector2){c*TILE+15,r*TILE+TILE-5},2,(Color){28,22,18,255});
            DrawLineEx((Vector2){c*TILE+22,r*TILE+5},(Vector2){c*TILE+13,r*TILE+19},2,(Color){28,22,18,255}); }
    }
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_bridge[r][c]){
        if(g_bridgeOn){ if(g_sprites) DrawTexture(g_tBridge,c*TILE,r*TILE,WHITE); else { DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){120,86,50,255}); DrawRectangle(c*TILE,r*TILE,TILE,4,(Color){160,120,70,255}); } }
        else DrawRectangleLinesEx((Rectangle){c*TILE+2,r*TILE+2,TILE-4,TILE-4},1,(Color){90,66,40,170}); // ghost
    }
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_spike[r][c])
        for(int k=0;k<4;k++) DrawTriangle((Vector2){c*TILE+k*8.0f,(r+1)*TILE},(Vector2){c*TILE+k*8.0f+4,(r+1)*TILE-14},(Vector2){c*TILE+k*8.0f+8,(r+1)*TILE},(Color){180,60,60,255});
    for(int i=0;i<g_liftN;i++){ Lift*L=&g_lifts[i]; DrawRectangle((int)L->x,(int)L->y,(int)L->w,(int)L->h,(Color){100,104,120,255}); DrawRectangle((int)L->x,(int)L->y,(int)L->w,3,(Color){150,156,175,255}); }
    for(int i=0;i<g_pkN;i++){ Pickup*p=&g_pk[i]; if(!p->alive) continue; float x=p->c*TILE+TILE*0.5f, y=(p->r+1)*TILE-16;
        if(p->kind=='H'){ DrawRectangle((int)x-3,(int)y-9,6,18,(Color){70,220,90,255}); DrawRectangle((int)x-9,(int)y-3,18,6,(Color){70,220,90,255}); }
        else if(p->kind=='K'){ DrawCircle((int)x-4,(int)y,6,(Color){235,205,70,255}); DrawRectangle((int)x,(int)y-2,12,4,(Color){235,205,70,255}); }
        else if(p->kind=='B'){ DrawCircle((int)x,(int)y,8,(Color){50,50,60,255}); DrawRectangle((int)x-1,(int)y-12,2,5,(Color){200,120,40,255}); }
        else if(p->kind=='*'){ DrawPoly((Vector2){x,y},4,9,45,(Color){90,220,235,255}); }
        else if(p->kind=='a'){ DrawRectangle((int)x-7,(int)y-5,14,10,(Color){170,135,70,255}); DrawRectangleLines((int)x-7,(int)y-5,14,10,(Color){90,70,40,255}); }
        else if(p->kind=='u'){ DrawPoly((Vector2){x,y},3,10,-90,(Color){120,170,255,255}); }   // speed upgrade
        else if(p->kind=='U'){ DrawPoly((Vector2){x,y},3,10,-90,(Color){255,120,120,255}); }   // power upgrade
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
        if(g_sprites){ float sc=e->type==3?1.7f:e->type==1?1.14f:1.0f, eh=EH*sc, ew=EW*sc; DrawActorTex(g_tEnemy[e->type],e->x+EW*0.5f-ew*0.5f,e->y+EH-eh,ew,eh,e->face,e->inCover?0.45f:1.0f,e->hitFlash>0); }
        else { Color col = e->hitFlash>0?(Color){255,255,255,255} : e->type==1?(Color){150,55,55,255} : e->type==2?(Color){150,80,185,255} : (Color){200,70,70,255}; DrawFigure(e->x,e->y,EW,EH,e->face,col,0,e->face,e->inCover); }
        if(!e->inCover){ int mh=e->type==3?300:e->type==1?110:e->type==2?45:E_HP; float bw=e->type==3?EW*1.7f:EW, bx=e->x+EW*0.5f-bw*0.5f; DrawRectangle((int)bx,(int)e->y-7,(int)bw,4,(Color){40,40,40,255}); DrawRectangle((int)bx,(int)e->y-7,(int)(bw*e->hp/(float)mh),4,e->type==3?(Color){230,90,90,255}:(Color){90,220,90,255}); }
    }
    for(int i=0;i<MAXSHOT;i++){ Shot*s=&g_shot[i]; if(s->age<0||s->age>0.09f) continue; Color c=s->owner?(Color){255,150,60,255}:(Color){120,230,255,255}; DrawLineEx((Vector2){s->x1,s->y1},(Vector2){s->x2,s->y2},3,c); }
    for(int i=0;i<MAXBOMB;i++) if(g_bomb[i].active){ Bomb*b=&g_bomb[i]; DrawCircle((int)b->x,(int)b->y-6,9,(Color){40,40,46,255}); int blink=((int)(b->fuse*8))&1; DrawCircle((int)b->x,(int)b->y-16,3,blink?(Color){255,80,60,255}:(Color){110,40,30,255}); }
    if(g_boomT>0){ float k=1.0f-(g_boomT/0.35f); float rad=BOMB_RADIUS*k; DrawCircleLines((int)g_boomX,(int)g_boomY,rad,(Color){255,180,60,(unsigned char)(220*(1-k))}); DrawCircle((int)g_boomX,(int)g_boomY,rad*0.5f,(Color){255,140,40,(unsigned char)(120*(1-k))}); }
    if(!P.dead){
        if(g_sprites){ float a=P.inCover?0.5f:(P.iframes>0?0.5f:1.0f); int moving=fabsf(P.vx)>5; Texture2D ht=(moving && ((int)(GetTime()*8)&1))?g_tHeroWalk:g_tHeroIdle; DrawActorTex(ht,P.x,P.y,PW,PH,P.face,a,0);
            if(P.muzzle>0) DrawCircle((int)(P.muzzleDir>0?P.x+PW+6:P.x-6),(int)(P.y+PH*0.42f+2),8,(Color){255,230,120,235}); }
        else { Color pc = P.iframes>0?(Color){255,255,255,255}:(Color){90,150,235,255}; DrawFigure(P.x,P.y,PW,PH,P.face,pc,P.muzzle>0,P.muzzleDir,P.inCover); }
    } else DrawRectangle((int)P.x,(int)(P.y+PH-8),(int)PW,8,(Color){120,40,40,220});
    if(g_hitboxes){ DrawRectangleLinesEx((Rectangle){P.x,P.y,PW,PH},1,GREEN); for(int i=0;i<g_enN;i++) if(g_en[i].alive) DrawRectangleLinesEx((Rectangle){g_en[i].x,g_en[i].y,EW,EH},1,RED); }
    EndMode2D();
}

static void Bar(int x,int y,int w,int h,float frac,Color fg){ DrawRectangle(x,y,w,h,(Color){30,30,36,220}); DrawRectangle(x,y,(int)(w*clampf(frac,0,1)),h,fg); DrawRectangleLines(x,y,w,h,(Color){10,10,12,255}); }

static void DrawHUD(void){
    DrawRectangle(0,0,SCREEN_W,40,(Color){0,0,0,150});
    DrawText("THORN",14,11,20,(Color){200,210,235,255});
    Bar(110,12,180,16,P.hp/(float)P_HP_MAX,(Color){210,70,70,255});
    DrawText(TextFormat("HP %d",P.hp),116,12,16,RAYWHITE);
    DrawText(TextFormat("SHELLS %d/%d%s",P.mag,P.reserve,P.reloadT>0?" RELOAD":""),300,12,18,(Color){235,225,160,255});
    DrawText(TextFormat("BOMB %d  SHARD %d  KEY %d  GUN P%d S%d",P.bombs,P.shards,P.keys,P.gunPow,P.gunSpd),470,12,18,(Color){180,200,210,255});
    DrawText(TextFormat("%s / %s",g_areaName,g_roomName),SCREEN_W-380,12,18,(Color){150,160,180,255});
    DrawText(TextFormat("STATE %s",PStateName()),14,SCREEN_H-26,18,(Color){150,170,190,255});
    DrawText("A/D move  SPACE jump  W climb/use  J fire  K back  E bomb  ` debug",360,SCREEN_H-26,16,(Color){90,100,115,255});
    { int fps=GetFPS(); const char*ft=TextFormat("%d FPS",fps); int fw=MeasureText(ft,18);
      Color fc = fps>=60?(Color){120,210,120,255} : fps>=30?(Color){220,210,120,255} : (Color){220,110,110,255};
      DrawText(ft,SCREEN_W-fw-12,SCREEN_H-26,18,fc); }
    if(g_msgT>0){ int w=MeasureText(g_msg,22); DrawRectangle(0,50,SCREEN_W,30,(Color){0,0,0,150}); DrawText(g_msg,SCREEN_W/2-w/2,55,22,(Color){235,225,160,255}); }
    if(g_won){ const char*m=g_victory?"THE USURPER FALLS":g_areaClear?"AREA CLEAR":"LEVEL CLEAR"; Color col=g_victory?(Color){235,210,90,255}:(Color){120,230,140,255}; int w=MeasureText(m,52); DrawRectangle(0,SCREEN_H/2-50,SCREEN_W,100,(Color){0,0,0,170}); DrawText(m,SCREEN_W/2-w/2,SCREEN_H/2-26,52,col); }
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
    g_snd[SND_JUMP]      = GenTone(0.14f,300,520,0,0.4f);
    g_audio=1;
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

// ---- main -------------------------------------------------------------------
int main(int argc,char**argv){
    int dump=0,docontinue=0; char pwarg[16]="";
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
        else if(!strcmp(argv[i],"--continue")) docontinue=1;
        else if(!strcmp(argv[i],"--password")&&i+1<argc) snprintf(pwarg,sizeof pwarg,"%s",argv[++i]);
    }
    if(dump) return DumpSprites();   // export thorn-sprites.png and exit (no window)
    if(pwarg[0]) for(unsigned k=0;k<sizeof(g_pwTable)/sizeof(g_pwTable[0]);k++) if(!strcmp(pwarg,g_pwTable[k].code)){ snprintf(g_roomStart,sizeof g_roomStart,"%s",g_pwTable[k].path); break; }
    if(docontinue){ FILE*sf=fopen("thorn-save.txt","r"); if(sf){ char code[32]="",path[160]=""; if(fscanf(sf,"%31s %159s",code,path)==2 && path[0]) snprintf(g_roomStart,sizeof g_roomStart,"%s",path); fclose(sf); } }
    if(g_debug){ g_dbg=fopen("thorn-debug.log","w"); if(!g_dbg) g_dbg=stderr; }
    for(int i=0;i<MAXSHOT;i++) g_shot[i].age=-1;

    if(g_selftest){ int e=RunSelfTest(); if(g_dbg&&g_dbg!=stderr) fclose(g_dbg); return e?1:0; }

    if(!g_headless){
        SetTraceLogLevel(LOG_WARNING);
        SetConfigFlags(FLAG_MSAA_4X_HINT);
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
    }

    DebugLog("boot","\"raylib\":\"%s\",\"build\":\"0.6.0\",\"headless\":%s",THORN_RAYLIB,g_headless?"true":"false");
    DebugLog("window","\"w\":%d,\"h\":%d,\"monitor\":%d",SCREEN_W,SCREEN_H,g_headless?0:GetCurrentMonitor());

    // New game: stats, then the first room (LoadRoom logs its own "level" event).
    P.hp=P_HP_MAX; P.mag=MAG_MAX; P.reserve=RESERVE_START; P.gunPow=0; P.gunSpd=0; P.reloadT=0;
    P.bombs=1; P.shards=0; P.keys=0; g_keyN=0;
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
                in.up=in.down=in.fireF=in.fireB=in.use=in.cycle=in.jump=0;   // consume edges after first sub-step
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
    if(g_audio){ for(int i=0;i<SND_N;i++) UnloadSound(g_snd[i]); CloseAudioDevice(); }
    CloseWindow();
    return 0;
}
