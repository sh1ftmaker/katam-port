/* GOALS: local achievements and the run timer (docs/features/goals.md).
 *
 * Presentation only.  Once per simulated frame, at the blit (present.c), it
 * reads the frame just shown -- the save's treasures, the map's rooms, the
 * Kirbys, the boss objects -- and keeps its own record on this device (the
 * browser's localStorage "katam.goals", natively goals.txt beside
 * settings.ini).  Nothing here is written to game memory, recorded, sent or
 * hashed, so online play is unaffected: each device keeps its own.  While
 * this device's online pause overlay is up (a private fork of the game is
 * in memory) the game is not read.
 *
 * Drawn over the presented picture with the GPU renderer's overlays (the
 * party HUD's way): a toast as a goal unlocks, and the timer when it is on.
 * The web page's 2D-canvas fallback draws neither (the record still keeps).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "task.h"
#include "functions.h"
#include "treasures.h"
#include "pause_area_map.h"
#include "constants/kirby.h"
#include "constants/object_types.h"
#include "port/port.h"
#include "port/localmp.h"
#include "port/lmpnet.h"
#include "port/menu_poc.h"
#include "render/gl_renderer.h"
#include "ui/goals.h"
#include "ui/goals_core.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#ifdef __cplusplus
extern "C" {
#endif

void ObjectMain(void);

#include "ui/goals_rooms.inc"

static const char *const kAreaName[10] = {
    "", "RAINBOW ROUTE", "MOONLIGHT MANSION", "CABBAGE CAVERN", "MUSTARD MOUNTAIN",
    "CARROT CASTLE", "OLIVE OCEAN", "PEPPERMINT PALACE", "RADISH RUINS", "CANDY CONSTELLATION",
};

static int sOn;                 /* PortGoalsInit */
static char sPath[1024];        /* native: the file; "" the browser's storage */
static GoalsSave sSave;
static GoalsRun sRun;
static int sDirty;
static double sLastNow, sLastSave;

/* the toasts waiting, the one up */
#define TOASTS 8
static u8 sToastQ[TOASTS];
static int sToastN;
static int sToastGoal = -1;
static double sToastAt;
#define TOAST_MS 3500.0

/* --- storage --------------------------------------------------------------- */

#ifdef __EMSCRIPTEN__
EM_JS(void, GoalsWebLoad, (char *buf, int size), {
    var t = '';
    try { if (typeof localStorage !== 'undefined') t = localStorage.getItem('katam.goals') || ''; }
    catch (e) { t = ''; }
    stringToUTF8(t, buf, size);
});
EM_JS(void, GoalsWebStore, (const char *text), {
    try { if (typeof localStorage !== 'undefined') localStorage.setItem('katam.goals', UTF8ToString(text)); }
    catch (e) {}
});
EM_JS(void, GoalsWebKeys, (void), {
    if (typeof window === 'undefined' || Module.goalsKeys) return;
    Module.goalsKeys = 1;
    window.addEventListener('keydown', function (e) {
        if (e.code === 'F9' && !e.repeat) {
            try { Module._PortGoalsTimerReset(); } catch (err) {}
            e.preventDefault();
        }
    });
});
#endif

static void Load(void)
{
    char text[1024];

    text[0] = '\0';
#ifdef __EMSCRIPTEN__
    GoalsWebLoad(text, (int)sizeof text);
#else
    {
        FILE *f = sPath[0] ? fopen(sPath, "rb") : NULL;

        if (f) {
            size_t n = fread(text, 1, sizeof text - 1, f);

            text[n] = '\0';
            fclose(f);
        }
    }
#endif
    GoalsSaveDefaults(&sSave);
    GoalsSaveParse(&sSave, text);
}

