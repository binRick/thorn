// THORN - a cinematic-platformer tribute to Blackthorne (1994), built on raylib 6.0.
//
// Original game: faithful to the *mechanics* of the genre - weighty, no-jump
// traversal; a pump-shotgun that fires forward AND over the shoulder; ducking
// into background shadow alcoves so bullets pass. Original naming/art (no Blizzard
// assets). See DESIGN.md.
//
// Conventions shared with ../Chernobyl, ../Chernobyl2, ../uapd:
//   - run.sh builds + runs (always with --debug).
//   - --debug streams newline-delimited JSON to ./thorn-debug.log: a recurring
//     ~5 Hz "state" snapshot plus discrete events. Reconstruct any bug from it.
//   - ASCII-only DrawText (raylib's default font is ASCII).
//
// Controls: A/D or arrows move - W/Up context (climb / enter shadow / use / talk)
//           S/Down (climb down / leave shadow / duck) - Shift walk
//           Space/J fire forward - K fire BACKWARD (over shoulder)
//           E use item - Q cycle item - P pause - ` or Tab debug overlay
//           R respawn(dev) - G god(dev) - H hitboxes(dev) - Esc quit
// CLI: --debug --no-enemies --god --demo --rate N --frames N --shot N
// ----------------------------------------------------------------------------
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

// raylib 6 renamed some skeletal-anim fields; harmless guard kept for portability
// even though Thorn is 2D and doesn't use them yet.
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
// Timestamp source: real wall clock when windowed, the sim clock when headless.
static double nowT(void){ return g_headless ? g_simTime : GetTime(); }

static void DebugLog(const char *ev, const char *fmt, ...) {
    if (!g_debug || !g_dbg) return;
    fprintf(g_dbg, "{\"t\":%.3f,\"ev\":\"%s\"", nowT(), ev);
    if (fmt && fmt[0]) { va_list ap; va_start(ap, fmt); fputc(',', g_dbg); vfprintf(g_dbg, fmt, ap); va_end(ap); }
    fprintf(g_dbg, "}\n"); fflush(g_dbg);
}
// Escape a string for JSON (ASCII, bounded).
static const char * __attribute__((unused)) JStr(const char *s){  // for future dynamic-string events (file paths, ids)
    static char b[96]; size_t j=0; if(!s)s="";
    for(size_t i=0;s[i]&&j<sizeof(b)-2;i++){char c=s[i]; if(c=='"'||c=='\\'){b[j++]='\\';b[j++]=c;} else if((unsigned char)c>=0x20)b[j++]=c;}
    b[j]=0; return b;
}

// ---- Config -----------------------------------------------------------------
#define SCREEN_W 1280
#define SCREEN_H 720
#define TILE     32
#define DT       (1.0f/120.0f)     // fixed simulation step

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
#define FALL_HURT  980.0f          // landing speed above which fall damage starts

#define P_HP_MAX   100
#define P_RANGE    380.0f
#define P_DMG      34
#define PUMP       0.55f           // shotgun pump cooldown (per shot)
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

// ---- Level (ASCII; loader pads short rows with air, treats off-grid sides/top
//      as solid and below-grid as void). Legend in DESIGN.md.
//   #=solid  P=spawn  g=guard  n=NPC  H=health  K=key  B=bomb  *=shard
//   L=lever  D=door   S=shadow alcove (cover)  ^=spikes  .=air
static const char *LEVEL[] = {
    "############################################", // 0  ceiling
    "#..........................................#", // 1
    "#..........................................#", // 2
    "#..........................................#", // 3
    "#..........................................#", // 4
    "#..........................................#", // 5
    "#..........................................#", // 6
    "#..........................................#", // 7
    "#..........................................#", // 8
    "#..........................................#", // 9
    "#..........................................#", // 10
    "#..........................................#", // 11
    "#..........................................#", // 12
    "#..........................................#", // 13
    "#..........................................#", // 14
    "#..........................................#", // 15
    "#..........................................#", // 16
    "#................................g..*..D...#", // 17  on the plateau: guard, shard, door
    "#.P.S.H..n..g..^^^...L..B..K..###########..#", // 18  floor markers + raised plateau (cols 30-40)
    "##########################################.#", // 19  ground (void at col 42)
    "##########################################.#", // 20
    "##########################################.#", // 21
};
#define LEVEL_ROWS (int)(sizeof(LEVEL)/sizeof(LEVEL[0]))

static char g_tiles[64][128];      // '#' solid, '.' air
static int  g_alcove[64][128];     // shadow-cover cells
static int  g_spike[64][128];      // hazard cells
static int  g_W=0, g_H=0;

static int SolidAt(int c,int r){
    if (c<0 || c>=g_W || r<0) return 1;   // walls + ceiling
    if (r>=g_H) return 0;                 // open void below the level (fatal)
    return g_tiles[r][c]=='#';
}

// ---- Entities ---------------------------------------------------------------
typedef struct { float x,y,vx,vy; int face; int hp; int alive; float fireT; float hitFlash; const char*st; } Enemy;
typedef struct { int c,r; char kind; int alive; } Pickup;     // H K B *
typedef struct { int c,r; int freed; } Npc;
typedef struct { float x1,y1,x2,y2; float age; int owner; } Shot; // owner 0=player 1=enemy

static struct {
    float x,y,vx,vy;
    int   face;            // +1 right, -1 left
    int   onGround, inCover;
    float iframes, fireCD;
    float muzzle; int muzzleDir;      // muzzle-flash timer + its world direction
    float climbT; int climbDir;       // climb lock + direction (+1 up, -1 down)
    int   turning;
    int   hp, ammo, keys, bombs, shards;
    int   dead; float deadT;
    int   spawnC, spawnR;
} P;

static Enemy  g_en[MAXEN];   static int g_enN=0;
static Pickup g_pk[MAXPK];   static int g_pkN=0;
static Npc    g_npc[MAXNPC]; static int g_npcN=0;
static Shot   g_shot[MAXSHOT]; static int g_shotHead=0;
static int    g_doorC=-1,g_doorR=-1, g_leverC=-1,g_leverR=-1, g_leverOn=0;
static Vector2 g_cam={0,0};   // camera top-left, for the snapshot

// ---- Flags / dev ------------------------------------------------------------
static int g_noEnemies=0, g_god=0, g_demo=0, g_paused=0, g_overlay=0, g_hitboxes=0;
static int g_rate=24, g_maxFrames=0, g_shotFrame=0;
static long g_frame=0; static int g_won=0;

// Small ring of recent event strings for the on-screen overlay.
static char g_evlog[8][80]; static int g_evHead=0;
static void Ev(const char*fmt,...){ va_list ap; va_start(ap,fmt); vsnprintf(g_evlog[g_evHead],sizeof g_evlog[0],fmt,ap); va_end(ap); g_evHead=(g_evHead+1)%8; }

// ---- Input ------------------------------------------------------------------
typedef struct { int left,right,walk;          // held
                 int up,down,fireF,fireB,use,cycle; } Input; // edge (one-shot)

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
// Deterministic attract/CI input: walk right, fire on a cadence, tap up to climb.
static Input DemoInput(void){
    static double fT=0,uT=0,bT=0; double ft=GetFrameTime();
    Input in={0}; in.right=1; fT+=ft; uT+=ft; bT+=ft;
    if(fT>0.8){ in.fireF=1; fT=0; }
    if(uT>1.6){ in.up=1;    uT=0; }
    if(bT>3.7){ in.fireB=1; bT=0; }
    return in;
}
// Deterministic per-sim-frame input for --headless (CI / repro capture).
static Input DemoFrameInput(long f){
    Input in={0}; in.right=1;
    if(f%96==1)    in.fireF=1;   // ~0.8 s cadence
    if(f%190==40)  in.up=1;      // ~1.6 s : climb / cover / use
    if(f%440==220) in.fireB=1;   // ~3.7 s : over-the-shoulder shot
    return in;
}

// ---- Helpers ----------------------------------------------------------------
static inline float clampf(float v,float a,float b){ return v<a?a:(v>b?b:v); }
static inline int   sgn(float v){ return v>0?1:(v<0?-1:0); }
static inline float pcx(void){ return P.x+PW*0.5f; }
static inline float pcy(void){ return P.y+PH*0.5f; }
static inline float gunY(void){ return P.y+PH*0.42f; }   // shoulder/gun height