static void Store(void)
{
    char text[1024];

    if (!sOn)
        return;
    GoalsSaveFormat(&sSave, text, (int)sizeof text);
#ifdef __EMSCRIPTEN__
    GoalsWebStore(text);
#else
    {
        FILE *f = sPath[0] ? fopen(sPath, "wb") : NULL;

        if (f) {
            fputs(text, f);
            fclose(f);
        }
    }
#endif
    sDirty = 0;
}

void PortGoalsInit(const char *path)
{
    snprintf(sPath, sizeof sPath, "%s", path ? path : "");
    Load();
    GoalsRunReset(&sRun);
    sOn = 1;
    {
        /* tests (host_sdl.c KATAM_GOALS_TEST): "timer" turns the timer on,
         * "toast" shows one */
        const char *t = getenv("KATAM_GOALS_TEST");

        if (t && strstr(t, "timer"))
            sSave.timerOn = 1;
        if (t && strstr(t, "toast"))
            sToastQ[sToastN++] = GOAL_MEGA_TITAN;
    }
#ifdef __EMSCRIPTEN__
    GoalsWebKeys();
#endif
}

int PortGoalsActive(void) { return sOn; }

EMSCRIPTEN_KEEPALIVE void PortGoalsTimerReset(void)
{
    GoalsRunReset(&sRun);
}

static void Unlock(int g)
{
    if (!GoalsUnlock(&sSave, g))
        return;
    sDirty = 1;
    PortLog("[goals] unlocked %s", gGoalInfo[g].name);
    if (sToastN < TOASTS)
        sToastQ[sToastN++] = (u8)g;
}

/* --- reading the game ------------------------------------------------------ */

/* the bosses: object types -> goal (Mega Titan: its head; the hands: Crazy
 * Hand of the pair) */
static int BossGoal(u8 type)
{
    switch (type) {
    case OBJ_KRACKO:          return GOAL_KRACKO;
    case OBJ_KING_GOLEM:      return GOAL_KING_GOLEM;
    case OBJ_MOLEY:           return GOAL_MOLEY;
    case OBJ_TITAN_HEAD:      return GOAL_MEGA_TITAN;
    case OBJ_GOBBLER:         return GOAL_GOBBLER;
    case OBJ_WIZ:             return GOAL_WIZ;
    case OBJ_CRAZY_HAND_1:    return GOAL_HANDS;
    case OBJ_DARK_META_KNIGHT: case OBJ_DARK_META_KNIGHT_W8: return GOAL_DARK_META_KNIGHT;
    case OBJ_DARK_MIND_FORM_2: return GOAL_DARK_MIND;
    default:                  return -1;
    }
}
static int IsMidboss(u8 type) { return type >= OBJ_MR_FROSTY && type <= OBJ_BOMBAR; }

/* the fights in the room this device's Kirby is in: per object type, seen
 * with health left */
static u8 sSeenAlive[0x60];
static u32 sMissTick;           /* sTick at the last miss */
static u16 sFightRoom = 0xFFFF;
static int sMissInRoom;         /* a miss since entering this room */
static u32 sStreak;
static s8 sLastHp = 1;
static int sLastArea;
static u16 sAreasSeen;          /* this session: areas the Kirby was in */
static u8 sLastShards = 0xFF;
static u32 sTick;

static const struct Object *TaskObject(const struct Task *t)
{
    if (!t->main || !t->structOffset)
        return NULL;
    return (const struct Object *)((t->flags & TASK_USE_EWRAM)
            ? (uintptr_t)EWRAM_START + ((uintptr_t)t->structOffset << 2)
            : (uintptr_t)IWRAM_START + t->structOffset);
}

static void Beaten(u8 type)
{
    int g = BossGoal(type);

    if (IsMidboss(type)) {
        sSave.midbosses |= 1u << (type - OBJ_MR_FROSTY);
        sDirty = 1;
        Unlock(GOAL_MIDBOSS);
        if ((sSave.midbosses & 0xFF) == 0xFF)
            Unlock(GOAL_ALL_MIDBOSSES);
    }
    if (g >= 0) {
        sSave.bosses |= 1u << g;
        sDirty = 1;
        Unlock(g);
        if (!sMissInRoom)
            Unlock(GOAL_BOSS_NO_MISS);
        if (g == GOAL_DARK_MIND && sSave.timerOn)
            GoalsRunSplit(&sRun, &sSave, GOALS_SPLIT_END) ? (void)(sDirty = 1) : (void)0;
    }
    if (gPortLnOn)
        Unlock(GOAL_ONLINE_BOSS);
}

static void SampleFights(u16 room, int hp)
{
    u8 alive[0x60];
    int i;

    if (room != sFightRoom) {
        memset(sSeenAlive, 0, sizeof sSeenAlive);
        sFightRoom = room;
    }
    memset(alive, 0, sizeof alive);
    for (i = 0; i < MAX_TASK_NUM; i++) {
        const struct Object *o = TaskObject(&gTasks[i]);
        u8 t;

        if (o == NULL || gTasks[i].main != FN(ObjectMain) || o->base.header.kind != 1)
            continue;
        t = o->type;
        if (t >= sizeof alive || (!IsMidboss(t) && BossGoal(t) < 0) || o->base.roomId != room)
            continue;
        if (o->unk80 > 0) {
            alive[t] = 1;
            sSeenAlive[t] = 1;
        }
    }
    /* seen with health, now none (spent, or gone) while the room stays --
     * and not as this Kirby's miss reloads the room (the boss goes then) */
    for (i = 0; i < (int)sizeof alive; i++)
        if (sSeenAlive[i] && !alive[i]) {
            sSeenAlive[i] = 0;
            if (hp <= 0 || sTick - sMissTick < 300)
                continue;
            Beaten((u8)i);
        }
}

static int AreaOf(u16 room)
{
    int a, i;

    for (a = 1; a <= 9; a++)
        for (i = kAreaRoomsAt[a - 1]; i < kAreaRoomsAt[a]; i++)
            if (kAreaRooms[i] == room)
                return a;
    return 0;
}

static void SampleMap(void)
{
    int a, i, done = 0, all = 0, maps = 0;

    for (a = 1; a <= 9; a++) {
        int n = 0, d = 0;

        for (i = kAreaRoomsAt[a - 1]; i < kAreaRoomsAt[a]; i++) {
            u16 r = kAreaRooms[i];

            n++;
            if (sub_08002A5C(r) && sub_08002AAC(r) == sub_08002AD0(r))
                d++;
        }
        if (n && d == n)
            Unlock(GOAL_AREA_DONE);
        done += d;
        all += n;
        if (HasBigChest((u8)a))
            maps++;
    }
    if (all && done * 2 >= all)
        Unlock(GOAL_MAP50);
    if (all && done == all)
        Unlock(GOAL_MAP100);
    if (maps == 9)
        Unlock(GOAL_ALL_MAPS);
}

static int AreaOfShard(int bit)
{
    int a;

    for (a = 1; a <= 9; a++)
        if (gAreaMapRoomIdsAndShards[a][2] == bit)
            return a;
    return 0;
}

static void SampleShards(int area)
{
    u32 field = gTreasures.shardField & 0xFF;
    u16 n = NumShardsCollected();
    int b;

    if (sLastShards != 0xFF && n > sLastShards) {
        if (gPortLnOn)
            Unlock(GOAL_ONLINE_SHARD);
        if (sSave.timerOn)
            for (b = 0; b < 8; b++)
                if ((field >> b) & 1) {
                    int a = AreaOfShard(b);

                    if (a <= 0)
                        a = area;
                    if (a > 0 && GoalsRunSplit(&sRun, &sSave, a))
                        sDirty = 1;
                }
    }
    sLastShards = (u8)n;
    if (n >= 1) Unlock(GOAL_SHARD1);
    if (n >= 4) Unlock(GOAL_SHARD4);
    if (n >= 8) Unlock(GOAL_SHARD8);
}