static int LineClear(float x1,float x2,float y){       // any solid tile between?
    int r=(int)floorf(y/TILE), a=(int)floorf(fminf(x1,x2)/TILE), b=(int)floorf(fmaxf(x1,x2)/TILE);
    for(int c=a;c<=b;c++) if(SolidAt(c,r)) return 0;
    return 1;
}

static void LoadLevel(void){
    g_H = LEVEL_ROWS; g_W = 0;
    for(int r=0;r<g_H;r++){ int len=(int)strlen(LEVEL[r]); if(len>g_W) g_W=len; }
    g_enN=g_pkN=g_npcN=0;
    for(int r=0;r<g_H;r++){
        int len=(int)strlen(LEVEL[r]);
        for(int c=0;c<g_W;c++){
            char t = c<len?LEVEL[r][c]:'.';
            g_tiles[r][c]='.'; g_alcove[r][c]=0; g_spike[r][c]=0;
            float feet=(r+1)*TILE;     // markers stand on the surface below them
            switch(t){
                case '#': g_tiles[r][c]='#'; break;
                case 'S': g_alcove[r][c]=1; break;
                case '^': g_spike[r][c]=1; break;
                case 'P': P.spawnC=c; P.spawnR=r; break;
                case 'g': if(g_enN<MAXEN){ Enemy*e=&g_en[g_enN++]; e->x=c*TILE+(TILE-EW)/2; e->y=feet-EH; e->vx=e->vy=0; e->face=-1; e->hp=E_HP; e->alive=1; e->fireT=E_INTERVAL*0.5f; e->hitFlash=0; e->st="IDLE"; } break;
                case 'n': if(g_npcN<MAXNPC){ g_npc[g_npcN++]=(Npc){c,r,0}; } break;
                case 'H': case 'K': case 'B': case '*': if(g_pkN<MAXPK){ g_pk[g_pkN++]=(Pickup){c,r,t,1}; } break;
                case 'L': g_leverC=c; g_leverR=r; break;
                case 'D': g_doorC=c; g_doorR=r; break;
                default: break;
            }
        }
    }
}