static int LocalSlot(void)
{
    int s = gLocalPlayerId;

    if (PortLnActive()) {
        u32 m = (u32)PortLnSelfSlots();

        for (s = 0; s < 32 && m && !((m >> s) & 1); s++)
            ;
        if (!m)
            s = 0;
    }
    return s < gNumKirbys ? s : 0;
}

/* a frame in play: the game's HUD up, not the attract demo, a room */
static int InPlay(int s)
{
    u16 room;

    if (gGameplayHudTask == NULL || (gUnk_0203AD10 & 0x10))
        return 0;
    room = gCurLevelInfo[s].currentRoom;
    return room != 0xFFFF && room < 1000;
}

static void SampleOnline(void)
{
    u32 m = PortLmpSlots() & ~1u, n = 1;

    if (!gPortLnOn)
        return;
    Unlock(GOAL_ONLINE);
    for (; m; m >>= 1)
        n += m & 1;
    if (n >= 4) Unlock(GOAL_ONLINE4);
    if (n >= 8) Unlock(GOAL_ONLINE8);
}

void PortGoalsSample(double nowMs)
{
    double dt = sLastNow > 0.0 ? nowMs - sLastNow : 0.0;
    int s, play;

    sLastNow = nowMs;
    if (!sOn)
        return;
    /* this device's online pause overlay: the game in memory may be its
     * private fork -- the clock runs, nothing is read */
    if (gPortLnOn && (PortLnOverlayOpen() || PortLnOverlayRunning() || PortLnShadowRunning()
                      || PortLnCatchingUp())) {
        if (sSave.timerOn)
            GoalsRunTick(&sRun, dt, sRun.started);
        return;
    }
    s = LocalSlot();
    play = InPlay(s);
    if (sSave.timerOn)
        GoalsRunTick(&sRun, dt, play);
    if (!play) {
        sLastShards = 0xFF;     /* a save loaded is not a shard found */
        sFightRoom = 0xFFFF;
        goto out;
    }
    sTick++;
    {
        const struct Kirby *k = &gKirbys[s];
        u16 room = gCurLevelInfo[s].currentRoom;
        int area = AreaOf(room);
        u8 ab = k->ability;

        /* a miss: the Kirby's health ran out */
        if (k->hp <= 0 && sLastHp > 0) {
            sMissInRoom = 1;
            sStreak = 0;
            sMissTick = sTick;
        }
        sLastHp = k->hp;
        if (room != sFightRoom && sFightRoom != 0xFFFF) {
            /* left a room: one more in a row if there was no miss in it */
            if (!sMissInRoom && k->hp > 0) {
                sStreak++;
                if (sStreak > sSave.bestStreak) {
                    sSave.bestStreak = sStreak;
                    sDirty = 1;
                }
            }
            sMissInRoom = 0;
        }
        if (sSave.bestStreak >= 10) Unlock(GOAL_STREAK10);
        if (sSave.bestStreak >= 50) Unlock(GOAL_STREAK50);
        SampleFights(room, k->hp);

        if (ab > KIRBY_ABILITY_NORMAL && ab <= KIRBY_ABILITY_MASTER && !((sSave.abilities >> ab) & 1)) {
            int n = 0, i;

            sSave.abilities |= 1u << ab;
            sDirty = 1;
            for (i = 1; i <= GOALS_ABILITIES; i++)
                n += (int)((sSave.abilities >> i) & 1);
            Unlock(GOAL_COPY1);
            if (n >= 10) Unlock(GOAL_COPY10);
            if (n >= GOALS_ABILITIES) Unlock(GOAL_COPY_ALL);
            if (ab == KIRBY_ABILITY_MASTER) Unlock(GOAL_MASTER);
        }
        if (area > 0) {
            sAreasSeen |= (u16)(1u << area);
            if (sAreasSeen == 0x3FE)
                Unlock(GOAL_ALL_AREAS);
            sLastArea = area;
        }
        SampleShards(area ? area : sLastArea);
        if ((sTick & 31) == 0) {
            SampleMap();
            if (NumChestsCollected() > 0) Unlock(GOAL_CHEST);
            if (NumVitalitiesCollected() >= 4) Unlock(GOAL_VITALITY);
            SampleOnline();
        }
        sSave.playFrames++;
        if (sSave.playFrames >= 60u * 60u * 60u)
            Unlock(GOAL_HOUR);
    }
out:
    if (sToastGoal < 0 && sToastN > 0) {
        sToastGoal = sToastQ[0];
        memmove(sToastQ, sToastQ + 1, (size_t)(--sToastN));
        sToastAt = nowMs;
    }
    if (sToastGoal >= 0 && nowMs - sToastAt > TOAST_MS)
        sToastGoal = -1;
    /* the record: as it changes, and the play time once a minute */
    if (sDirty && nowMs - sLastSave > 2000.0) {
        Store();
        sLastSave = nowMs;
    } else if (nowMs - sLastSave > 60000.0) {
        Store();
        sLastSave = nowMs;
    }
}

/* --- drawing --------------------------------------------------------------- */

/* 3x5 letters: 15 cells, rows top to bottom, '#' ink */
static const struct { char ch; const char *g; } kFont[] = {
    {'A', ".#.#.#####.##.#"}, {'B', "##.#.###.#.###."}, {'C', ".###..#..#...##"},
    {'D', "##.#.##.##.###."}, {'E', "####..##.#..###"}, {'F', "####..##.#..#.."},
    {'G', ".###..#.##.#.##"}, {'H', "#.##.#####.##.#"}, {'I', "###.#..#..#.###"},
    {'J', "..#..#..##.#.#."}, {'K', "#.##.###.#.##.#"}, {'L', "#..#..#..#..###"},
    {'M', "#.########.##.#"}, {'N', "##.#.##.##.##.#"}, {'O', ".#.#.##.##.#.#."},
    {'P', "##.#.###.#..#.."}, {'Q', ".#.#.##.###..##"}, {'R', "##.#.###.#.##.#"},
    {'S', ".###...#...###."}, {'T', "###.#..#..#..#."}, {'U', "#.##.##.##.####"},
    {'V', "#.##.##.##.#.#."}, {'W', "#.##.########.#"}, {'X', "#.##.#.#.#.##.#"},
    {'Y', "#.##.#.#..#..#."}, {'Z', "###..#.#.#..###"}, {'0', "####.##.##.####"},
    {'1', ".#.##..#..#.###"}, {'2', "##...#.#.#..###"}, {'3', "##...#.#...###."},
    {'4', "#.##.####..#..#"}, {'5', "####..##...###."}, {'6', ".###..####.####"},
    {'7', "###..#.#..#..#."}, {'8', "####.#####.####"}, {'9', "####.####..###."},
    {':', "....#.....#...."}, {'.', ".............#."}, {'-', "......###......"},
    {'+', "....#.###.#...."}, {'/', "..#..#.#.#..#.."}, {'%', "#.#..#.#.#..#.#"},
    {'!', ".#..#..#.....#."},
};

static const char *Glyph(char c)
{
    int i;

    for (i = 0; i < (int)(sizeof kFont / sizeof kFont[0]); i++)
        if (kFont[i].ch == c)
            return kFont[i].g;
    return NULL;
}

#define CW 160
#define CH 32
typedef struct { u32 px[CW * CH]; int w, h; } Canvas;
static Canvas sToastCv, sTimerCv;

static void Fill(Canvas *cv, int x, int y, int w, int h, u32 c)
{
    int i, j;

    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < cv->w && j < cv->h)
                cv->px[j * cv->w + i] = c;
}

static int TextW(const char *s) { return (int)strlen(s) * 4 - 1; }