static void ResetPlayer(void){
    P.x=P.spawnC*TILE+(TILE-PW)/2; P.y=(P.spawnR+1)*TILE-PH;
    P.vx=P.vy=0; P.face=1; P.onGround=1; P.inCover=0; P.iframes=0; P.fireCD=0;
    P.muzzle=0; P.climbT=0; P.turning=0; P.dead=0; P.deadT=0;
    P.hp=P_HP_MAX; P.ammo=20; // keys/bombs/shards persist across respawns
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
static void Fire(int dir){           // dir = +1 right, -1 left (back-shot flips it)
    if(P.fireCD>0) return;
    P.fireCD=PUMP; P.muzzle=0.09f; P.muzzleDir=dir;
    float mx = dir>0 ? P.x+PW : P.x;
    float my = gunY();
    int hitIdx=-1; float best=P_RANGE;
    if(P.ammo>0){
        for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive) continue;
            float ex=e->x+EW*0.5f, ey=e->y+EH*0.5f, d=(ex-mx)*dir;
            if(d>0 && d<best && fabsf(ey-my)<TILE*0.6f && LineClear(mx,ex,my)){ best=d; hitIdx=i; } // same-tier only
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
    if(P.dead){ P.deadT+=DT; if(P.deadT>1.4f){ ResetPlayer(); DebugLog("respawn","\"x\":%.1f,\"y\":%.1f",P.x,P.y); Ev("respawn"); } return; }
    P.iframes=fmaxf(0,P.iframes-DT); P.fireCD=fmaxf(0,P.fireCD-DT); P.muzzle=fmaxf(0,P.muzzle-DT);

    // Locked actions: climbing (ignore input until the animation finishes).
    if(P.climbT>0){ P.climbT-=DT; return; }

    // Cover: standing still in shadow; leave on down/move.
    if(P.inCover){
        if(in.down||in.left||in.right){ P.inCover=0; DebugLog("cover","\"who\":\"player\",\"in\":false"); Ev("leave cover"); }
        else return;
    }

    int fr=(int)floorf((P.y+PH+1)/TILE);                          // floor tile row under feet
    int ahead = P.face>0 ? (int)floorf((P.x+PW+1)/TILE) : (int)floorf((P.x-1)/TILE);
    int pcol=(int)floorf(pcx()/TILE);

    // Context UP: door > NPC > lever > shadow > climb-up
    if(in.up && P.onGround){
        if(g_doorC>=0 && pcol==g_doorC){ g_won=1; DebugLog("door","\"id\":0,\"state\":\"exit\""); Ev("reached the door!"); }
        else { int didNpc=0; for(int i=0;i<g_npcN;i++) if(!g_npc[i].freed && pcol==g_npc[i].c){ g_npc[i].freed=1; didNpc=1; P.ammo+=6; DebugLog("npc","\"id\":%d,\"gave\":\"ammo\"",i); Ev("freed an Aurithi (+ammo)"); break; }
            if(!didNpc){
                if(g_leverC>=0 && pcol==g_leverC){ g_leverOn=!g_leverOn; DebugLog("lever","\"id\":0,\"state\":%s",g_leverOn?"true":"false"); Ev("lever %s",g_leverOn?"on":"off"); }
                else if(g_alcove[fr-1][pcol]){ P.inCover=1; P.vx=0; DebugLog("cover","\"who\":\"player\",\"in\":true"); Ev("enter cover"); }
                else if(SolidAt(ahead,fr-1)&&!SolidAt(ahead,fr-2)&&!SolidAt(ahead,fr-3)){
                    P.y=(fr-1)*TILE-PH; P.x=ahead*TILE+(TILE-PW)/2; P.vx=P.vy=0; P.climbT=0.28f; P.climbDir=1;
                    DebugLog("climb","\"dir\":\"up\",\"x\":%.1f,\"y\":%.1f",P.x,P.y); Ev("climb up"); return;
                }
            }
        }
    }
    // Context DOWN: climb down a one-tile ledge.
    if(in.down && P.onGround && !P.inCover){
        if(!SolidAt(ahead,fr-1)&&!SolidAt(ahead,fr)&&SolidAt(ahead,fr+1)){
            P.y=(fr+1)*TILE-PH; P.x=ahead*TILE+(TILE-PW)/2; P.vx=P.vy=0; P.climbT=0.24f; P.climbDir=-1;
            DebugLog("climb","\"dir\":\"down\",\"x\":%.1f,\"y\":%.1f",P.x,P.y); Ev("climb down"); return;
        }
    }

    // Horizontal movement with momentum + weighty turn.
    float target = in.walk?WALK_SPD:RUN_SPD; int d=in.right-in.left; P.turning=0;
    if(d!=0){
        if(d==P.face || fabsf(P.vx)<40){ P.face=d; P.vx+=d*ACCEL*DT; P.vx=clampf(P.vx,-target,target); }
        else { P.vx-=sgn(P.vx)*FRICTION*1.6f*DT; if(fabsf(P.vx)<40){P.vx=0;P.face=d;} P.turning=1; }
    } else { P.vx-=sgn(P.vx)*FRICTION*DT; if(fabsf(P.vx)<20)P.vx=0; }

    // Fire (forward / over-the-shoulder back).
    if(in.fireF) Fire(P.face);
    if(in.fireB) Fire(-P.face);

    // Gravity + integrate.
    P.vy=fminf(P.vy+GRAV*DT,MAXFALL);
    PlayerMoveX(); PlayerMoveY();

    // Hazards: spikes underfoot, and the kill plane (pits/void).
    if(P.onGround){ int sr=(int)floorf((P.y+PH+1)/TILE)-1; int sc=(int)floorf(pcx()/TILE); if(sr>=0&&sc>=0&&g_spike[sr][sc]) HurtPlayer(SPIKE_DMG,"spike"); }
    if(P.y>g_H*TILE+80){ DebugLog("fall","\"fatal\":true"); if(g_god){ ResetPlayer(); } else { P.hp=0; P.dead=1; P.deadT=0; DebugLog("death","\"who\":\"player\",\"cause\":\"pit\""); Ev("fell into the void"); } }

    // Auto-collect pickups on the same cell.
    int feetRow=(int)floorf((P.y+PH+1)/TILE)-1, ccol=(int)floorf(pcx()/TILE);
    for(int i=0;i<g_pkN;i++){ Pickup*p=&g_pk[i]; if(!p->alive) continue; if(p->c==ccol && p->r==feetRow){ p->alive=0;
        const char*nm="";
        switch(p->kind){ case 'H': P.hp=P.hp+30>P_HP_MAX?P_HP_MAX:P.hp+30; nm="health"; break;
                         case 'K': P.keys++; nm="key"; break;
                         case 'B': P.bombs++; nm="bomb"; break;
                         case '*': P.shards++; nm="shard"; break; }
        DebugLog("pickup","\"item\":\"%s\",\"x\":%d,\"y\":%d",nm,p->c*TILE,p->r*TILE); Ev("got %s",nm);
    } }
}

// ---- Enemy update -----------------------------------------------------------
static void UpdateEnemies(void){
    for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive) continue;
        e->hitFlash=fmaxf(0,e->hitFlash-DT);
        float ex=e->x+EW*0.5f, ey=e->y+EH*0.5f, dx=pcx()-ex;
        if(fabsf(dx)<E_AGGRO && fabsf(pcy()-ey)<TILE*1.5f){ e->face=dx>0?1:-1; e->st="AIM"; } else { e->st="IDLE"; }
        e->fireT-=DT;
        if(g_noEnemies) continue;
        int facingPlayer = (dx*e->face)>0;
        if(e->fireT<=0 && facingPlayer && !P.dead){
            e->fireT=E_INTERVAL;
            float mx=e->face>0?e->x+EW:e->x, my=ey;
            float endx=mx+e->face*E_RANGE;
            SpawnShot(mx,my,endx,my,1);
            DebugLog("enemyfire","\"i\":%d,\"x\":%.1f,\"y\":%.1f,\"dir\":%d",i,mx,my,e->face);
            int canHit = !P.inCover && fabsf(dx)<E_RANGE && fabsf(pcy()-my)<TILE*0.6f && LineClear(mx,pcx(),my); // same-tier only
            if(canHit) HurtPlayer(E_DMG,"shot");
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

// ---- Recurring state snapshot (the heartbeat of the instrumentation) --------
static void EmitState(void){
    if(!g_debug||!g_dbg) return;
    fprintf(g_dbg,"{\"t\":%.3f,\"ev\":\"state\",\"frame\":%ld,\"fps\":%d,",nowT(),g_frame,g_headless?120:GetFPS());
    fprintf(g_dbg,"\"p\":{\"x\":%.1f,\"y\":%.1f,\"vx\":%.1f,\"vy\":%.1f,\"face\":%d,\"st\":\"%s\",\"hp\":%d,\"ammo\":%d,\"ground\":%s,\"cover\":%s},",
            P.x,P.y,P.vx,P.vy,P.face,PStateName(),P.hp,P.ammo,P.onGround?"true":"false",P.inCover?"true":"false");
    fprintf(g_dbg,"\"inv\":{\"keys\":%d,\"bombs\":%d,\"shards\":%d},",P.keys,P.bombs,P.shards);
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
    DrawRectangle((int)(x+w*0.18f),(int)(y+h*0.24f),(int)(w*0.64f),(int)(h*0.56f),body);   // torso
    DrawRectangle((int)(x+w*0.22f),(int)(y+h*0.80f),(int)(w*0.22f),(int)(h*0.20f),body);   // leg
    DrawRectangle((int)(x+w*0.56f),(int)(y+h*0.80f),(int)(w*0.22f),(int)(h*0.20f),body);   // leg
    DrawCircle((int)cx,(int)(y+h*0.14f),w*0.30f,head);                                      // head
    // gun barrel in facing dir
    float by=y+h*0.42f; float bx=face>0?x+w*0.6f:x+w*0.4f; float blen=w*1.1f;
    DrawRectangle((int)(face>0?bx:bx-blen),(int)by,(int)blen,5,(Color){40,44,52,dim?120:255});
    if(firing){ float fx = dir>0? x+w : x; float fox = dir>0? fx : fx-14;
        DrawCircle((int)(dir>0?fx+8:fx-8),(int)(y+h*0.42f+2),8,(Color){255,230,120,235}); (void)fox; }
}

static void DrawWorld(void){
    Camera2D cam={ .offset={SCREEN_W*0.5f,SCREEN_H*0.5f}, .target={g_cam.x+SCREEN_W*0.5f,g_cam.y+SCREEN_H*0.5f}, .rotation=0, .zoom=1 };
    BeginMode2D(cam);
    // background plane: shadow alcoves drawn recessed/dark
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_alcove[r][c]){
        DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){14,16,26,255});
        DrawRectangleLinesEx((Rectangle){c*TILE+3,r*TILE+3,TILE-6,TILE-6},2,(Color){40,44,70,255});
    }
    // solid tiles
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_tiles[r][c]=='#'){
        DrawRectangle(c*TILE,r*TILE,TILE,TILE,(Color){58,54,64,255});
        DrawRectangle(c*TILE,r*TILE,TILE,4,(Color){92,86,104,255});           // lit top edge
    }
    // spikes
    for(int r=0;r<g_H;r++) for(int c=0;c<g_W;c++) if(g_spike[r][c])
        for(int k=0;k<4;k++) DrawTriangle((Vector2){c*TILE+k*8.0f,(r+1)*TILE},(Vector2){c*TILE+k*8.0f+4,(r+1)*TILE-14},(Vector2){c*TILE+k*8.0f+8,(r+1)*TILE},(Color){180,60,60,255});
    // pickups
    for(int i=0;i<g_pkN;i++){ Pickup*p=&g_pk[i]; if(!p->alive) continue; float x=p->c*TILE+TILE*0.5f, y=(p->r+1)*TILE-16;
        if(p->kind=='H'){ DrawRectangle((int)x-3,(int)y-9,6,18,(Color){70,220,90,255}); DrawRectangle((int)x-9,(int)y-3,18,6,(Color){70,220,90,255}); }
        else if(p->kind=='K'){ DrawCircle((int)x-4,(int)y,6,(Color){235,205,70,255}); DrawRectangle((int)x,(int)y-2,12,4,(Color){235,205,70,255}); }
        else if(p->kind=='B'){ DrawCircle((int)x,(int)y,8,(Color){50,50,60,255}); DrawRectangle((int)x-1,(int)y-12,2,5,(Color){200,120,40,255}); }
        else if(p->kind=='*'){ DrawPoly((Vector2){x,y},4,9,45,(Color){90,220,235,255}); }
    }
    // lever + door
    if(g_leverC>=0){ float x=g_leverC*TILE+TILE*0.5f,y=(g_leverR+1)*TILE; DrawRectangle((int)x-2,(int)y-10,4,10,(Color){120,120,130,255}); DrawCircle((int)(x+(g_leverOn?7:-7)),(int)y-12,5,g_leverOn?(Color){90,220,120,255}:(Color){220,90,90,255}); }
    if(g_doorC>=0){ float x=g_doorC*TILE,y=(g_doorR+1)*TILE; DrawRectangle((int)x+4,(int)y-TILE-12,TILE-8,TILE+12,(Color){76,52,34,255}); DrawRectangle((int)x+8,(int)y-TILE-8,TILE-16,TILE+8,(Color){34,24,18,255}); }
    // NPCs
    for(int i=0;i<g_npcN;i++){ Npc*n=&g_npc[i]; float x=n->c*TILE+(TILE-EW)/2,y=(n->r+1)*TILE-EH; DrawFigure(x,y,EW,EH,1,n->freed?(Color){120,200,140,255}:(Color){150,150,160,255},0,1,0); }
    // enemies
    for(int i=0;i<g_enN;i++){ Enemy*e=&g_en[i]; if(!e->alive){ DrawRectangle((int)e->x,(int)(e->y+EH-8),(int)EW,8,(Color){90,30,30,200}); continue; }
        Color col = e->hitFlash>0?(Color){255,255,255,255}:(Color){200,70,70,255};
        DrawFigure(e->x,e->y,EW,EH,e->face,col,0,e->face,0);
        DrawRectangle((int)e->x,(int)e->y-7,(int)EW,4,(Color){40,40,40,255});
        DrawRectangle((int)e->x,(int)e->y-7,(int)(EW*e->hp/(float)E_HP),4,(Color){90,220,90,255});
    }
    // shots (tracers)
    for(int i=0;i<MAXSHOT;i++){ Shot*s=&g_shot[i]; if(s->age<0||s->age>0.09f) continue; Color c=s->owner?(Color){255,150,60,255}:(Color){120,230,255,255}; DrawLineEx((Vector2){s->x1,s->y1},(Vector2){s->x2,s->y2},3,c); }
    // player
    if(!(P.dead)){ Color pc = P.iframes>0?(Color){255,255,255,255}:(Color){90,150,235,255}; int firing=P.muzzle>0; DrawFigure(P.x,P.y,PW,PH,P.face,pc,firing,P.muzzleDir,P.inCover); }
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
    DrawText(TextFormat("KEY %d   BOMB %d   SHARD %d",P.keys,P.bombs,P.shards),450,12,18,(Color){180,200,210,255});
    DrawText("Sunken Mines / mine_01",SCREEN_W-360,12,18,(Color){150,160,180,255});
    DrawText(TextFormat("STATE %s",PStateName()),14,SCREEN_H-26,18,(Color){150,170,190,255});
    DrawText("A/D move  W climb/use  S down  Shift walk  SPACE fire  K back-fire  ` debug",330,SCREEN_H-26,16,(Color){90,100,115,255});
    if(g_won){ const char*m="LEVEL CLEAR"; int w=MeasureText(m,52); DrawRectangle(0,SCREEN_H/2-50,SCREEN_W,100,(Color){0,0,0,160}); DrawText(m,SCREEN_W/2-w/2,SCREEN_H/2-26,52,(Color){120,230,140,255}); }
    if(P.dead){ const char*m="YOU DIED"; int w=MeasureText(m,52); DrawText(m,SCREEN_W/2-w/2,SCREEN_H/2-26,52,(Color){220,80,80,255}); }
    if(g_paused){ const char*m="PAUSED"; int w=MeasureText(m,40); DrawText(m,SCREEN_W/2-w/2,SCREEN_H/2-20,40,RAYWHITE); }
}

static void DrawOverlay(void){
    if(!g_overlay) return;
    int x=SCREEN_W-330,y=48,w=318;
    DrawRectangle(x,y,w,250,(Color){0,0,0,180}); DrawRectangleLines(x,y,w,250,(Color){80,90,110,255});
    DrawText("-- DEBUG (live snapshot) --",x+10,y+8,16,(Color){150,220,150,255});
    DrawText(TextFormat("frame %ld  fps %d  rate %d",g_frame,GetFPS(),g_rate),x+10,y+30,16,RAYWHITE);
    DrawText(TextFormat("pos %.0f,%.0f  vel %.0f,%.0f",P.x,P.y,P.vx,P.vy),x+10,y+50,16,RAYWHITE);
    DrawText(TextFormat("face %d  st %s",P.face,PStateName()),x+10,y+70,16,RAYWHITE);
    DrawText(TextFormat("ground %d cover %d iframe %.2f",P.onGround,P.inCover,P.iframes),x+10,y+90,16,RAYWHITE);
    DrawText(TextFormat("god %d  noEnemies %d  demo %d",g_god,g_noEnemies,g_demo),x+10,y+110,16,(Color){200,200,120,255});
    DrawText("recent events:",x+10,y+134,16,(Color){150,200,220,255});
    for(int i=0;i<8;i++){ int idx=(g_evHead+i)%8; if(g_evlog[idx][0]) DrawText(g_evlog[idx],x+10,y+154+i*12,12,(Color){170,180,190,255}); }
}