static void Text(Canvas *cv, int x, int y, const char *s, u32 ink)
{
    for (; *s; s++, x += 4) {
        const char *g = Glyph(*s);
        int r, c;

        if (!g)
            continue;
        for (r = 0; r < 5; r++)
            for (c = 0; c < 3; c++)
                if (g[r * 3 + c] == '#')
                    Fill(cv, x + c, y + r, 1, 1, ink);
    }
}

#define INK    0xFFFFFFFFu
#define GOLD   0xFF40D8FFu      /* premultiplied RGBA, R low */
#define GREEN  0xFF60F060u
#define RED    0xFF5050F0u
#define BACK   0xC0180C10u

static void Begin(Canvas *cv, int w, int h)
{
    cv->w = w < CW ? w : CW;
    cv->h = h < CH ? h : CH;
    memset(cv->px, 0, sizeof cv->px);
    Fill(cv, 0, 0, cv->w, cv->h, BACK);
}

void PortGoalsDraw(int fbW, int fbH, const int pic[4], double scale, int smooth)
{
    double k = scale >= 1.0 ? (double)(int)scale : 1.0;

    (void)smooth;
    if (!sOn || pic[2] <= 0 || pic[3] <= 0)
        return;
    if (sToastGoal >= 0) {
        const char *n = gGoalInfo[sToastGoal].name;
        int w = TextW(n), tw = TextW("ACHIEVEMENT"), cw = (w > tw ? w : tw) + 8;

        Begin(&sToastCv, cw, 16);
        Text(&sToastCv, (cw - tw) / 2, 2, "ACHIEVEMENT", GOLD);
        Text(&sToastCv, (cw - w) / 2, 9, n, INK);
        GlrDrawOverlay(GLR_OVERLAY_GOALS, sToastCv.px, sToastCv.w, sToastCv.h, 1, fbW, fbH,
                       pic[0] + (pic[2] - sToastCv.w * k) / 2, pic[1] + 4 * k, k, 0);
    }
    if (sSave.timerOn) {
        char rta[24], igt[28], t[24], line[48];
        int rows = 2, w;

        GoalsFormatMs(sRun.rtaMs, rta, sizeof rta);
        GoalsFormatMs(GoalsFramesMs(sRun.igt), t, sizeof t);
        snprintf(igt, sizeof igt, "IGT %s", t);
        line[0] = '\0';
        if (sSave.splitsOn && sRun.lastSplit >= 0) {
            int s = sRun.lastSplit;
            const char *nm = s == GOALS_SPLIT_END ? "END" : kAreaName[s];
            double d = 0.0;
            /* against the best before this run's: the best now is this one
             * if it beat it; shown as the time itself then */
            if (sSave.bestRta[s] && sSave.bestRta[s] < sRun.splitRta[s])
                d = (double)sRun.splitRta[s] - sSave.bestRta[s];
            GoalsFormatDelta(d, t, sizeof t);
            snprintf(line, sizeof line, "%s %s", nm, d > 0.0 ? t : "BEST");
            rows = 3;
        }
        w = TextW(rta);
        if (TextW(igt) > w) w = TextW(igt);
        if (TextW(line) > w) w = TextW(line);
        Begin(&sTimerCv, w + 6, rows * 7 + 2);
        Text(&sTimerCv, 3, 2, rta, sRun.finished ? GOLD : INK);
        Text(&sTimerCv, 3, 9, igt, 0xFFC0C0C0u);
        if (rows == 3)
            Text(&sTimerCv, 3, 16, line, strstr(line, "BEST") ? GOLD
                 : (sRun.lastSplit >= 0 ? RED : GREEN));
        GlrDrawOverlay(GLR_OVERLAY_GOALS + 1, sTimerCv.px, sTimerCv.w, sTimerCv.h, 1, fbW, fbH,
                       /* top right, under the native menu button; clear
                        * of the game's HUD along the bottom */
                       pic[0] + pic[2] - (sTimerCv.w + 2) * k, pic[1] + 16 * k, k, 0);
    }
}