// ---- main -------------------------------------------------------------------
int main(int argc,char**argv){
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--debug")) g_debug=1;
        else if(!strcmp(argv[i],"--no-enemies")) g_noEnemies=1;
        else if(!strcmp(argv[i],"--god")) g_god=1;
        else if(!strcmp(argv[i],"--demo")) g_demo=1;
        else if(!strcmp(argv[i],"--rate")&&i+1<argc) g_rate=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--frames")&&i+1<argc) g_maxFrames=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--shot")&&i+1<argc) g_shotFrame=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--headless")){ g_headless=1; g_debug=1; }
    }
    if(g_debug){ g_dbg=fopen("thorn-debug.log","w"); if(!g_dbg) g_dbg=stderr; }
    for(int i=0;i<MAXSHOT;i++) g_shot[i].age=-1;

    if(!g_headless){
        SetTraceLogLevel(LOG_WARNING);
        SetConfigFlags(FLAG_MSAA_4X_HINT);
        InitWindow(SCREEN_W,SCREEN_H,"Thorn - cinematic platformer (raylib " THORN_RAYLIB ")");
        SetTargetFPS(120);
        // No display (SSH / CI / sandbox)? Bail cleanly instead of crashing on a
        // dead GL context. --headless runs the sim with no window at all.
        if(!IsWindowReady()){
            DebugLog("error","\"msg\":\"window-init-failed\"");
            fprintf(stderr,"Thorn: display unavailable (window init failed). Use './build/thorn --headless --frames N' for a no-window capture.\n");
            if(g_dbg && g_dbg!=stderr) fclose(g_dbg);
            return 1;
        }
    }

    DebugLog("boot","\"raylib\":\"%s\",\"build\":\"0.1.0\",\"headless\":%s",THORN_RAYLIB,g_headless?"true":"false");
    DebugLog("window","\"w\":%d,\"h\":%d,\"monitor\":%d",SCREEN_W,SCREEN_H,g_headless?0:GetCurrentMonitor());
    LoadLevel(); ResetPlayer();
    DebugLog("level","\"area\":\"Sunken Mines\",\"room\":\"mine_01\",\"w\":%d,\"h\":%d,\"enemies\":%d,\"pickups\":%d",
             g_W,g_H,g_enN,g_pkN);

    // Headless: pure fixed-timestep sim, no window or rendering. Drives the
    // deterministic demo input so --frames produces a complete capture for CI/repro.
    if(g_headless){
        int running=1;
        while(running){
            Input in=DemoFrameInput(g_frame);
            UpdatePlayer(in); UpdateEnemies(); UpdateShots(); UpdateCam();
            g_simTime+=DT; g_frame++;
            if(g_rate<=0 || g_frame%g_rate==0) EmitState();
            if(g_won){ DebugLog("win","\"frame\":%ld",g_frame); g_won=0; }   // log clear, keep capturing
            if(g_maxFrames && g_frame>=g_maxFrames) running=0;
        }
        DebugLog("shutdown","\"frames\":%ld",g_frame);
        if(g_dbg && g_dbg!=stderr) fclose(g_dbg);
        return 0;
    }

    float acc=0; int shotDone=0; int running=1;
    while(running && !WindowShouldClose()){
        // per-frame toggles (work regardless of pause)
        if(IsKeyPressed(KEY_GRAVE)||IsKeyPressed(KEY_TAB)) g_overlay=!g_overlay;
        if(IsKeyPressed(KEY_P)){ g_paused=!g_paused; DebugLog("pause","\"paused\":%s",g_paused?"true":"false"); }
        if(IsKeyPressed(KEY_G)){ g_god=!g_god; DebugLog("mode","\"god\":%s",g_god?"true":"false"); }
        if(IsKeyPressed(KEY_H)){ g_hitboxes=!g_hitboxes; }
        if(IsKeyPressed(KEY_N)){ g_noEnemies=!g_noEnemies; DebugLog("mode","\"noEnemies\":%s",g_noEnemies?"true":"false"); }
        if(IsKeyPressed(KEY_R)){ ResetPlayer(); g_won=0; DebugLog("respawn","\"x\":%.1f,\"y\":%.1f",P.x,P.y); Ev("manual respawn"); }

        Input in = g_demo?DemoInput():KeyInput();

        float ft=GetFrameTime(); if(ft>0.05f) ft=0.05f;
        if(!g_paused && !g_won){
            acc+=ft; int steps=0;
            while(acc>=DT && steps<5){
                UpdatePlayer(in); UpdateEnemies(); UpdateShots(); UpdateCam();
                in.up=in.down=in.fireF=in.fireB=in.use=in.cycle=0;   // consume edges after first sub-step
                acc-=DT; steps++; g_frame++;
                if(g_rate>0 && g_frame%g_rate==0) EmitState();
                else if(g_rate==0) EmitState();
                if(g_maxFrames && g_frame>=g_maxFrames) running=0;
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