/* --- SETTINGS > GOALS (port/menu_poc.h struct PortPocRows) ------------------ */

enum { ROW_TIMER, ROW_SPLITS, ROW_RESET, ROW_CLEARBEST, ROW_COUNT, ROW_FIRST_GOAL };

static const char *RowName(int row)
{
    switch (row) {
    case ROW_TIMER:     return "RUN TIMER";
    case ROW_SPLITS:    return "SPLITS";
    case ROW_RESET:     return "RESET TIMER";
    case ROW_CLEARBEST: return "CLEAR BEST SPLITS";
    case ROW_COUNT:     return "ACHIEVEMENTS";
    default:
        row -= ROW_FIRST_GOAL;
        return row >= 0 && row < GOALS_N ? gGoalInfo[row].name : "";
    }
}

static const char *RowDesc(int row)
{
    static char d[32];

    switch (row) {
    case ROW_TIMER:     return "REAL AND IN-GAME TIME";
    case ROW_SPLITS:    return "A SPLIT AT EACH SHARD";
    case ROW_RESET:     return "A OR F9 - STARTS IN PLAY";
    case ROW_CLEARBEST: return "FORGET THE BEST SPLITS";
    case ROW_COUNT:     return "KEPT ON THIS DEVICE";
    default:
        row -= ROW_FIRST_GOAL;
        if (row < 0 || row >= GOALS_N)
            return "";
        snprintf(d, sizeof d, "%s", gGoalInfo[row].desc);
        return d;
    }
}

static void RowValue(int row, char *buf, int size)
{
    switch (row) {
    case ROW_TIMER:     snprintf(buf, size, "%s", sSave.timerOn ? "ON" : "OFF"); return;
    case ROW_SPLITS:    snprintf(buf, size, "%s", sSave.splitsOn ? "ON" : "OFF"); return;
    case ROW_RESET:
        if (sRun.started)
            GoalsFormatMs(sRun.rtaMs, buf, size);
        else
            snprintf(buf, size, "-");
        return;
    case ROW_CLEARBEST: buf[0] = '\0'; return;
    case ROW_COUNT:     snprintf(buf, size, "%d/%d", GoalsCount(&sSave), GOALS_N); return;
    default:
        row -= ROW_FIRST_GOAL;
        snprintf(buf, size, "%s", row >= 0 && row < GOALS_N && ((sSave.unlocked >> row) & 1)
                                      ? "DONE" : "-");
    }
}

static void RowStep(int row, int dir)
{
    switch (row) {
    case ROW_TIMER:
        sSave.timerOn = !sSave.timerOn;
        if (sSave.timerOn)
            GoalsRunReset(&sRun);
        break;
    case ROW_SPLITS:    sSave.splitsOn = !sSave.splitsOn; break;
    case ROW_RESET:     if (dir > 0) GoalsRunReset(&sRun); return;
    case ROW_CLEARBEST:
        if (dir > 0) {
            memset(sSave.bestRta, 0, sizeof sSave.bestRta);
            memset(sSave.bestIgt, 0, sizeof sSave.bestIgt);
        }
        break;
    default:
        return;
    }
    Store();
}

static int RowAvailable(int row)
{
    if (row == ROW_RESET)
        return sSave.timerOn;
    if (row >= ROW_FIRST_GOAL)      /* lit once done */
        return (int)((sSave.unlocked >> (row - ROW_FIRST_GOAL)) & 1);
    return row != ROW_COUNT;
}

extern const struct PortPocRows gPocGoalsRows;
const struct PortPocRows gPocGoalsRows = {
    "GOALS", RowName, RowDesc, RowValue, RowStep, RowAvailable, NULL, NULL, NULL,
};

int PortGoalsRowCount(void) { return ROW_FIRST_GOAL + GOALS_N; }

/* tests (make goals-test drives the core; this is for a running game) */
int PortGoalsUnlockedCount(void) { return GoalsCount(&sSave); }

#ifdef __cplusplus
}
#endif
