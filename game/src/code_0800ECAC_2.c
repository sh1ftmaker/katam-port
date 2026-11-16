
#ifdef __cplusplus
extern "C" {
#endif

#include "object_collision.h"
#include <limits.h>
#include "code_0800ECAC.h"
#include "functions.h"
#include "kirby.h"
#include "random.h"
#include "constants/kirby.h"
#include "port/localmp.h"   /* PORT: human slots (PortIsHuman) */

u8 CpuNavNodeAt(struct ObjectBase *);
void CpuScanTerrain(struct CpuBrain *);

void CpuScanTargetFlying(struct CpuBrain *);
void CpuScanTargetMouthful(struct CpuBrain *);
void CpuScanNormal(struct CpuBrain *);
void CpuScanFire(struct CpuBrain *);
void CpuScanIce(struct CpuBrain *);
void CpuScanBurning(struct CpuBrain *);
void CpuScanWheel(struct CpuBrain *);
void CpuScanParasol(struct CpuBrain *);
void CpuScanCutter(struct CpuBrain *);
void CpuScanBeam(struct CpuBrain *);
void CpuScanStone(struct CpuBrain *);
void CpuScanBomb(struct CpuBrain *);
void CpuScanThrow(struct CpuBrain *);
void CpuScanCook(struct CpuBrain *);
void CpuScanLaser(struct CpuBrain *);
void CpuScanUfo(struct CpuBrain *);
void CpuScanSpark(struct CpuBrain *);
void CpuScanTornado(struct CpuBrain *);
void CpuScanHammer(struct CpuBrain *);
void CpuScanSword(struct CpuBrain *);
void CpuScanCupid(struct CpuBrain *);
void CpuScanFighter(struct CpuBrain *);
void CpuScanMagic(struct CpuBrain *);
void CpuScanSmash(struct CpuBrain *);
void CpuScanCrash(struct CpuBrain *);
void CpuScanMissile(struct CpuBrain *);
void CpuScanMaster(struct CpuBrain *);
void CpuScanUnderwater(struct CpuBrain *);
bool16 CpuBreakBlockNormal(struct CpuBrain *);
bool16 CpuBreakBlockFireIce(struct CpuBrain *);
bool16 CpuBreakBlockBurning(struct CpuBrain *);
bool16 CpuBreakBlockWheel(struct CpuBrain *);
bool16 CpuBreakBlockParasol(struct CpuBrain *);
bool16 CpuBreakBlockShot(struct CpuBrain *);
bool16 CpuBreakBlockBeam(struct CpuBrain *);
bool16 CpuBreakBlockStone(struct CpuBrain *);
bool16 CpuBreakBlockSparkSword(struct CpuBrain *);
bool16 CpuBreakBlockCupid(struct CpuBrain *);
bool16 CpuBreakBlockHammer(struct CpuBrain *);
bool16 CpuBreakBlockThrow(struct CpuBrain *);
bool16 CpuBreakBlockMissile(struct CpuBrain *);
bool16 CpuBreakBlockTornado(struct CpuBrain *);
void CpuScriptOpIfRoomState(struct CpuBrain *);
void CpuGoalRoamTick(struct CpuBrain *);
void CpuGoalOffscreenBegin(struct CpuBrain *);
void CpuGoalOffscreenRoamBegin(struct CpuBrain *);
void CpuGoalOffscreenRoamTick(struct CpuBrain *);
void CpuActThink(struct CpuBrain *);
void CpuActDecide(struct CpuBrain *);
void CpuActWalkStride(struct CpuBrain *);
void CpuActWalkTick(struct CpuBrain *);
void CpuActLadder(struct CpuBrain *);
void CpuActJump(struct CpuBrain *);
void CpuActJumpAir(struct CpuBrain *);
void CpuActFly(struct CpuBrain *);
void CpuActFlySweepBegin(struct CpuBrain *);
void CpuActFlySweepTick(struct CpuBrain *);
void CpuActThrow(struct CpuBrain *);
void CpuActSlide(struct CpuBrain *);
void CpuActAirPuff(struct CpuBrain *);
void CpuActSpit(struct CpuBrain *);
void CpuActInhale(struct CpuBrain *);
void CpuActSwallow(struct CpuBrain *);
void CpuActAttack(struct CpuBrain *);
void CpuActWaterAttack(struct CpuBrain *);
void CpuActBomb(struct CpuBrain *);
void CpuActThrow(struct CpuBrain *);
void CpuActThrowAim(struct CpuBrain *);
void CpuActUfoShot(struct CpuBrain *);
void CpuActCupidShot(struct CpuBrain *);
void CpuActMissile(struct CpuBrain *);
void CpuActMissileFly(struct CpuBrain *);
void CpuActMissileHomingSteer(struct CpuBrain *);
void CpuActRoom397(struct CpuBrain *);
void CpuActRoom397Tick(struct CpuBrain *);
bool16 CpuBreakBlockNone(struct CpuBrain *);
void CpuScanNone(struct CpuBrain *);
void CpuScanMini(struct CpuBrain *);
void CpuScriptOpNop(struct CpuBrain *);
void CpuScriptOpEnd(struct CpuBrain *);
void CpuScriptOpWait(struct CpuBrain *);
void CpuScriptOpGoToNode(struct CpuBrain *);
void CpuScriptOpSetAbility(struct CpuBrain *);
void CpuScriptOpFace(struct CpuBrain *);
void CpuScriptOpInhale(struct CpuBrain *);
void CpuScriptOpSlide(struct CpuBrain *);
void CpuScriptOpSkip(struct CpuBrain *);
void CpuScriptOpRoam(struct CpuBrain *);
void CpuGoalRoam(struct CpuBrain *);
void CpuGoalFollowBegin(struct CpuBrain *);
void CpuGoalRoom397Wait(struct CpuBrain *);
void CpuSenseIdle(struct CpuBrain *);
void CpuSenseMoving(struct CpuBrain *);
void CpuSenseRoom397(struct CpuBrain *);
void CpuSenseSurroundings(struct CpuBrain *);
void CpuSenseSurroundingsAndPath(struct CpuBrain *);
void CpuActLeaveFlight(struct CpuBrain *);
void CpuActDropAbility(struct CpuBrain *);
void CpuActAnswerPhone(struct CpuBrain *);
void CpuActBackToThink(struct CpuBrain *);
void CpuActMove(struct CpuBrain *);
void CpuActDashFirstTap(struct CpuBrain *);
void CpuActDashGap(struct CpuBrain *);
void CpuActDashSecondTap(struct CpuBrain *);
void CpuActDashHold(struct CpuBrain *);
void CpuActWalkPauseBegin(struct CpuBrain *);
void CpuActWalkPause(struct CpuBrain *);
void CpuActHoldDown(struct CpuBrain *);
void CpuActHoldDownTick(struct CpuBrain *);
void CpuActLadderGrab(struct CpuBrain *);
void CpuActLadderStart(struct CpuBrain *);
void CpuActLadderClimbBegin(struct CpuBrain *);
void CpuActJumpBegin(struct CpuBrain *);
void CpuActFlyFromGround(struct CpuBrain *);
void CpuActFlyJump(struct CpuBrain *);
void CpuActFlyPuffBegin(struct CpuBrain *);
void CpuActFlyPuff(struct CpuBrain *);
void CpuActFlySteerBegin(struct CpuBrain *);
void CpuActFlySteerLeg(struct CpuBrain *);
void CpuActSwim(struct CpuBrain *);
void CpuActSwimStrokeBegin(struct CpuBrain *);
void CpuActSwimStroke(struct CpuBrain *);
void CpuActSwimLeg(struct CpuBrain *);
void CpuActUfoMove(struct CpuBrain *);
void CpuActUfoMoveLeg(struct CpuBrain *);
void CpuActSlideAfterTurn(struct CpuBrain *);
void CpuActSlideBegin(struct CpuBrain *);
void CpuActSlideCrouch(struct CpuBrain *);
void CpuActSlideKick(struct CpuBrain *);
void CpuActAirPuffAfterTurn(struct CpuBrain *);
void CpuActAirPuffBegin(struct CpuBrain *);
void CpuActAirPuffPress(struct CpuBrain *);
void CpuActSpitAfterTurn(struct CpuBrain *);
void CpuActSpitBegin(struct CpuBrain *);
void CpuActSpitPress(struct CpuBrain *);
void CpuActInhaleAfterTurn(struct CpuBrain *);
void CpuActInhaleBegin(struct CpuBrain *);
void CpuActSwallowBegin(struct CpuBrain *);
void CpuActSwallowPress(struct CpuBrain *);
void CpuActAttackAfterTurn(struct CpuBrain *);
void CpuActAttackBegin(struct CpuBrain *);
void CpuActAttackHold(struct CpuBrain *);
void CpuActWaterAttackBegin(struct CpuBrain *);
void CpuActTurn(struct CpuBrain *);
void CpuActTurnBegin(struct CpuBrain *);
void CpuActBombAfterTurn(struct CpuBrain *);
void CpuActBombDraw(struct CpuBrain *);
void CpuActBombWait(struct CpuBrain *);
void CpuActBombThrowBegin(struct CpuBrain *);
void CpuActBombThrow(struct CpuBrain *);
void CpuActBombFollowBegin(struct CpuBrain *);
void CpuActBombFollow(struct CpuBrain *);
void CpuActBombCooldownBegin(struct CpuBrain *);
void CpuActBombCooldown(struct CpuBrain *);
void CpuActStoneToggle(struct CpuBrain *);
void CpuActStoneToggleBecome(struct CpuBrain *);
void CpuActStoneToggleBecomePress(struct CpuBrain *);
void CpuActStoneToggleFall(struct CpuBrain *);
void CpuActStoneToggleRevert(struct CpuBrain *);
void CpuActStoneToggleRevertWait(struct CpuBrain *);
void CpuActStoneDrop(struct CpuBrain *);
void CpuActStoneDropDispatch(struct CpuBrain *);
void CpuActStoneDropFromGround(struct CpuBrain *);
void CpuActStoneDropJumpBegin(struct CpuBrain *);
void CpuActStoneDropPress(struct CpuBrain *);
void CpuActStoneDropFall(struct CpuBrain *);
void CpuActStoneDropSitBegin(struct CpuBrain *);
void CpuActStoneDropSit(struct CpuBrain *);
void CpuActStoneDropRevert(struct CpuBrain *);
void CpuActStoneDropRevertPress(struct CpuBrain *);
void CpuActStoneDropRevertWait(struct CpuBrain *);
void CpuActThrowAfterTurn(struct CpuBrain *);
void CpuActThrowBegin(struct CpuBrain *);
void CpuActThrowHoldBegin(struct CpuBrain *);
void CpuActThrowHold(struct CpuBrain *);
void CpuActThrowAimHold(struct CpuBrain *);
void CpuActThrowRelease(struct CpuBrain *);
void CpuActThrowCooldownBegin(struct CpuBrain *);
void CpuActThrowCooldown(struct CpuBrain *);
void CpuActUfoShotAfterTurn(struct CpuBrain *);
void CpuActUfoShotBegin(struct CpuBrain *);
void CpuActUfoShotCharge(struct CpuBrain *);
void CpuActCupidShotAfterTurn(struct CpuBrain *);
void CpuActCupidShotBegin(struct CpuBrain *);
void CpuActMissileAfterTurn(struct CpuBrain *);
void CpuActMissileBegin(struct CpuBrain *);
void CpuActMissileHoming(struct CpuBrain *);
void CpuScriptWaitBegin(struct CpuBrain *, u16);
void CpuScriptWaitTick(struct CpuBrain *);
void CpuScriptGoToBegin(struct CpuBrain *, u16);
void CpuScriptGoToTick(struct CpuBrain *);
void CpuScriptFaceBegin(struct CpuBrain *, u16);
void CpuScriptFaceStart(struct CpuBrain *);
void CpuScriptFaceTick(struct CpuBrain *);
void CpuScriptInhaleBegin(struct CpuBrain *, u16);
void CpuScriptInhalePrepare(struct CpuBrain *);
void CpuScriptInhaleSpitFirst(struct CpuBrain *);
void CpuScriptInhaleSpitWait(struct CpuBrain *);
void CpuScriptInhaleLandFirst(struct CpuBrain *);
void CpuScriptInhaleLandWait(struct CpuBrain *);
void CpuScriptInhaleStart(struct CpuBrain *);
void CpuScriptInhaleTick(struct CpuBrain *);
void CpuScriptSlideBegin(struct CpuBrain *);
void CpuScriptSlideSpitFirst(struct CpuBrain *);
void CpuScriptSlideSpitWait(struct CpuBrain *);
void CpuScriptSlideLandFirst(struct CpuBrain *);
void CpuScriptSlideLandWait(struct CpuBrain *);
void CpuScriptSlideFallFirst(struct CpuBrain *);
void CpuScriptSlideFallWait(struct CpuBrain *);
void CpuScriptSlideStart(struct CpuBrain *);
void CpuScriptSlideTick(struct CpuBrain *);
void CpuGoalIdle(struct CpuBrain *);
void CpuGoalScriptNodeTick(struct CpuBrain *);
void CpuGoalRoamPauseBegin(struct CpuBrain *);
void CpuGoalFollowTick(struct CpuBrain *);
void CpuGoalOffscreenPauseBegin(struct CpuBrain *);

bool16 (*const gCpuBreakBlockChecks[])(struct CpuBrain *) = {
    [KIRBY_ABILITY_NORMAL]  = FN(CpuBreakBlockNormal),
    [KIRBY_ABILITY_FIRE]    = FN(CpuBreakBlockFireIce),
    [KIRBY_ABILITY_ICE]     = FN(CpuBreakBlockFireIce),
    [KIRBY_ABILITY_BURNING] = FN(CpuBreakBlockBurning),
    [KIRBY_ABILITY_WHEEL]   = FN(CpuBreakBlockWheel),
    [KIRBY_ABILITY_PARASOL] = FN(CpuBreakBlockParasol),
    [KIRBY_ABILITY_CUTTER]  = FN(CpuBreakBlockShot),
    [KIRBY_ABILITY_BEAM]    = FN(CpuBreakBlockBeam),
    [KIRBY_ABILITY_STONE]   = FN(CpuBreakBlockStone),
    [KIRBY_ABILITY_BOMB]    = FN(CpuBreakBlockNormal),
    [KIRBY_ABILITY_THROW]   = FN(CpuBreakBlockThrow),
    [KIRBY_ABILITY_SLEEP]   = FN(CpuBreakBlockNormal),
    [KIRBY_ABILITY_COOK]    = FN(CpuBreakBlockNormal),
    [KIRBY_ABILITY_LASER]   = FN(CpuBreakBlockShot),
    [KIRBY_ABILITY_UFO]     = FN(CpuBreakBlockShot),
    [KIRBY_ABILITY_SPARK]   = FN(CpuBreakBlockSparkSword),
    [KIRBY_ABILITY_TORNADO] = FN(CpuBreakBlockTornado),
    [KIRBY_ABILITY_HAMMER]  = FN(CpuBreakBlockHammer),
    [KIRBY_ABILITY_SWORD]   = FN(CpuBreakBlockSparkSword),
    [KIRBY_ABILITY_CUPID]   = FN(CpuBreakBlockCupid),
    [KIRBY_ABILITY_FIGHTER] = FN(CpuBreakBlockNormal),
    [KIRBY_ABILITY_MAGIC]   = FN(CpuBreakBlockNormal),
    [KIRBY_ABILITY_SMASH]   = FN(CpuBreakBlockNormal),
    [KIRBY_ABILITY_MINI]    = FN(CpuBreakBlockNone),
    [KIRBY_ABILITY_CRASH]   = FN(CpuBreakBlockNormal),
    [KIRBY_ABILITY_MISSILE] = FN(CpuBreakBlockMissile),
    [KIRBY_ABILITY_MASTER]  = FN(CpuBreakBlockHammer),
};

void (*const gCpuTargetScansLand[])(struct CpuBrain *) = {
    [KIRBY_ABILITY_NORMAL]  = FN(CpuScanNormal),
    [KIRBY_ABILITY_FIRE]    = FN(CpuScanFire),
    [KIRBY_ABILITY_ICE]     = FN(CpuScanIce),
    [KIRBY_ABILITY_BURNING] = FN(CpuScanBurning),
    [KIRBY_ABILITY_WHEEL]   = FN(CpuScanWheel),
    [KIRBY_ABILITY_PARASOL] = FN(CpuScanParasol),
    [KIRBY_ABILITY_CUTTER]  = FN(CpuScanCutter),
    [KIRBY_ABILITY_BEAM]    = FN(CpuScanBeam),
    [KIRBY_ABILITY_STONE]   = FN(CpuScanStone),
    [KIRBY_ABILITY_BOMB]    = FN(CpuScanBomb),
    [KIRBY_ABILITY_THROW]   = FN(CpuScanThrow),
    [KIRBY_ABILITY_SLEEP]   = FN(CpuScanNone),
    [KIRBY_ABILITY_COOK]    = FN(CpuScanCook),
    [KIRBY_ABILITY_LASER]   = FN(CpuScanLaser),
    [KIRBY_ABILITY_UFO]     = FN(CpuScanUfo),
    [KIRBY_ABILITY_SPARK]   = FN(CpuScanSpark),
    [KIRBY_ABILITY_TORNADO] = FN(CpuScanTornado),
    [KIRBY_ABILITY_HAMMER]  = FN(CpuScanHammer),
    [KIRBY_ABILITY_SWORD]   = FN(CpuScanSword),
    [KIRBY_ABILITY_CUPID]   = FN(CpuScanCupid),
    [KIRBY_ABILITY_FIGHTER] = FN(CpuScanFighter),
    [KIRBY_ABILITY_MAGIC]   = FN(CpuScanMagic),
    [KIRBY_ABILITY_SMASH]   = FN(CpuScanSmash),
    [KIRBY_ABILITY_MINI]    = FN(CpuScanMini),
    [KIRBY_ABILITY_CRASH]   = FN(CpuScanCrash),
    [KIRBY_ABILITY_MISSILE] = FN(CpuScanMissile),
    [KIRBY_ABILITY_MASTER]  = FN(CpuScanMaster),
};

void (*const gCpuTargetScansWater[])(struct CpuBrain *) = {
    [KIRBY_ABILITY_NORMAL]  = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_FIRE]    = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_ICE]     = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_BURNING] = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_WHEEL]   = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_PARASOL] = FN(CpuScanParasol),
    [KIRBY_ABILITY_CUTTER]  = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_BEAM]    = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_STONE]   = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_BOMB]    = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_THROW]   = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_SLEEP]   = FN(CpuScanNone),
    [KIRBY_ABILITY_COOK]    = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_LASER]   = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_UFO]     = FN(CpuScanUfo),
    [KIRBY_ABILITY_SPARK]   = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_TORNADO] = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_HAMMER]  = FN(CpuScanHammer),
    [KIRBY_ABILITY_SWORD]   = FN(CpuScanSword),
    [KIRBY_ABILITY_CUPID]   = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_FIGHTER] = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_MAGIC]   = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_SMASH]   = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_MINI]    = FN(CpuScanMini),
    [KIRBY_ABILITY_CRASH]   = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_MISSILE] = FN(CpuScanUnderwater),
    [KIRBY_ABILITY_MASTER]  = FN(CpuScanMaster),
};

extern const u32 gCpuAbilityAttackMasks[] = {
    [KIRBY_ABILITY_NORMAL]  = 0x20000042,
    [KIRBY_ABILITY_FIRE]    = 0x20000103,
    [KIRBY_ABILITY_ICE]     = 0x20000203,
    [KIRBY_ABILITY_BURNING] = 0x30001103,
    [KIRBY_ABILITY_WHEEL]   = 0x30000043,
    [KIRBY_ABILITY_PARASOL] = 0x20000043,
    [KIRBY_ABILITY_CUTTER]  = 0x20002003,
    [KIRBY_ABILITY_BEAM]    = 0x20000403,
    [KIRBY_ABILITY_STONE]   = 0x30005003,
    [KIRBY_ABILITY_BOMB]    = 0x20000103,
    [KIRBY_ABILITY_THROW]   = 0x20001003,
    [KIRBY_ABILITY_SLEEP]   = 0x20000082,
    [KIRBY_ABILITY_COOK]    = 0x20008003,
    [KIRBY_ABILITY_LASER]   = 0x20000103,
    [KIRBY_ABILITY_UFO]     = 0x20000103,
    [KIRBY_ABILITY_SPARK]   = 0x20000403,
    [KIRBY_ABILITY_TORNADO] = 0x30000043,
    [KIRBY_ABILITY_HAMMER]  = 0x30005003,
    [KIRBY_ABILITY_SWORD]   = 0x30002003,
    [KIRBY_ABILITY_CUPID]   = 0x2003,
    [KIRBY_ABILITY_FIGHTER] = 0x30000043,
    [KIRBY_ABILITY_MAGIC]   = 0x20000082,
    [KIRBY_ABILITY_SMASH]   = 0x30000043,
    [KIRBY_ABILITY_MINI]    = 0x20000082,
    [KIRBY_ABILITY_CRASH]   = 0x10003,
    [KIRBY_ABILITY_MISSILE] = 0x30001043,
    [KIRBY_ABILITY_MASTER]  = 0x30007103,
};

extern const u8 gCpuOffscreenAbilityPool[] = {
    KIRBY_ABILITY_FIRE, KIRBY_ABILITY_FIRE, KIRBY_ABILITY_FIRE, KIRBY_ABILITY_FIRE,
    KIRBY_ABILITY_ICE, KIRBY_ABILITY_ICE, KIRBY_ABILITY_ICE,
    KIRBY_ABILITY_CUTTER, KIRBY_ABILITY_CUTTER, KIRBY_ABILITY_CUTTER,
    KIRBY_ABILITY_BEAM, KIRBY_ABILITY_BEAM, KIRBY_ABILITY_BEAM,
    KIRBY_ABILITY_SWORD, KIRBY_ABILITY_SWORD, KIRBY_ABILITY_SWORD,
    KIRBY_ABILITY_CUTTER, KIRBY_ABILITY_CUTTER, KIRBY_ABILITY_CUTTER,
    KIRBY_ABILITY_CUPID, KIRBY_ABILITY_CUPID, KIRBY_ABILITY_CUPID, KIRBY_ABILITY_CUPID,
    KIRBY_ABILITY_MISSILE, KIRBY_ABILITY_MISSILE, KIRBY_ABILITY_MISSILE,
    KIRBY_ABILITY_BURNING, KIRBY_ABILITY_BURNING, KIRBY_ABILITY_BURNING,
    KIRBY_ABILITY_PARASOL, KIRBY_ABILITY_PARASOL, KIRBY_ABILITY_PARASOL,
    KIRBY_ABILITY_WHEEL, KIRBY_ABILITY_WHEEL, KIRBY_ABILITY_WHEEL,
    KIRBY_ABILITY_PARASOL, KIRBY_ABILITY_PARASOL, KIRBY_ABILITY_PARASOL, KIRBY_ABILITY_PARASOL,
    KIRBY_ABILITY_BOMB, KIRBY_ABILITY_BOMB, KIRBY_ABILITY_BOMB,
    KIRBY_ABILITY_LASER, KIRBY_ABILITY_LASER, KIRBY_ABILITY_LASER,
    KIRBY_ABILITY_SPARK, KIRBY_ABILITY_SPARK, KIRBY_ABILITY_SPARK,
    KIRBY_ABILITY_HAMMER, KIRBY_ABILITY_HAMMER,
    KIRBY_ABILITY_SMASH,
    KIRBY_ABILITY_TORNADO, KIRBY_ABILITY_TORNADO, KIRBY_ABILITY_TORNADO,
    KIRBY_ABILITY_FIGHTER, KIRBY_ABILITY_FIGHTER, KIRBY_ABILITY_FIGHTER,
    KIRBY_ABILITY_UFO,
    KIRBY_ABILITY_STONE, KIRBY_ABILITY_STONE,
    KIRBY_ABILITY_THROW,
    KIRBY_ABILITY_COOK,
    KIRBY_ABILITY_MAGIC,
    KIRBY_ABILITY_CRASH,
};

extern const u16 gCpuLinkedRooms[][2] = {
    { 0x12C, 0x144 },
    { 0x2BC, 0x2C6 },
    { 0x190, 0x1A1 },
    { 0x320, 0x336 },
    {  0xC8,  0xDB },
    { 0x1F4, 0x204 },
    { 0x258, 0x2E2 },
    {  0x72,  0x81 },
};

struct Kirby *CpuNearestHumanInRoom(struct ObjectBase *objBase, s16 *a2) {
    u16 i;
    struct Kirby *ret = NULL;
    s16 dx, dy;
    s16 max;

    *a2 = SHRT_MAX;
    for (i = 0; i < 4; ++i) {
        if (i != objBase->unk56
            && PortIsHuman(gKirbys[i].base.unk56)
            && gCurLevelInfo[i].currentRoom == gCurLevelInfo[objBase->unk56].currentRoom) {
            dx = objBase->x >> 8 > gKirbys[i].base.x >> 8
                ? (objBase->x >> 8) - (gKirbys[i].base.x >> 8)
                : (gKirbys[i].base.x >> 8) - (objBase->x >> 8);
            dy = objBase->y >> 8 > gKirbys[i].base.y >> 8
                ? (objBase->y >> 8) - (gKirbys[i].base.y >> 8)
                : (gKirbys[i].base.y >> 8) - (objBase->y >> 8);
            max = dy < dx ? dx : dy;
            if (!ret || *a2 > max) {
                *a2 = max;
                ret = gKirbys + i;
            }
        }
    }
    return ret;
}

void CpuDecideMouthful(struct CpuBrain *brain) {
    struct Kirby *kirby = brain->kirby;

    if (kirby->base.flags & 0x80) {
        if (!brain->swallowCooldown) {
            if (brain->wantAbility > KIRBY_ABILITY_NORMAL && brain->wantAbility <= KIRBY_ABILITY_MASTER
                && (kirby->transitioningAbility & 0x1F) == brain->wantAbility) {
                if (brain->action != CPU_ACT_SWALLOW)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwallow);
            } else {
                if (brain->wantAbility == 0xFF) {
                    if (brain->action != CPU_ACT_SPIT) {
                        brain->actArg = 0xFFFF;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActSpit);
                    }
                } else {
                    if ((Rand16() & 0xFF) < 0xC0) {
                        if ((Rand16() & 0xFF) < 0xFF) {
                            if (brain->action != CPU_ACT_SWALLOW)
                                brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwallow);
                        } else if (brain->action != CPU_ACT_SPIT) {
                            brain->actArg = 0xFFFF;
                            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSpit);
                        }
                    } else {
                        brain->swallowCooldown = (Rand16() & 0x7F) + 0x1E;
                    }
                }
            }
        } else {
            --brain->swallowCooldown;
        }
    }
}

u8 CpuPickRandomNode(struct LevelInfo *li, u8 a2) {
    const struct LevelInfo_1E8 *r4 = gUnk_08D63C28[gRoomProps[li->currentRoom].unk22];
    const struct LevelInfo_1E8_14 *r8 = r4->unk14, *r9 = &r8[a2];
    u16 array[] = {
        li->unk_S32Vec2_6C.x >> 8, li->unk_S32Vec2_6C.y >> 8,
        li->unk_S32Vec2_74.x >> 8, li->unk_S32Vec2_74.y >> 8,
    };
    u16 r = Rand16();
    u32 mask = 0x1F; // damn
    u16 r5 = 0xFFFF;

    for (r &= mask; r != 0xFFFF; --r) {
        while (1) {
            for (++r5; r5 < r4->unk18; ++r5) {
                if (r9->unk0[r5] != 0xFF
                    && array[0] < r8[r5].unk20 && r8[r5].unk20 < array[2]
                    && array[1] < r8[r5].unk22 && r8[r5].unk22 < array[3]) {
                    break;
                }
            }
            if (r5 < r4->unk18)
                break;
            r5 = 0xFFFF;
        }
    }
    return r5;
}

void CpuUpdateGoalVectors(struct CpuBrain *brain) {
    brain->goalDx = (brain->kirby->base.x > brain->goalX
        ? brain->kirby->base.x - brain->goalX
        : brain->goalX - brain->kirby->base.x) >> 8;
    brain->goalDy = (brain->kirby->base.y > brain->goalY
        ? brain->kirby->base.y - brain->goalY
        : brain->goalY - brain->kirby->base.y) >> 8;
    brain->goalDxDyDiff = brain->goalDx > brain->goalDy ? brain->goalDx - brain->goalDy : brain->goalDy - brain->goalDx;
    brain->goalAngle = sub_08154FE8((brain->goalX - brain->kirby->base.x) >> 8, (brain->goalY - brain->kirby->base.y) >> 8) & 0x3FF;
    if (brain->curNode != brain->nextNode && brain->nextNode != 0xFF) {
        const struct LevelInfo_1E8_14 *r2 = &brain->levelInfo->unk1E8->unk14[brain->nextNode];

        brain->waypointDx = r2->unk20 - (brain->kirby->base.x >> 8);
        brain->waypointDy = r2->unk22 - (brain->kirby->base.y >> 8);
        brain->waypointAngle = sub_08154FE8(brain->waypointDx, brain->waypointDy) & 0x3FF;
        brain->waypointDx = abs(brain->waypointDx);
        brain->waypointDy = abs(brain->waypointDy);
        brain->waypointDist = (u16)brain->waypointDy < (u16)brain->waypointDx ? brain->waypointDx : brain->waypointDy;
    } else {
        brain->waypointX = brain->goalX;
        brain->waypointY = brain->goalY;
        brain->waypointDx = brain->goalDx;
        brain->waypointDy = brain->goalDy;
        brain->waypointDist = brain->goalDxDyDiff;
        brain->waypointAngle = brain->goalAngle;
    }
}

void CpuUpdateTargetMemory(struct CpuBrain *brain) {
    u16 i;

    if (brain->target && brain->target->base.flags & 0x1000)
        brain->target = NULL;
    for (i = 0; i < 4; ++i) {
        if (brain->targetMemory[i].obj) {
            if (brain->targetMemory[i].obj->base.flags & 0x1000)
                brain->targetMemory[i].obj = NULL;
            else {
                if (!brain->targetMemory[i].ignoring) {
                    if (brain->target == brain->targetMemory[i].obj
                        && brain->target->unk80 != brain->targetMemory[i].lastHp) {
                        Rand32();
                        brain->targetMemory[i].timer += 0x3C + ((gRngVal >> 0x10) & 0x7F);
                        brain->targetMemory[i].lastHp = brain->target->unk80;
                    } else if (!--brain->targetMemory[i].timer) {
                        u32 r = Rand32();
                        u32 tmp = brain->targetMemory[i].timer + 0x12C;

                        tmp += (r >> 0x10) & 0xFF;
                        brain->targetMemory[i].timer = tmp;
                        brain->targetMemory[i].ignoring = 1;
                    }
                } else if (!--brain->targetMemory[i].timer) {
                    brain->targetMemory[i].obj = NULL;
                }
            }
        }
    }
}

bool32 CpuUpdateFollow(struct CpuBrain *brain) {
    u16 i;
    bool32 r7;
    s16 stack;

    if (!(brain->flags & CPU_FLAG_FREE))
        return (brain->flags >> 4) & 1;
    for (i = 0; i < 0x10; ++i) {
        if (gUnk_0835105C[i] == brain->levelInfo->currentRoom)
            break;
    }
    if (i < 0x10)
        return FALSE;
    if (brain->flags & CPU_FLAG_FOLLOWING) {
        r7 = FALSE;
        if (!brain->followKirby
            || brain->levelInfo->currentRoom != brain->followKirby->base.roomId) {
            r7 = TRUE;
        } else {
            s16 dx = brain->kirby->base.x >> 8 > brain->followKirby->base.x >> 8
                ? (brain->kirby->base.x >> 8) - (brain->followKirby->base.x >> 8)
                : (brain->followKirby->base.x >> 8) - (brain->kirby->base.x >> 8);
            s16 dy = brain->kirby->base.y >> 8 > brain->followKirby->base.y >> 8
                ? (brain->kirby->base.y >> 8) - (brain->followKirby->base.y >> 8)
                : (brain->followKirby->base.y >> 8) - (brain->kirby->base.y >> 8);
            s16 max = dy < dx ? dx : dy;

            if (max < 0x50) {
                if (brain->nearHumanMeter < 0x12C0) brain->nearHumanMeter -= max - 0x50;
                if (brain->nearHumanMeter > 0x12C0) brain->nearHumanMeter = 0x12C0;
            } else {
                if (brain->nearHumanMeter > 0x10)
                    brain->nearHumanMeter -= 0x10;
                else
                    brain->nearHumanMeter = 0;
            }
            if (!brain->nearHumanMeter)
                r7 = TRUE;
            if (brain->kirby->base.xspeed == 0
                && brain->kirby->base.yspeed == 0)
                ++brain->followStillFrames;
            else
                brain->followStillFrames = 0;
            if (brain->followStillFrames > 600)
                r7 = TRUE;
        }
        if (r7) {
            brain->flags &= ~CPU_FLAG_FOLLOWING;
            brain->followKirby = NULL;
            brain->nearHumanMeter = 0;
            brain->followStillFrames = 0;
        }
        return (brain->flags >> 4) & 1;
    } else {
        struct Kirby *kirby = CpuNearestHumanInRoom(&brain->kirby->base, &stack);

        if (kirby && stack < 0x50) {
            if (brain->nearHumanMeter < 0x12C0) brain->nearHumanMeter -= stack - 0x50;
            if (brain->nearHumanMeter > 0x12C0) brain->nearHumanMeter = 0x12C0;
        } else {
            if (brain->nearHumanMeter > 0x10)
                brain->nearHumanMeter -= 0x10;
            else
                brain->nearHumanMeter = 0;
        }
        if (brain->nearHumanMeter > 0xB40 && kirby) {
            brain->flags |= CPU_FLAG_FOLLOWING;
            brain->followKirby = kirby;
            brain->followStillFrames = 0;
        }
        return (brain->flags >> 4) & 1;
    }
}

// https://decomp.me/scratch/Q0OQa
struct Object *CpuFindTarget(struct CpuBrain *brain, s32 a2, s32 a3) {
    s32 dx, dy;
    PTR32(struct ObjectBase) *r7 = GetRoomObjectCollisionList(&brain->kirby->base);
    struct Object *sp = NULL;
    s32 x = brain->kirby->base.x;
    s32 y = brain->kirby->base.y;

    for (; *r7; ++r7) {
        struct Object *r3 = (struct Object *)*r7;
        struct Object *r5 = (struct Object *)*r7;

        if (r3->base.flags & 0x10000000 || !(r3->base.flags & 0x8000)) {
            if (ObjType0To37(r3) || ObjType38To52(r3) || ObjType43To52(r3) || ObjType53To5C(r3) || r3->type == OBJ_EMPTY_5D) {
                switch (r3->type) {
                case OBJ_JACK:
                case OBJ_COOKIN:
                case OBJ_MINNY:
                case OBJ_PRANK:
                case OBJ_MIRRA:
                    continue;
                case OBJ_MR_FROSTY:
                case OBJ_BONKERS:
                case OBJ_PHAN_PHAN:
                case OBJ_BATAFIRE:
                case OBJ_BOX_BOXER:
                case OBJ_BOXY:
                case OBJ_MASTER_HAND:
                case OBJ_BOMBAR:
                    if (r5->unk80 <= 0)
                        continue;
                    break;
                default:
                case OBJ_BIG_WADDLE_DEE:
                case OBJ_WADDLE_DOO:
                case OBJ_FLAMER:
                case OBJ_HOT_HEAD:
                case OBJ_LASER_BALL:
                case OBJ_PENGY:
                case OBJ_ROCKY:
                case OBJ_SIR_KIBBLE:
                case OBJ_SPARKY:
                case OBJ_SWORD_KNIGHT:
                case OBJ_UFO:
                case OBJ_TWISTER:
                case OBJ_WHEELIE:
                case OBJ_NODDY:
                case OBJ_GOLEM_1:
                case OBJ_GOLEM_2:
                case OBJ_GOLEM_3:
                case OBJ_FOLEY_1:
                case OBJ_SHOOTY:
                case OBJ_SCARFY_FALLING:
                case OBJ_BOXIN:
                case OBJ_BOMBER:
                case OBJ_HEAVY_KNIGHT:
                case OBJ_GIANT_ROCKY:
                case OBJ_METAL_GUARDIAN:
                case OBJ_EMPTY_2C:
                case OBJ_BATTY:
                case OBJ_FOLEY_2:
                case OBJ_BANG_BANG:
                case OBJ_DARK_MIND_BOMB:
                case OBJ_EMPTY_31:
                case OBJ_DROPPY:
                case OBJ_SHOTZO_1:
                case OBJ_SHADOW_KIRBY:
                case OBJ_WADDLE_DEE_2:
                    break;
                }
            } else {
                if (ObjType5ETo6C(r3))
                    continue;
                if (ObjType6Dto9A(r3)) {
#ifndef NONMATCHING
                    /* PORT: asm barrier removed */
#endif
                    if (r3->type != OBJ_CANNON_FUSE && r3->type != OBJ_STAR_PLATFORM)
                        continue;
                } else {
#ifndef NONMATCHING
                    /* PORT: asm barrier removed */
#endif
                    if (r3->type != OBJ_ABILITY_STAR_1) {
                        if (r3->type == OBJ_ABILITY_STAR_2)
                            continue;
                    } else {
                        if (r3->base.parent == brain->kirby) {
                            if (((struct Kirby *)r3->base.parent)->ability != KIRBY_ABILITY_NORMAL)
                                continue;
                            if (r3->base.counter > 60)
                                goto label; // TODO
                            continue;
                        } else {
                            if (r3->base.counter <= 120)
                                continue;
                        }
                    }
                }
            }
        label:
            dx = x > r3->base.x ? x - r3->base.x : r3->base.x - x;
            dy = y > r3->base.y ? y - r3->base.y : r3->base.y - y;
            if (ObjType38To52(r5)) {
                dx >>= 2;
                dy >>= 1;
                if (a2 >= dx && a3 >= dy)
                    sp = r5;
            } else {
                if (a2 >= dx && a3 >= dy) {
                    struct CpuTargetMemory *r6 = NULL;
                    bool32 b = FALSE;
                    u16 i;

                    for (i = 0; i < 4; ++i) {
                        if (brain->targetMemory[i].obj == r3) {
                            b = TRUE;
                            if (brain->targetMemory[i].ignoring)
                                break;
                        } else {
                            if (!brain->targetMemory[i].obj)
                                r6 = &brain->targetMemory[i];
                        }
                    }
                    if (i == 4) {
                        if (!b && r6) {
                            r6->ignoring = 0;
                            r6->obj = r3;
                            r6->lastHp = r3->unk80;
                            r6->timer = (Rand16() & 0xFF) + 0x12C;
                        }
                        return r3;
                    }
                }
            }
        }
    }
    return sp;
}

bool32 CpuTryHopOverTarget(struct CpuBrain *brain, struct Object *obj) {
    s16 var;

    switch (brain->action) {
    case 7:
    case 8:
    case 0xA:
        return TRUE;
    }
    if (brain->kirby->base.xspeed) {
        if (brain->kirby->base.flags & 1) {
            if (brain->kirby->base.x <= obj->base.x)
                return FALSE;
        } else {
            if (brain->kirby->base.x >= obj->base.x)
                return FALSE;
        }
        var = (brain->kirby->base.y - obj->base.y) >> 8;
        if (var + 7 >= 0 && var + 7 <= 0x1E) {
            if (brain->action != CPU_ACT_JUMP) {
                brain->actArg = (Rand16() & 1) + 3;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActJump);
            }
            return TRUE;
        }
    }
    return FALSE;
}

void CpuScanForTarget(struct CpuBrain *brain) {
    u32 ability = brain->kirby->ability;

    if (ability == KIRBY_ABILITY_UFO) {
        if (brain->kirby->base.unk58 & 2)
            CALL(gCpuTargetScansWater[KIRBY_ABILITY_UFO])(brain);
        else
            CALL(gCpuTargetScansLand[KIRBY_ABILITY_UFO])(brain);
    } else if (brain->kirby->base.flags & 0x40) {
        CpuScanTargetFlying(brain);
    } else if (brain->kirby->base.flags & 0x80) {
        CpuScanTargetMouthful(brain);
    } else {
        if (brain->kirby->base.unk58 & 2)
            CALL(gCpuTargetScansWater[ability])(brain);
        else
            CALL(gCpuTargetScansLand[ability])(brain);
    }
}

static inline bool32 IsOneOfTheCurrentRooms(u16 roomId) {
    u16 i;

    for (i = 0; i < PortHumanEnd(); i = PortHumanNext(i)) {
        if (roomId == gCurLevelInfo[i].currentRoom)
            return TRUE;
    }
    return FALSE;
}

static inline bool32 CpuRowIsClear(struct CpuBrain *brain, u16 sl, bool16 b) {
    u32 r8 = brain->levelInfo->roomWidth >> 4;
    s16 r7 = b == TRUE ? -1 : 1;
    u16 r4 = r7 + (brain->kirby->base.x >> 12);
    u16 sb = brain->kirby->base.y >> 12;

    for (; sl-- && r4 < r8; r4 += r7) {
        if (!(gCollisionAttributes[sub_080024F0(brain->levelInfo, r4, sb)] & 1))
            return FALSE;
    }
    return TRUE;
}

void CpuScanTargetFlying(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x1800);
    if (brain->target) {
        if (!IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom))
            return;
        if (~(brain->target->base.unk5C & ~7) & 0x40 && (brain->target->base.unk5C & 7) < 3) {
            if (brain->action != CPU_ACT_AIR_PUFF) {
                brain->actArg = brain->kirby->base.x >= brain->target->base.x;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAirPuff);
            }
        } else {
            u32 ability = brain->kirby->ability;
            bool32 b = FALSE;

            if (ability < KIRBY_ABILITY_COUNT) {
                // TODO: better match
                const u32 *table = gCpuAbilityAttackMasks;
                const u32 *r2 = &table[ability];
                u32 inter = brain->target->base.unk5C & ~7;
                u32 var = 0x3FFFF8 & ~inter;

                if (var & *r2
                    && (*r2 & 7) >= (brain->target->base.unk5C & 7))
                    b = TRUE;
            }
            if (b)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        }
    }
}

void CpuScanTargetMouthful(struct CpuBrain *brain) {
    if ((brain->kirby->transitioningAbility & 0x1F) != KIRBY_ABILITY_NORMAL
        && (brain->kirby->transitioningAbility & 0x1F) < KIRBY_ABILITY_COUNT
        && (Rand16() & 0xFF) < 0xC0) {
        if (brain->action != CPU_ACT_SWALLOW)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwallow);
    } else {
        brain->target = CpuFindTarget(brain, 0x3000, 0x1800);
        if (brain->target && brain->action != CPU_ACT_SPIT) {
            brain->actArg = brain->kirby->base.x >= brain->target->base.x;
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSpit);
        }
    }
}

void CpuScanNormal(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x1800);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x20 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_INHALE) {
                    brain->actArg = b;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActInhale);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanFire(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x100 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = b;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanIce(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x200 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = b;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanBurning(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x5000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x1100 && (brain->target->base.unk5C & 7) < 4) {
                bool16 b = brain->kirby->base.x >= brain->target->base.x;
                s16 r0 = (brain->kirby->base.x - brain->target->base.x) >> 0xC;

                if (CpuRowIsClear(brain, abs(r0), b)) {
                    if (brain->action != CPU_ACT_ATTACK) {
                        brain->actArg = b;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                    }
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanWheel(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x5000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x40 && (brain->target->base.unk5C & 7) < 4) {
                bool16 b = brain->kirby->base.x >= brain->target->base.x;
                s16 r0 = (brain->kirby->base.x - brain->target->base.x) >> 0xC;

                if (CpuRowIsClear(brain, abs(r0), b)) {
                    if (brain->action != CPU_ACT_ATTACK) {
                        brain->actArg = b;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                    }
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanParasol(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x40 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = b;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanCutter(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x2000 && (brain->target->base.unk5C & 7) < 4) {
                bool16 b = brain->kirby->base.x >= brain->target->base.x;
                s16 r0 = (brain->kirby->base.x - brain->target->base.x) >> 0xC;

                if (CpuRowIsClear(brain, abs(r0), b)) {
                    if (brain->action != CPU_ACT_ATTACK) {
                        brain->actArg = b;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                    }
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanBeam(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x400 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = b;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanStone(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x3000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x5000 && (brain->target->base.unk5C & 7) < 4) {
                s16 r5 = (brain->kirby->base.y - brain->target->base.y) >> 8;
                s16 r0 = (brain->kirby->base.x - brain->target->base.x) >> 8;

                r0 = abs(r0);
                if (r0 < 0x30 && r5 < 0x40) {
                    bool32 b = brain->kirby->base.x >= brain->target->base.x;

                    if (brain->action != CPU_ACT_STONE_DROP) {
                        brain->actArg = b;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDrop);
                    }
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanBomb(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x4000, 0x3000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x100 && (brain->target->base.unk5C & 7) < 4) {
                bool16 b = brain->kirby->base.x >= brain->target->base.x;
                u16 r3 = 0xFFFF;
                s16 r0 = (brain->kirby->base.x - brain->target->base.x) >> 0xC;
                s16 unk;

                if (CpuRowIsClear(brain, abs(r0), b)) {
                    unk = (brain->kirby->base.y - brain->target->base.y) >> 0xC;
                    if (unk >= 0x2FFF)
                        r3 = 2;
                    if (brain->action != CPU_ACT_BOMB) {
                        brain->actArg = b;
                        brain->actArg2 = r3;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActBomb);
                    }
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanThrow(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x2000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x200020 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = b;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanCook(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x1000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x8000 && (brain->target->base.unk5C & 7) < 4) {
                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = 0xFFFF;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanLaser(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x6000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x100 && (brain->target->base.unk5C & 7) < 4) {
                bool16 b = brain->kirby->base.x >= brain->target->base.x;
                s16 r0 = (brain->kirby->base.x - brain->target->base.x) >> 0xC;

                if (CpuRowIsClear(brain, abs(r0), b)) {
                    if (brain->action != CPU_ACT_ATTACK) {
                        brain->actArg = b;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                    }
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanUfo(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x6000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            bool16 b = brain->kirby->base.x >= brain->target->base.x;
            s16 r0 = (brain->kirby->base.x - brain->target->base.x) >> 0xC;
            s16 sl = abs(r0);

            if (Rand16() & 1 && sl < 4 && ~(brain->target->base.unk5C & ~7) & 0x400 && (brain->target->base.unk5C & 7) < 4) {
                if (brain->action != CPU_ACT_UFO_SHOT) {
                    brain->actArg = b;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoShot);
                }
                return;
            }
            if (Rand16() & 1 && ~(brain->target->base.unk5C & ~7) & 0x100 && (brain->target->base.unk5C & 7) < 4) {
                if (CpuRowIsClear(brain, sl, b)) {
                    if (brain->action != CPU_ACT_UFO_SHOT) {
                        brain->actArg = b;
                        brain->actArg2 = 1;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoShot);
                    }
                    return;
                }
            }
            if (~(brain->target->base.unk5C & ~7) & 0x100 && (brain->target->base.unk5C & 7) < 4) {
                if (CpuRowIsClear(brain, sl, b)) {
                    if (brain->action != CPU_ACT_UFO_SHOT) {
                        brain->actArg = b;
                        brain->actArg2 = 2;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoShot);
                    }
                    return;
                }
            }
            if (~(brain->target->base.unk5C & ~7) & 0x1100 && (brain->target->base.unk5C & 7) < 4) {
                if (brain->action != CPU_ACT_UFO_SHOT) {
                    brain->actArg = b;
                    brain->actArg2 = 3;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoShot);
                }
            }
            return;
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanSpark(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x1800, 0x1800);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x400 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = b;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanTornado(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x40 && (brain->target->base.unk5C & 7) < 4) {
                bool16 b = brain->kirby->base.x >= brain->target->base.x;
                s16 r0 = (brain->kirby->base.x - brain->target->base.x) >> 0xC;

                if (CpuRowIsClear(brain, abs(r0), b)) {
                    if (brain->action != CPU_ACT_ATTACK) {
                        brain->actArg = b;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                    }
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanHammer(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x2000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x5000 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = b;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanSword(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x2000 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = b;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanCupid(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x6000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x2000 && (brain->target->base.unk5C & 7) < 4) {
                bool16 b = brain->kirby->base.x >= brain->target->base.x;
                s16 r0 = (brain->kirby->base.x - brain->target->base.x) >> 0xC;
                s16 sp04 = abs(r0);

                if (CpuRowIsClear(brain, sp04, b)) {
                    if (brain->action != CPU_ACT_CUPID_SHOT) {
                        brain->actArg = b;
                        brain->actArg2 = 0xFFFF;
                        brain->actNext.data = sp04 >> 1;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActCupidShot);
                    }
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanFighter(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x2000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x40 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = b;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanMagic(struct CpuBrain *brain) {
    brain->target = NULL;
    if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
        if ((Rand16() & 0xFF) < 5 && brain->action != CPU_ACT_ATTACK) {
            brain->actArg = 0xFFFF;
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
        }
    }
}

void CpuScanSmash(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x2000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x40 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = b;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanCrash(struct CpuBrain *brain) {
    brain->target = NULL;
    if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
        if ((Rand16() & 0xFF) < 5 && brain->action != CPU_ACT_ATTACK) {
            brain->actArg = 0xFFFF;
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
        }
    }
}

void CpuScanMissile(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x5000, 0x2000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x1000 && (brain->target->base.unk5C & 7) < 4) {
                bool16 b = brain->kirby->base.x >= brain->target->base.x;
                s16 r0 = (brain->kirby->base.x - brain->target->base.x) >> 0xC;
                s16 sp04 = abs(r0);

                if (CpuRowIsClear(brain, sp04, b)) {
                    u16 r2 = brain->kirby->base.y >> 0xC;

                    if (sp04 < 2 || !r2
                        || (gCollisionAttributes[sub_080024F0(brain->levelInfo, brain->kirby->base.x >> 0xC, r2 - 1)] & 1)) {
                        if (brain->action != CPU_ACT_MISSILE_HOMING) {
                            brain->actArg = (u32)brain->target->base.x >> 8;
                            brain->actArg2 = (u32)brain->target->base.y >> 8;
                            brain->layers[CPU_LAYER_ACTION] = FN(CpuActMissileHoming);
                        }
                    }
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanMaster(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x3000, 0x1000);
    if (brain->target) {
        if (IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            if (~(brain->target->base.unk5C & ~7) & 0x7100 && (brain->target->base.unk5C & 7) < 4) {
                bool32 b = brain->kirby->base.x >= brain->target->base.x;

                if (brain->action != CPU_ACT_ATTACK) {
                    brain->actArg = b;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                }
                return;
            }
        }
        CpuTryHopOverTarget(brain, brain->target);
    }
}

void CpuScanUnderwater(struct CpuBrain *brain) {
    brain->target = CpuFindTarget(brain, 0x1800, 0x1800);
    if (brain->target) {
        if (~(brain->target->base.unk5C & ~7) & 0x40 && (brain->target->base.unk5C & 7) < 4) {
            s16 ret = sub_08154FE8((brain->target->base.x - brain->kirby->base.x) >> 8,
                (brain->target->base.y - brain->kirby->base.y) >> 8) & 0x3FF;

            if (ret >= 0x80 && ret < 0x180) {
                if (brain->action != CPU_ACT_WATER_ATTACK) {
                    brain->actArg = 0;
                    brain->actArg2 = 0xFFFF;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActWaterAttack);
                }
            } else if (ret >= 0x180 && ret < 0x280) {
                if (brain->action != CPU_ACT_WATER_ATTACK) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 2;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActWaterAttack);
                }
            } else if (ret >= 0x280 && ret < 0x380) {
                if (brain->action != CPU_ACT_WATER_ATTACK) {
                    brain->actArg = 1;
                    brain->actArg2 = 0xFFFF;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActWaterAttack);
                }
            } else {
                if (brain->action != CPU_ACT_WATER_ATTACK) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 3;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActWaterAttack);
                }
            }
        }
    }
}

/* PORT: CPU AI modes (cpu_ai_modes.c) -- the stock block-break check for
 * the Kirby's ability, the call CpuActDecide makes. */
bool16 CpuBreakBlockCheck(struct CpuBrain *brain) {
    return CALL(gCpuBreakBlockChecks[brain->kirby->ability])(brain);
}

bool16 CpuBreakBlockNormal(struct CpuBrain *brain) {
    s8 r8 = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 r2 = (brain->kirby->base.x >> 0xC) + r8;

    if (r2 >= 0 && r2 < brain->levelInfo->roomWidth >> 4) {
        s16 r7 = (brain->kirby->base.x + r8 * 0x800) >> 0xC;
        s16 r0 = (brain->kirby->base.y >> 0xC) - 1;
        u32 r1;
        const u32 *r4, *r4_2, *r4_3;

        if (r0 >= 0
            && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, r7, r0)]) & 0x1000
            && !(r1 & 0xF00000)) {
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        } else {
            s16 r1_2 = (brain->kirby->base.y >> 0xC) + 1;
            s16 r1_3;
            s16 r2_2;

            if (r1_2 < brain->levelInfo->roomHeight >> 4
                && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, r7, r1_2)]) & 0x1000) {
                if (!(r1 & 0xF00000)) {
                    if (Rand16() & 1) {
                        if (brain->action != CPU_ACT_ATTACK) {
                            brain->actArg = 0xFFFF;
                            brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                        }
                        return TRUE;
                    } else {
                        if (brain->action != CPU_ACT_SLIDE) {
                            brain->actArg = 0xFFFF;
                            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
                        }
                        return TRUE;
                    }
                } else if ((r1 & 0xF00000) == 0x100000) {
                    if (brain->action != CPU_ACT_SLIDE) {
                        brain->actArg = 0xFFFF;
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
                    }
                    return TRUE;
                }
            }
            r1_3 = (brain->kirby->base.x >> 0xC) + r8;
            r2_2 = brain->kirby->base.y >> 0xC;
            r4_3 = gCollisionAttributes;
            r1 = r4_3[sub_080024F0(brain->levelInfo, r1_3, r2_2)];
            if (r1 & 0x1000) {
                if (r1 & 0xF00000) {
                    if ((r1 & 0xF00000) == 0x100000) {
                        if (brain->action != CPU_ACT_SLIDE) {
                            brain->actArg = 0xFFFF;
                            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
                        }
                    } else {
                        return FALSE;
                    }
                } else {
                    if (Rand16() & 1) {
                        if (brain->action != CPU_ACT_ATTACK) {
                            brain->actArg = 0xFFFF;
                            brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
                        }
                    } else {
                        if (brain->action != CPU_ACT_SLIDE) {
                            brain->actArg = 0xFFFF;
                            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
                        }
                    }
                }
                return TRUE;
            }
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockFireIce(struct CpuBrain *brain) {
    s8 r8 = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 r7 = (brain->kirby->base.x + 0x1800 * r8) >> 0xC;
    s16 r2 = (brain->kirby->base.y >> 0xC) - 1;
    const u32 *r4, *r4_2, *r4_3;
    u32 r1;

    if (r7 >= 0
        && r7 < brain->levelInfo->roomWidth >> 4
        && r2 >= 0
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, r7, r2)]) & 0x1000
        && ((r1 & 0xF00000) == 0 || (r1 & 0xF00000) == 0x100000)) {
        if (brain->action != CPU_ACT_ATTACK) {
            brain->actArg = 0xFFFF;
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
        }
        return TRUE;
    }
    r2 = (brain->kirby->base.y >> 0xC) + 1;
    if (r7 >= 0
        && r7 < brain->levelInfo->roomWidth >> 4
        && r2 < brain->levelInfo->roomHeight >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, r7, r2)]) & 0x1000
        && ((r1 & 0xF00000) == 0 || (r1 & 0xF00000) == 0x100000)) {
        if (Rand16() & 1) {
            if (brain->action != CPU_ACT_SLIDE) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
            }
        } else {
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
        }
        return TRUE;
    }
    r7 = (brain->kirby->base.x >> 0xC) + r8;
    r2 = brain->kirby->base.y >> 0xC;
    if (r7 >= 0
        && r7 < brain->levelInfo->roomWidth >> 4
        && (r4_3 = gCollisionAttributes, r1 = r4_3[sub_080024F0(brain->levelInfo, r7, r2)]) & 0x1000
        && ((r1 & 0xF00000) == 0 || (r1 & 0xF00000) == 0x100000)) {
        if (brain->action != CPU_ACT_ATTACK) {
            brain->actArg = 0xFFFF;
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
        }
        return TRUE;
    }
    return FALSE;
}

bool16 CpuBreakBlockBurning(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b = brain->kirby->base.x >> 0xC;
    s16 c = (brain->kirby->base.y >> 0xC) - 1;
    const u32 *r4, *r4_2;
    u32 r1;

    if (c >= 0
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
        case 0x200000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    b = (brain->kirby->base.x >> 0xC) + a;
    c = brain->kirby->base.y >> 0xC;
    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
        case 0x200000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockWheel(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b = (brain->kirby->base.x >> 0xC) + a;
    s16 c = brain->kirby->base.y >> 0xC;
    const u32 *r4, *r4_2;
    u32 r1;

    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
        case 0x200000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    b = brain->kirby->base.x >> 0xC;
    c = (brain->kirby->base.y >> 0xC) + 1;
    if (c < brain->levelInfo->roomHeight >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
        case 0x200000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockParasol(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b = brain->kirby->base.x >> 0xC;
    s16 c = (brain->kirby->base.y >> 0xC) - 1;
    const u32 *r4, *r4_2, *r4_3;
    u32 r1;

    if (c >= 0
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    b = (brain->kirby->base.x >> 0xC) + a;
    c = brain->kirby->base.y >> 0xC;
    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    c = (brain->kirby->base.y >> 0xC) + 1;
    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && c < brain->levelInfo->roomHeight >> 4
        && (r4_3 = gCollisionAttributes, r1 = r4_3[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockShot(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b = (brain->kirby->base.x >> 0xC) + a;
    s16 c = brain->kirby->base.y >> 0xC;
    const u32 *r4;
    u32 r1;

    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockBeam(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b = (brain->kirby->base.x + 0x1800 * a) >> 0xC;
    s16 c = (brain->kirby->base.y >> 0xC) - 1;
    const u32 *r4, *r4_2, *r4_3;
    u32 r1;

    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && c >= 0
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    b = (brain->kirby->base.x >> 0xC) + a;
    c = brain->kirby->base.y >> 0xC;
    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    b = (brain->kirby->base.x + 0x1800 * a) >> 0xC;
    c = (brain->kirby->base.y >> 0xC) + 1;
    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && c < brain->levelInfo->roomHeight >> 4
        && (r4_3 = gCollisionAttributes, r1 = r4_3[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockStone(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b = brain->kirby->base.x >> 0xC;
    s16 c = (brain->kirby->base.y >> 0xC) - 1;
    const u32 *r4, *r4_2;
    u32 r1;

    if (c >= 0
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
        case 0x200000:
            if (brain->action != CPU_ACT_STONE_TOGGLE) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
            }
            return TRUE;
        }
    }
    c = (brain->kirby->base.y >> 0xC) + 1;
    if (c < brain->levelInfo->roomHeight >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
        case 0x200000:
            if (brain->action != CPU_ACT_STONE_TOGGLE) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockSparkSword(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b = brain->kirby->base.x >> 0xC;
    s16 c = (brain->kirby->base.y >> 0xC) - 1;
    const u32 *r4, *r4_2, *r4_3;
    u32 r1;

    if (c >= 0
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    b = (brain->kirby->base.x >> 0xC) + a;
    c = brain->kirby->base.y >> 0xC;
    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    b = brain->kirby->base.x >> 0xC;
    c = (brain->kirby->base.y >> 0xC) + 1;
    if (c < brain->levelInfo->roomHeight >> 4
        && (r4_3 = gCollisionAttributes, r1 = r4_3[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockCupid(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b = (brain->kirby->base.x >> 0xC) + a;
    s16 c = (brain->kirby->base.y >> 0xC) - 1;
    const u32 *r4, *r4_2, *r4_3;
    u32 r1;

    if (c >= 0
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_CUPID_SHOT) {
                brain->actArg = 0xFFFF;
                brain->actArg2 = 2;
                brain->actNext.data = 0;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActCupidShot);
            }
            return TRUE;
        }
    }
    b = (brain->kirby->base.x >> 0xC) + a;
    c = brain->kirby->base.y >> 0xC;
    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_CUPID_SHOT) {
                brain->actArg = 0xFFFF;
                brain->actArg2 = 0xFFFF;
                brain->actNext.data = 0;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActCupidShot);
            }
            return TRUE;
        }
    }
    b = (brain->kirby->base.x >> 0xC) + a;
    c = (brain->kirby->base.y >> 0xC) + 1;
    if (c < brain->levelInfo->roomHeight >> 4
        && (r4_3 = gCollisionAttributes, r1 = r4_3[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_CUPID_SHOT) {
                brain->actArg = 0xFFFF;
                brain->actArg2 = 3;
                brain->actNext.data = 0;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActCupidShot);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockHammer(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b = brain->kirby->base.x >> 0xC;
    s16 c = (brain->kirby->base.y >> 0xC) - 1;
    const u32 *r4, *r4_2, *r4_3;
    u32 r1;

    if (c >= 0
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
        case 0x200000:
        case 0x300000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    b = (brain->kirby->base.x >> 0xC) + a;
    c = brain->kirby->base.y >> 0xC;
    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
        case 0x200000:
        case 0x300000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    b = (brain->kirby->base.x + 0x1800 * a) >> 0xC;
    c = (brain->kirby->base.y >> 0xC) + 1;
    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && c < brain->levelInfo->roomHeight >> 4
        && (r4_3 = gCollisionAttributes, r1 = r4_3[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
        case 0x200000:
        case 0x300000:
            if (brain->action != CPU_ACT_ATTACK) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttack);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockThrow(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 d = (brain->kirby->base.x >> 0xC) + a;
    s16 b, c;
    const u32 *r4, *r4_2, *r4_3;
    u32 r1;

    if (d >= 0
        && d < brain->levelInfo->roomWidth >> 4) {
        b = (brain->kirby->base.x + 0x800 * a) >> 0xC;
        c = (brain->kirby->base.y >> 0xC) - 1;
        if (c >= 0
            && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
            switch (r1 & 0xF00000) {
            case 0:
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
                return TRUE;
            }
        }
    } else {
        return FALSE;
    }
    c = (brain->kirby->base.y >> 0xC) + 1;
    if (c < brain->levelInfo->roomHeight >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
            if (Rand16() & 1) {
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
            } else {
                if (brain->action != CPU_ACT_SLIDE) {
                    brain->actArg = 0xFFFF;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
                }
            }
            return TRUE;
        case 0x100000:
            if (brain->action != CPU_ACT_SLIDE) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
            }
            return TRUE;
        }
    }
    b = (brain->kirby->base.x >> 0xC) + a;
    c = brain->kirby->base.y >> 0xC;
    if ((r4_3 = gCollisionAttributes, r1 = r4_3[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
            if (Rand16() & 1) {
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
            } else {
                if (brain->action != CPU_ACT_SLIDE) {
                    brain->actArg = 0xFFFF;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
                }
            }
            return TRUE;
        case 0x100000:
            if (brain->action != CPU_ACT_SLIDE) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockMissile(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b, c;
    const u32 *r4, *r4_2, *r4_3;
    u32 r1;

    b = brain->kirby->base.x >> 0xC;
    c = (brain->kirby->base.y >> 0xC) - 1;
    if (c >= 0
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
        case 0x200000:
            if (brain->action != CPU_ACT_MISSILE) {
                brain->actArg = 0xFFFF;
                brain->actArg2 = 2;
                brain->actNext.data = 0x1E;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActMissile);
            }
            return TRUE;
        }
    }
    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4) {
        b = (brain->kirby->base.x >> 0xC) + a;
        c = brain->kirby->base.y >> 0xC;
        if ((r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
            switch (r1 & 0xF00000) {
            case 0:
            case 0x100000:
            case 0x200000:
                if (brain->action != CPU_ACT_MISSILE) {
                    brain->actArg = a <= 0;
                    brain->actArg2 = 0xFFFF;
                    brain->actNext.data = 0x1E;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActMissile);
                }
                return TRUE;
            }
        }
    } else {
        return FALSE;
    }
    b = (brain->kirby->base.x >> 0xC) + a;
    c = (brain->kirby->base.y >> 0xC) + 1;
    if (c < brain->levelInfo->roomHeight >> 4
        && (r4_3 = gCollisionAttributes, r1 = r4_3[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
            if (Rand16() & 1) {
                if (brain->action != CPU_ACT_SLIDE) {
                    brain->actArg = 0xFFFF;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
                }
            } else {
        case 0x100000:
        case 0x200000:
                if (brain->action != CPU_ACT_MISSILE) {
                    brain->actArg = a <= 0;
                    brain->actArg2 = 3;
                    brain->actNext.data = 0x1E;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActMissile);
                }
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool16 CpuBreakBlockTornado(struct CpuBrain *brain) {
    s8 a = brain->kirby->base.flags & 1 ? -1 : 1;
    s16 b = (brain->kirby->base.x >> 0xC) + a;
    s16 c = brain->kirby->base.y >> 0xC;
    const u32 *r4, *r4_2;
    u32 r1;

    if (b >= 0
        && b < brain->levelInfo->roomWidth >> 4
        && (r4 = gCollisionAttributes, r1 = r4[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_SLIDE) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
            }
            return TRUE;
        }
    }
    b = brain->kirby->base.x >> 0xC;
    c = (brain->kirby->base.y >> 0xC) + 1;
    if (c < brain->levelInfo->roomHeight >> 4
        && (r4_2 = gCollisionAttributes, r1 = r4_2[sub_080024F0(brain->levelInfo, b, c)]) & 0x1000) {
        switch (r1 & 0xF00000) {
        case 0:
        case 0x100000:
            if (brain->action != CPU_ACT_SLIDE) {
                brain->actArg = 0xFFFF;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
            }
            return TRUE;
        }
    }
    return FALSE;
}

void CpuScriptStep(struct CpuBrain *brain) {
    if (!(brain->flags & CPU_FLAG_FREE)) {
        if (!IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoam);
            brain->flags |= (CPU_FLAG_FREE | CPU_FLAG_SCRIPT_WAITING);
        } else if (!(brain->flags & CPU_FLAG_SCRIPT_WAITING)) {
            switch (brain->script[brain->scriptPc]) {
            case CPU_OP_NOP:
                CpuScriptOpNop(brain);
                break;
            case CPU_OP_END:
                CpuScriptOpEnd(brain);
                break;
            case CPU_OP_WAIT:
                CpuScriptOpWait(brain);
                break;
            case CPU_OP_GOTO_NODE:
                CpuScriptOpGoToNode(brain);
                break;
            case CPU_OP_SET_ABILITY:
                CpuScriptOpSetAbility(brain);
                break;
            case CPU_OP_FACE:
                CpuScriptOpFace(brain);
                break;
            case CPU_OP_INHALE:
                CpuScriptOpInhale(brain);
                break;
            case CPU_OP_SLIDE:
                CpuScriptOpSlide(brain);
                break;
            case CPU_OP_HALT:
                break;
            case CPU_OP_SKIP:
                CpuScriptOpSkip(brain);
                break;
            case CPU_OP_IF_ROOM_STATE:
                CpuScriptOpIfRoomState(brain);
                break;
            case CPU_OP_ROAM:
                CpuScriptOpRoam(brain);
                break;
            }
        }
    }
}

void CpuScriptSlidePrepare(struct CpuBrain *brain) {
    if (brain->kirby->base.flags & 0x80
        || !--brain->scriptArg) {
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlideSpitFirst);
    } else {
        switch (brain->kirby->ability) {
        default:
            if (brain->kirby->base.flags & 0x40)
                brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlideLandFirst);
            else if (brain->kirby->base.flags & 0x20)
                brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlideFallFirst);
            else
                brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlideStart);
            break;
        case KIRBY_ABILITY_SLEEP:
        case KIRBY_ABILITY_UFO:
        case KIRBY_ABILITY_MINI:
            brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlideStart);
            break;
        }
    }
}

void CpuScriptOpIfRoomState(struct CpuBrain *brain) {
    u16 r6 = *GetStateSlot(STATE_SLOT_ROOM, 0xF, brain->levelInfo->unk65E);
    bool32 a = FALSE;
    u8 r7;
    u16 b, c;

    ++brain->scriptPc;
    r7 = brain->script[brain->scriptPc];
    ++brain->scriptPc;
    b = brain->script[brain->scriptPc];
    ++brain->scriptPc;
    c = brain->script[brain->scriptPc];
    ++brain->scriptPc;
    switch (r7) {
    case 0:
        if (r6 == b)
            a = TRUE;
        break;
    case 1:
        if (r6 != b)
            a = TRUE;
        break;
    case 2:
        if (r6 > b)
            a = TRUE;
        break;
    case 3:
        if (r6 < b)
            a = TRUE;
        break;
    case 4:
        if (r6 >= b)
            a = TRUE;
        break;
    case 5:
        if (r6 <= b)
            a = TRUE;
        break;
    default:
        break;
    }
    if (a)
        brain->scriptPc += c;
}

void CpuGoalHoldBegin(struct CpuBrain *brain) {
    if (brain->levelInfo->currentRoom == 0x397)
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoom397Wait);
    else {
        brain->goalNode = 0xFF;
        brain->nextNode = 0xFF;
        brain->goalX = brain->kirby->base.x;
        brain->goalY = brain->kirby->base.y;
        brain->goalDx = 0;
        brain->goalDy = 0;
        brain->goalDxDyDiff = 0;
        brain->goalAngle = 0;
        brain->waypointX = brain->goalX;
        brain->waypointY = brain->goalY;
        brain->waypointDx = brain->goalDx;
        brain->waypointDy = brain->goalDy;
        brain->waypointDist = brain->goalDxDyDiff;
        brain->waypointAngle = brain->goalAngle;
        brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseIdle);
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalIdle);
    }
}

static inline const struct LevelInfo_1E8 *CpuNavGraph(struct CpuBrain *brain) {
    return brain->levelInfo->unk1E8;
}

void CpuGoalScriptNodeBegin(struct CpuBrain *brain) {
    const struct LevelInfo_1E8 *r5 = brain->levelInfo->unk1E8;
    u8 a;

    brain->flags &= ~CPU_FLAG_GOAL_REACHED;
    brain->goalTimer = 0x708;
    a = CpuNavNodeAt(&brain->kirby->base);
    if (a != brain->curNode)
        ++brain->nodeChanges;
    brain->curNode = a;
    brain->goalX = r5->unk14[brain->goalNode].unk20 << 8;
    brain->goalY = r5->unk14[brain->goalNode].unk22 << 8;
    brain->nextNode = CpuNavGraph(brain)->unk14[brain->curNode].unk0[brain->goalNode];
    CpuUpdateGoalVectors(brain);
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseMoving);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalScriptNodeTick);
}

void CpuGoalRoamBegin(struct CpuBrain *brain) {
    const struct LevelInfo_1E8 *r5;

    if (brain->curNode == 0xFF)
        brain->curNode = CpuNavNodeAt(&brain->kirby->base);
    r5 = brain->levelInfo->unk1E8;
    brain->goalNode = CpuPickRandomNode(brain->levelInfo, brain->curNode);
    brain->goalX = r5->unk14[brain->goalNode].unk20 << 8;
    brain->goalY = r5->unk14[brain->goalNode].unk22 << 8;
    brain->nextNode = CpuNavGraph(brain)->unk14[brain->curNode].unk0[brain->goalNode];
    CpuUpdateGoalVectors(brain);
    brain->goalTimer = (Rand16() & 0x3FF) + 0x2D0;
    brain->flags &= ~CPU_FLAG_GOAL_REACHED;
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseMoving);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoamTick);
}

void CpuGoalRoamTick(struct CpuBrain *brain) {
    u16 i;

    for (i = 0; i < PortHumanEnd(); i = PortHumanNext(i)) {
        if (gCurLevelInfo[i].currentRoom == brain->levelInfo->currentRoom)
            break;
    }
    if (i >= PortHumanEnd())
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalOffscreenBegin);
    else if (CpuUpdateFollow(brain))
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalFollowBegin);
    else if (!(brain->flags & CPU_FLAG_GOAL_REACHED)) {
        if (brain->goalTimer) --brain->goalTimer;
        if (!(brain->flags & CPU_FLAG_GOAL_REACHED)
            && (brain->curNode == brain->goalNode || brain->goalNode == 0xFF || !brain->goalTimer)) {
            brain->flags |= CPU_FLAG_GOAL_REACHED;
            brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoamPauseBegin);
        }
    }
}

void CpuGoalRoamPauseTick(struct CpuBrain *brain) {
    u16 i;

    for (i = 0; i < PortHumanEnd(); i = PortHumanNext(i)) {
        if (gCurLevelInfo[i].currentRoom == brain->levelInfo->currentRoom)
            break;
    }
    if (i >= PortHumanEnd())
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalOffscreenBegin);
    else if (CpuUpdateFollow(brain))
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalFollowBegin);
    else {
        if (!(brain->goalTimer & 0x1F))
            CpuUpdateGoalVectors(brain);
        if (!--brain->goalTimer)
            brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoamBegin);
    }
}

struct Kirby *CpuPickLeader(u8 slot) {
    u16 r1;
    u16 r2;
    u32 mask;
    u16 roomId = gCurLevelInfo[slot].currentRoom;
    const struct Unk_08D6CD0C *sl = gUnk_08D6CD0C[roomId];

    /* PORT: human slots -- r2 counts the humans (the r2-th person is slot
     * PortSlotOrder(r2)); identical with nobody joined (platform/localmp.c). */
    if (!gNumHumanPlayers)
        return NULL;
    for (r2 = 0; r2 < gNumHumanPlayers; ++r2) {
        if (gCurLevelInfo[PortSlotOrder(r2)].currentRoom != 0xFFFF) {
            const struct Unk_08D6CD0C *ptr = gUnk_08D6CD0C[gCurLevelInfo[PortSlotOrder(r2)].currentRoom];

            if (sl->unk46 == ptr->unk46) break;
        }
    }
    if (r2 < gNumHumanPlayers) {
        r1 = Rand16();
        mask = 3; // same issue as CpuPickRandomNode
        r2 = 0xFFFF;
        for (r1 &= mask; r1 != 0xFFFF; --r1) {
            while (1) {
                for (++r2; r2 < gNumHumanPlayers; ++r2) {
                    if (gCurLevelInfo[PortSlotOrder(r2)].currentRoom != 0xFFFF) {
                        const struct Unk_08D6CD0C *ptr = gUnk_08D6CD0C[gCurLevelInfo[PortSlotOrder(r2)].currentRoom];

                        if (sl->unk46 == ptr->unk46) break;
                    }
                }
                if (r2 < gNumHumanPlayers)
                    break;
                r2 = 0xFFFF;
            }
        }
    } else {
        r1 = Rand16();
        mask = 3; // same issue as CpuPickRandomNode
        r2 = 0xFFFF;
        for (r1 &= mask; r1 != 0xFFFF; --r1) {
            if (++r2 >= gNumHumanPlayers)
                r2 = 0;
        }
    }
    return gKirbys + PortSlotOrder(r2);
}

bool32 CpuWarpToRoom(struct CpuBrain *brain, u16 a2) {
    u8 a, b;
    const struct Unk_08D6CD0C *c, *d;
    u8 a_, b_;

    sub_08002D40(a2, &a, &b);
    a_ = a;
    b_ = b;
    c = gUnk_08D6CD0C[a2];
    d = gUnk_08D6CD0C[brain->levelInfo->currentRoom];
    if (sub_080551FC(brain->kirby, a2, a_, b_)) {
        if (d->unk46 == c->unk46)
            ++brain->areaWarpCount;
        else
            brain->areaWarpCount = 0;
        return TRUE;
    } else {
        return FALSE;
    }
}

u16 CpuPickAreaExit(struct CpuBrain *brain, u16 a2) {
    const struct Unk_08D6CD0C *b = gUnk_08D6CD0C[a2];
    const struct Unk_08D6DCAC *a = gUnk_08D6DCAC[b->unk46];
    u16 i;
    u16 r;
    u32 mask;

    for (i = 0; i < a->unk4 && a->unk0[2*i] != a2; ++i)
        ;
    if (i == a->unk4)
        return 0xFFFF;
    r = Rand16();
    mask = 0x1F;
    i = 0xFFFF;
    for (r &= mask; r != 0xFFFF; --r) {
        while (1) {
            for (++i; i < a->unk4; ++i) {
                if (a->unk0[2*i] == a2)
                    break;
            }
            if (i < a->unk4)
                break;
            i = 0xFFFF;
        }
    }
    return a->unk0[2*i + 1];
}

u16 CpuPickNeighborRoom(struct CpuBrain *brain, u16 a2) {
    const struct Unk_08D6CD0C *b = gUnk_08D6CD0C[a2];
    u16 i;
    u16 r;
    u32 mask;
    u32 var;

    for (i = 0; i < b->unk48; ++i) {
        const struct Unk_08D6CD0C *r3 = gUnk_08D6CD0C[b->unk0[i][0]];

        if (b->unk0[i][1] > 1 && !(r3->unk47 & 0x80))
            break;
    }
    if (i == b->unk48) {
        u16 ret = CpuPickAreaExit(brain, a2);

        if (ret != 0xFFFF)
            return ret;
        else
            return brain->homeRoom;
    }
    r = Rand16();
    mask = 0x1F;
    i = 0xFFFF;
    var = 0xFFFF; // permuter
    for (r &= mask; r != var; --r) {
        while (1) {
            for (++i; i < b->unk48; ++i) {
                if (b->unk0[i][1] > 1 && !(gUnk_08D6CD0C[b->unk0[i][0]]->unk47 & 0x80))
                    break;
            }
            if (i < b->unk48)
                break;
            i = 0xFFFF;
        }
    }
    return b->unk0[i][0];
}

u16 CpuNextRoomToward(struct CpuBrain *brain, u16 a2, u16 *a3) {
    const struct Unk_08D6DCAC *a;
    const struct Unk_08D6CD0C *b = gUnk_08D6CD0C[a2];
    const struct Unk_08D6CD0C *c;
    u16 i;
    u16 r;
    u16 r4 = 0;

    if (*a3 == 0xFFFF) {
        if (!brain->leader)
            brain->leader = &gKirbys[(brain->kirby->base.unk56 + 1) & 3];
        *a3 = gCurLevelInfo[brain->leader->base.unk56].currentRoom;
    }
    if (b->unk47 & 8) {
        for (i = 0; i < 8 && (a2 != gCpuLinkedRooms[i][0] || *a3 != gCpuLinkedRooms[i][1]); ++i)
            ;
        if (i == 8)
            *a3 = r4 = brain->homeRoom;
        else if (*GetStateSlot(STATE_SLOT_SESSION, i + 1, 0))
            *a3 = r4 = brain->homeRoom;
        else
            r4 = *a3;
    } else {
        if (b->unk47 & 0x87)
            *a3 = r4 = brain->homeRoom;
    }
    if (r4) return r4;
    for (i = 0; i < b->unk48 && b->unk0[i][0] != *a3; ++i)
        ;
    if (i != b->unk48 && b->unk0[i][1]) {
        r4 = b->unk0[i][3];
        return r4;
    }
    c = gUnk_08D6CD0C[*a3];
    if (b->unk46 != c->unk46) {
        a = gUnk_08D6DCAC[b->unk46];
        for (i = 0; i < a->unk4; ++i) {
            if (gUnk_08D6CD0C[a->unk0[2*i + 1]]->unk46 == c->unk46)
                break;
        }
        if (i < a->unk4) {
            if (a->unk0[2*i] == a2)
                r4 = a->unk0[2*i + 1];
            else {
                *a3 = a->unk0[2*i];
                r4 = a2;
            }
            return r4;
        }
        if (a->unk4) {
            r = (Rand16() & 0x1F) + 1;
            i = 0xFFFF;
            for (; r; --r) {
                if (++i >= a->unk4)
                    i = 0;
            }
            *a3 = a->unk0[2 * i];
        } else {
            *a3 = brain->homeRoom;
        }
        r4 = *a3;
    } else {
        *a3 = CpuPickNeighborRoom(brain, a2);
        if (brain->homeRoom == *a3)
            r4 = brain->homeRoom;
        else
            r4 = a2;
    }
    return r4;
}

static inline u16 CpuLinkedRoomIndex(u16 roomId) {
    u16 i;

    for (i = 0; i < 8; ++i) {
        if (roomId == gCpuLinkedRooms[i][0])
            return i;
    }
    return 0xFFFF;
}

static inline u16 CurrentRoom(struct LevelInfo *li) {
    return li->currentRoom;
}

bool32 CpuTryRoomHop(struct CpuBrain *brain) {
    if (brain->roomHopTimer) {
        --brain->roomHopTimer;
        if (brain->leader
            && gCurLevelInfo[brain->leader->base.unk56].currentRoom != 0xFFFF
            && brain->levelInfo->currentRoom == gCurLevelInfo[brain->leader->base.unk56].currentRoom)
            brain->flags |= CPU_FLAG_WITH_LEADER;
    } else {
        if (!IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
            bool32 r9 = TRUE;
            u16 r6 = brain->levelInfo->currentRoom;
            const struct Unk_08D6CD0C *r2 = gUnk_08D6CD0C[r6];

            if (r2->unk47 & 2) {
                if (!CpuWarpToRoom(brain, brain->homeRoom))
                    r9 = FALSE;
                brain->leader = CpuPickLeader(brain->kirby->base.unk56);
                brain->flags &= ~CPU_FLAG_WITH_LEADER;
            } else {
                if ((r6 == 0x323 || r6 == 0x324 || r6 == 0x325)
                    && !(gCurLevelInfo[0].currentRoom == 0x323 || gCurLevelInfo[0].currentRoom == 0x324 || gCurLevelInfo[0].currentRoom == 0x325)) {
                    const struct Unk_08D6CD0C *r4, *sl;
                    u8 a, b;

                    brain->leader = CpuPickLeader(brain->kirby->base.unk56);
                    brain->flags &= ~CPU_FLAG_WITH_LEADER;
                    brain->homeRoom = 0x321;
                    a = gUnk_082D8D08[KATAM_ROM_ROW(brain->kirby->base.unk56)][0] >> 0xC;
                    b = gUnk_082D8D08[KATAM_ROM_ROW(brain->kirby->base.unk56)][1] >> 0xC;
                    sl = gUnk_08D6CD0C[brain->homeRoom];
                    r4 = gUnk_08D6CD0C[CurrentRoom(brain->levelInfo)];
                    if (sub_080551FC(brain->kirby, 0x321, a, b)) {
                        if (r4->unk46 == sl->unk46)
                            ++brain->areaWarpCount;
                        else
                            brain->areaWarpCount = 0;
                    } else {
                        r9 = FALSE;
                    }
                } else {
                    if (r2->unk47 & 8) {
                        u16 i;
                        u16 r0 = CpuLinkedRoomIndex(brain->levelInfo->currentRoom);

                        for (i = 0; i < PortHumanEnd(); i = PortHumanNext(i)) {
                            if (gKirbys[i].base.roomId == gCpuLinkedRooms[r0][1])
                                break;
                        }
                        if (i < PortHumanEnd()) {
                            brain->leader = gKirbys + i;
                            brain->flags &= ~CPU_FLAG_WITH_LEADER;
                            brain->destRoom = gCurLevelInfo[brain->leader->base.unk56].currentRoom;
                            if (!CpuWarpToRoom(brain, CpuNextRoomToward(brain, r6, &brain->destRoom)))
                                r9 = FALSE;
                        }
                    } else {
                        if (r6 == brain->destRoom) {
                            if (brain->leader && gCurLevelInfo[brain->leader->base.unk56].currentRoom != 0xFFFF && !(brain->flags & CPU_FLAG_WITH_LEADER)) {
                                u16 r1;

                                brain->destRoom = gCurLevelInfo[brain->leader->base.unk56].currentRoom;
                                r1 = CpuNextRoomToward(brain, r6, &brain->destRoom);
                                if (r6 == r1)
                                    r9 = FALSE;
                                else if (brain->homeRoom == r1) {
                                    if (!CpuWarpToRoom(brain, r1))
                                        r9 = FALSE;
                                } else {
                                    if (!CpuWarpToRoom(brain, r1))
                                        r9 = FALSE;
                                }

                            } else {
                                brain->destRoom = CpuPickNeighborRoom(brain, r6);
                                brain->flags &= ~CPU_FLAG_WITH_LEADER;
                                if (brain->homeRoom == brain->destRoom
                                    && !CpuWarpToRoom(brain, brain->destRoom))
                                    r9 = FALSE;
                            }
                        } else {
                            u16 r1 = CpuNextRoomToward(brain, r6, &brain->destRoom);

                            if (brain->homeRoom == r1) {
                                if (!CpuWarpToRoom(brain, r1))
                                    r9 = FALSE;
                                brain->leader = CpuPickLeader(brain->kirby->base.unk56);
                                brain->flags &= ~CPU_FLAG_WITH_LEADER;
                            } else {
                                if (r6 != r1) {
                                    if (!CpuWarpToRoom(brain, r1))
                                        r9 = FALSE;
                                } else {
                                    r9 = FALSE;
                                    brain->leader = CpuPickLeader(brain->kirby->base.unk56);
                                }
                            }
                        }
                    }
                }
            }
            if (r9) {
                const struct Unk_08D6CD0C *r0 = gUnk_08D6CD0C[brain->levelInfo->currentRoom];
                u16 r1;

                if ((brain->levelInfo->currentRoom == 0x323 || brain->levelInfo->currentRoom == 0x324 || brain->levelInfo->currentRoom == 0x325)
                    || r0->unk47 & 0x83)
                    r1 = 0xB4;
                else if (r0->unk47 & 8)
                    r1 = (Rand16() & 0x7F) + 0x3C;
                else
                    r1 = (Rand16() & 0xFFF) + 0x2328;
                brain->roomHopTimer = r1;
            }
            return r9;
        } else {
            const struct Unk_08D6CD0C *r0 = gUnk_08D6CD0C[brain->levelInfo->currentRoom];
            u16 r1;

            if ((brain->levelInfo->currentRoom == 0x323 || brain->levelInfo->currentRoom == 0x324 || brain->levelInfo->currentRoom == 0x325)
                || r0->unk47 & 0x83)
                r1 = 0xB4;
            else if (r0->unk47 & 8)
                r1 = (Rand16() & 0x7F) + 0x3C;
            else
                r1 = (Rand16() & 0x7FF) + 0xE10;
            brain->roomHopTimer = r1;
        }
    }
    return FALSE;
}

void CpuGoalOffscreenBegin(struct CpuBrain *brain) {
    const struct Unk_08D6CD0C *r0 = gUnk_08D6CD0C[brain->levelInfo->currentRoom];
    u16 r1;

    if ((brain->levelInfo->currentRoom == 0x323 || brain->levelInfo->currentRoom == 0x324 || brain->levelInfo->currentRoom == 0x325)
        || r0->unk47 & 0x83)
        r1 = 0xB4;
    else if (r0->unk47 & 8)
        r1 = (Rand16() & 0x7F) + 0x3C;
    else
        r1 = (Rand16() & 0x7FF) + 0xE10;
    brain->roomHopTimer = r1;
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalOffscreenRoamBegin);
}

void CpuGoalOffscreenRoamBegin(struct CpuBrain *brain) {
    const struct LevelInfo_1E8 *r5;

    if (brain->curNode == 0xFF)
        brain->curNode = CpuNavNodeAt(&brain->kirby->base);
    r5 = brain->levelInfo->unk1E8;
    brain->goalNode = CpuPickRandomNode(brain->levelInfo, brain->curNode);
    brain->goalX = r5->unk14[brain->goalNode].unk20 << 8;
    brain->goalY = r5->unk14[brain->goalNode].unk22 << 8;
    brain->nextNode = CpuNavGraph(brain)->unk14[brain->curNode].unk0[brain->goalNode];
    CpuUpdateGoalVectors(brain);
    brain->goalTimer = (Rand16() & 0x3FF) + 0x2D0;
    brain->flags &= ~CPU_FLAG_GOAL_REACHED;
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseMoving);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalOffscreenRoamTick);
}

void CpuGoalOffscreenRoamTick(struct CpuBrain *brain) {
    u16 i;

    for (i = 0; i < PortHumanEnd(); i = PortHumanNext(i)) {
        if (gCurLevelInfo[i].currentRoom == brain->levelInfo->currentRoom) {
            brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoam);
            return;
        }
    }
    if (CpuTryRoomHop(brain)) {
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalOffscreenRoamBegin);
        return;
    }
    if (!(brain->flags & CPU_FLAG_GOAL_REACHED)) {
        if (brain->goalTimer) --brain->goalTimer;
        if (!(brain->flags & CPU_FLAG_GOAL_REACHED) && (brain->curNode == brain->nextNode || brain->goalNode == 0xFF || !brain->goalTimer)) {
            brain->flags |= CPU_FLAG_GOAL_REACHED;
            brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalOffscreenPauseBegin);
        }
    }
}

void CpuGoalOffscreenPauseTick(struct CpuBrain *brain) {
    u16 i;

    for (i = 0; i < PortHumanEnd(); i = PortHumanNext(i)) {
        if (gCurLevelInfo[i].currentRoom == brain->levelInfo->currentRoom) {
            brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoam);
            return;
        }
    }
    if (CpuTryRoomHop(brain))
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalOffscreenRoamBegin);
    else if (!--brain->goalTimer)
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalOffscreenRoamBegin);
}

u16 CpuFlyKeysToWaypoint(struct CpuBrain *brain) {
    u16 ret = 0;
    struct Kirby *kirby;

    if (brain->waypointAngle > 0x20 && brain->waypointAngle < 0x1E0)
        ret = 0x10;
    else if (brain->waypointAngle > 0x220 && brain->waypointAngle < 0x3E0)
        ret = 0x20;
    if (brain->waypointAngle > 0x120 && brain->waypointAngle < 0x2E0)
        ret |= 0x40;
    else if (brain->waypointAngle < 0xE0 || brain->waypointAngle > 0x320)
        ret |= 0x80;
    kirby = brain->kirby;
    if (ret & 0x20 && !(ret & 0x40) && !(ret & 0x80) && kirby->base.flags & 1 && kirby->base.unk62 & 1) {
        if (brain->waypointAngle > 0x100 && brain->waypointAngle < 0x300)
            ret |= 0x40;
        else
            ret |= 0x80;
    } else if (ret & 0x10 && !(ret & 0x40) && !(ret & 0x80) && !(kirby->base.flags & 1) && kirby->base.unk62 & 1) {
        if (brain->waypointAngle > 0x100 && brain->waypointAngle < 0x300)
            ret |= 0x40;
        else
            ret |= 0x80;
    } else if (ret & 0x40 && !(ret & 0x10) && !(ret & 0x20) && kirby->base.unk62 & 8) {
        if (brain->waypointAngle > 0x200)
            ret |= 0x20;
        else
            ret |= 0x10;
    } else if (ret & 0x80 && !(ret & 0x10) && !(ret & 0x20) && kirby->base.unk62 & 4) {
        if (brain->waypointAngle > 0x200)
            ret |= 0x20;
        else
            ret |= 0x10;
    }
    if (brain->kirby->base.xspeed > 0 && brain->terrainRow[3] == 1)
        ret = (brain->keys | 0x20) & ~0x10;
    else if (brain->kirby->base.xspeed < 0 && brain->terrainRow[1] == 1)
        ret = (brain->keys | 0x10) & ~0x20;
    if (brain->kirby->base.yspeed > 0 && brain->terrain[7] == 1)
        ret = (brain->keys | 0x80) & ~0x40;
    else if (brain->kirby->base.yspeed < 0 && (brain->terrainBelow[2] == 1 || brain->terrainBelow[7] == 0xFF))
        ret = (brain->keys | 0x40) & ~0x80;
    return ret;
}

u16 CpuSwimKeysToWaypoint(struct CpuBrain *brain) {
    u16 ret = 0;
    struct Kirby *kirby;

    if (brain->waypointAngle > 0x20 && brain->waypointAngle < 0x1E0)
        ret = 0x10;
    else if (brain->waypointAngle > 0x220 && brain->waypointAngle < 0x3E0)
        ret = 0x20;
    if (brain->waypointAngle > 0x120 && brain->waypointAngle < 0x2E0)
        ret |= 0x40;
    else if (brain->waypointAngle < 0xE0 || brain->waypointAngle > 0x320)
        ret |= 0x80;
    kirby = brain->kirby;
    if (ret & 0x20 && !(ret & 0x40) && !(ret & 0x80) && kirby->base.flags & 1 && kirby->base.unk62 & 1) {
        if (brain->waypointAngle > 0x100 && brain->waypointAngle < 0x300)
            ret |= 0x40;
        else
            ret |= 0x80;
    } else if (ret & 0x10 && !(ret & 0x40) && !(ret & 0x80) && !(kirby->base.flags & 1) && kirby->base.unk62 & 1) {
        if (brain->waypointAngle > 0x100 && brain->waypointAngle < 0x300)
            ret |= 0x40;
        else
            ret |= 0x80;
    } else if (ret & 0x40 && !(ret & 0x10) && !(ret & 0x20) && kirby->base.unk62 & 8) {
        if (brain->waypointAngle > 0x200)
            ret |= 0x20;
        else
            ret |= 0x10;
    } else if (ret & 0x80 && !(ret & 0x10) && !(ret & 0x20) && kirby->base.unk62 & 4) {
        if (brain->waypointAngle > 0x200)
            ret |= 0x20;
        else
            ret |= 0x10;
    }
    if (brain->kirby->base.xspeed > 0 && brain->terrainRow[3] == 1)
        ret = (brain->keys | 0x20) & ~0x10;
    else if (brain->kirby->base.xspeed < 0 && brain->terrainRow[1] == 1)
        ret = (brain->keys | 0x10) & ~0x20;
    if (brain->kirby->base.yspeed > 0 && brain->terrain[7] == 1)
        ret = (brain->keys | 0x80) & ~0x40;
    else if (brain->kirby->base.yspeed < 0 && brain->terrainBelow[2] == 1)
        ret = (brain->keys | 0x40) & ~0x80;
    return ret;
}

// clone of CpuSwimKeysToWaypoint
u16 CpuUfoKeysToWaypoint(struct CpuBrain *brain) {
    u16 ret = 0;
    struct Kirby *kirby;

    if (brain->waypointAngle > 0x20 && brain->waypointAngle < 0x1E0)
        ret = 0x10;
    else if (brain->waypointAngle > 0x220 && brain->waypointAngle < 0x3E0)
        ret = 0x20;
    if (brain->waypointAngle > 0x120 && brain->waypointAngle < 0x2E0)
        ret |= 0x40;
    else if (brain->waypointAngle < 0xE0 || brain->waypointAngle > 0x320)
        ret |= 0x80;
    kirby = brain->kirby;
    if (ret & 0x20 && !(ret & 0x40) && !(ret & 0x80) && kirby->base.flags & 1 && kirby->base.unk62 & 1) {
        if (brain->waypointAngle > 0x100 && brain->waypointAngle < 0x300)
            ret |= 0x40;
        else
            ret |= 0x80;
    } else if (ret & 0x10 && !(ret & 0x40) && !(ret & 0x80) && !(kirby->base.flags & 1) && kirby->base.unk62 & 1) {
        if (brain->waypointAngle > 0x100 && brain->waypointAngle < 0x300)
            ret |= 0x40;
        else
            ret |= 0x80;
    } else if (ret & 0x40 && !(ret & 0x10) && !(ret & 0x20) && kirby->base.unk62 & 8) {
        if (brain->waypointAngle > 0x200)
            ret |= 0x20;
        else
            ret |= 0x10;
    } else if (ret & 0x80 && !(ret & 0x10) && !(ret & 0x20) && kirby->base.unk62 & 4) {
        if (brain->waypointAngle > 0x200)
            ret |= 0x20;
        else
            ret |= 0x10;
    }
    if (brain->kirby->base.xspeed > 0 && brain->terrainRow[3] == 1)
        ret = (brain->keys | 0x20) & ~0x10;
    else if (brain->kirby->base.xspeed < 0 && brain->terrainRow[1] == 1)
        ret = (brain->keys | 0x10) & ~0x20;
    if (brain->kirby->base.yspeed > 0 && brain->terrain[7] == 1)
        ret = (brain->keys | 0x80) & ~0x40;
    else if (brain->kirby->base.yspeed < 0 && brain->terrainBelow[2] == 1)
        ret = (brain->keys | 0x40) & ~0x80;
    return ret;
}

static inline bool32 CpuIsHoldingThrow(struct CpuBrain *brain) {
    switch (brain->kirby->animationIndex) {
    case 0x70 ... 0x75:
        return TRUE;
    default:
        return FALSE;
    }
}

void CpuActThink(struct CpuBrain *brain) {
    bool32 b;

    brain->prevAction = brain->action;
    brain->action = CPU_ACT_THINK;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    if (brain->levelInfo->currentRoom == 0x397)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActRoom397);
    else {
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_STONE:
        case KIRBY_ABILITY_SMASH:
            if (brain->kirby->animationIndex == 0x34) {
                if (brain->action != CPU_ACT_STONE_TOGGLE)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
                return;
            }
            break;
        case KIRBY_ABILITY_THROW:
            if (CpuIsHoldingThrow(brain)) {
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
                return;
            }
            break;
        }
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_UFO:
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActDecide);
            return;
        case KIRBY_ABILITY_TORNADO:
            if (brain->kirby->base.sprite.animId == 0x13A && brain->kirby->base.sprite.variant == 1) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActDecide);
                return;
            }
            break;
        }
        if (brain->kirby->base.flags & 0x40)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActLeaveFlight);
        else
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActDecide);
    }
}

static inline bool32 CpuIsAboveLadder(struct CpuBrain *brain) {
    u16 var = (brain->kirby->base.y >> 0xC) + 1;

    if (var < brain->levelInfo->roomHeight >> 4
        && sub_08002434(brain->levelInfo, brain->kirby->base.x >> 0xC, var) == 0x20)
        return TRUE;
    else
        return FALSE;
}

static inline bool32 CpuShouldAnswerPhone(struct CpuBrain *brain) {
    struct Kirby *kirby;

    if (PortIsHuman(gUnk_02021580)
        && (kirby = &gKirbys[gUnk_02021580], gCurLevelInfo[kirby->base.unk56].currentRoom != brain->levelInfo->currentRoom)
        && !(kirby->base.unkC & 0x10000)
        && (gRoomProps + brain->levelInfo->currentRoom)->priorityFlags & 0x10)
        return TRUE;
    else
        return FALSE;
}

void CpuActDecide(struct CpuBrain *brain) {
    u16 v22;

    if (brain->flags & CPU_FLAG_FREE && IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom) && brain->nodeChanges > 0x10) {
        u16 array[] = { brain->kirby->base.x >> 0xC, brain->kirby->base.y >> 0xC };

        if (array[0] < brain->levelInfo->roomWidth >> 4
            && array[1] < brain->levelInfo->roomHeight >> 4
            && gCollisionAttributes[sub_08002434(brain->levelInfo, array[0], array[1])] & 0x4000) {
            struct Unk_0888562C_3 *unk = sub_080025AC(brain->kirby->base.unk56, array[0], array[1]).pat3;
            const struct Unk_08D6CD0C *a = gUnk_08D6CD0C[unk->unk08];

            if (!(a->unk47 & 0x83)) {
                u8 m = unk->unk0A, n = unk->unk0B;
                const struct Unk_08D6CD0C *b = gUnk_08D6CD0C[brain->levelInfo->currentRoom];

                if (sub_080551FC(brain->kirby, unk->unk08, m, n)) {
                    if (b->unk46 == a->unk46)
                        ++brain->areaWarpCount;
                    else
                        brain->areaWarpCount = 0;
                    return;
                }
            }
        }
    }
    if (((brain->terrainBelow[2] != 2 && brain->terrainBelow[7] == 1) || brain->terrainRow[2] == 0xFF)
        && brain->kirby->ability != KIRBY_ABILITY_UFO) {
        if (brain->kirby->base.unk58 & 2)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwim);
        else
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActFly);
        return;
    }
    if (CpuShouldAnswerPhone(brain)) {
        if (brain->action != CPU_ACT_ANSWER_PHONE)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActAnswerPhone);
        return;
    }
    if (brain->waypointDist > 0x20 && (brain->waypointAngle < 0xE0 || brain->waypointAngle > 0x320)) {

        if (brain->kirby->animationIndex == 0x2D || brain->kirby->animationIndex == 0x2E
            || CpuIsAboveLadder(brain)){
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActLadder);
            return;
        }
    }
    if (brain->wantAbility == 0xFF && brain->kirby->ability != KIRBY_ABILITY_NORMAL) {
        if (brain->action != CPU_ACT_DROP_ABILITY)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActDropAbility);
        return;
    }
    if (CALL(gCpuBreakBlockChecks[brain->kirby->ability])(brain))
        return;
    if ((brain->nodeChanges >> 2) & 1 && !IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom)) {
        if (brain->kirby->ability == KIRBY_ABILITY_NORMAL) {
            if (Rand16() < 0x400) {
                u8 ability = gCpuOffscreenAbilityPool[Rand16() % ARRAY_COUNT(gCpuOffscreenAbilityPool)];

                if (gUnk_0203AD10 & 4 && ability == KIRBY_ABILITY_MAGIC)
                    ability = KIRBY_ABILITY_FIRE;
                sub_0805BD4C(brain->kirby, ability);
            }
        } else {
            if (Rand16() < 0x100) {
                if (brain->action != CPU_ACT_DROP_ABILITY)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActDropAbility);
                return;
            }
        }
    }
    v22 = 0xFFFF;
    if (brain->nextNode != 0xFF)
        v22 = gUnk_08D63C28[gRoomProps[brain->levelInfo->currentRoom].unk22]->unk14[brain->nextNode].unk24;
    if (brain->curNode != brain->nextNode) {
        if (brain->nextNode == 0xFF)
            return;
    } else if (brain->curNode == 0xFF || v22 >= brain->waypointDist) {
        return;
    }
    if (brain->action != CPU_ACT_MOVE)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActMove);
}

void CpuActAnswerPhoneWait(struct CpuBrain *brain) {
    if (brain->actArg-- == 0) {
        if (CpuShouldAnswerPhone(brain))
            brain->keys = 0x100;
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActBackToThink);
    }
}

s8 CpuJumpNeeded(struct CpuBrain *brain) {
    u16 sp00 = brain->kirby->base.x >> 0xC;
    u16 r5 = brain->kirby->base.y >> 0xC;
    s16 a = brain->waypointAngle < 0x200 ? 1 : -1;
    u16 sp04 = sp00 + a;
    s32 r1;

    if (brain->terrainBelow[2] < 2) {
        if (((brain->waypointAngle > 0x100 && brain->waypointAngle < 0x300) || (Rand16() & 0xF) > 0xA)) {
            u8 ret = brain->waypointDist >> 4;

            if (ret) return ret;
        }
        if (brain->waypointAngle < 0xC0 || brain->waypointAngle > 0x340)
            return 0;
    }
    r1 = a + 2;
    if (brain->terrainBelow[r1] == 1 || brain->terrainRow[a + r1] == 1)
        return Rand16() & 7;
    if (!brain->terrainRow[r1])
        return 0;
    if (brain->levelInfo->roomWidth >> 4 > sp04) {
        u32 v8 = brain->levelInfo->roomHeight >> 4;
        u16 v9 = r5 - 1;
        u16 v10 = 1;

        while (v10 < 0x10) {
            if (v8 <= v9 || gCollisionAttributes[sub_080024F0(brain->levelInfo, sp00, v9)] & 0x200)
                return (Rand16() & 0xFF) < 5 ? v10 : -1;
            if (!(gCollisionAttributes[sub_080024F0(brain->levelInfo, sp04, v9)] & 0x200))
                return v10;
            ++v10;
            --v9;
        }
    }
    return -1;
}

void CpuActWalk(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_WALK;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->base.flags & 0x40)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->kirby->base.unk58 & 2)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->kirby->ability == KIRBY_ABILITY_STONE && brain->kirby->animationIndex == 0x34)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->kirby->ability == KIRBY_ABILITY_THROW && CpuIsHoldingThrow(brain))
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if ((brain->followKirby && brain->followKirby->base.flags & 0x10) || (brain->waypointDx < 0 || brain->waypointDx > 0x40))
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActDashFirstTap);
    else if (brain->kirby->base.flags & 0x10)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActWalkPauseBegin);
    else
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActWalkStride);
}

void CpuActWalkStride(struct CpuBrain *brain) {
    u32 var;

    if (brain->waypointDist < 0x20)
        var = (Rand16() & 7) + 4;
    else if (brain->waypointDist < 0x40)
        var = (Rand16() & 0xF) + 4;
    else
        var = (Rand16() & 0x1F) + 4;
    brain->actTimer = var;
    brain->actVar = brain->waypointAngle < 0x200 ? 0x10 : 0x20;
    brain->keys = brain->actVar;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActWalkTick);
}

static inline bool32 CpuIsOnLadder(struct CpuBrain *brain) {
    u16 var;

    if (brain->kirby->animationIndex == 0x2D || brain->kirby->animationIndex == 0x2E)
        return TRUE;
    var = (brain->kirby->base.y >> 0xC) + 1;
    if (var < brain->levelInfo->roomHeight >> 4
        && sub_08002434(brain->levelInfo, brain->kirby->base.x >> 0xC, var) == 0x20)
        return TRUE;
    else
        return FALSE;
}

void CpuActWalkTick(struct CpuBrain *brain) {
    if (brain->kirby->base.flags & 0x40)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->kirby->base.unk58 & 2)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->kirby->ability == KIRBY_ABILITY_STONE && brain->kirby->animationIndex == 0x34)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->kirby->ability == KIRBY_ABILITY_THROW && CpuIsHoldingThrow(brain))
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else {
        if (CpuShouldAnswerPhone(brain)) {
            if (brain->action != CPU_ACT_ANSWER_PHONE)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAnswerPhone);
            return;
        }
        if (brain->waypointDist < 0x50 && (brain->waypointDy < 0 || brain->waypointDy > 0x20)
            && brain->waypointAngle > 0x160 && brain->waypointAngle < 0x2A0) {
            u32 r1 = (u16)brain->waypointDy >> 4;

            if (r1 == 0)
                r1 = 1;
            if (brain->action != CPU_ACT_JUMP) {
                brain->actArg = r1;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActJump);
            }
            return;
        }
        if ((brain->waypointAngle > 0x160 && brain->waypointAngle < 0x2A0 && (brain->waypointDy < 0 || brain->waypointDy >= 0x50))
            || (brain->kirby->base.flags & 0x20 && (brain->terrainBelow[2] == 1 || brain->terrainBelow[7] == 0xFF))) {
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActFly);
            return;
        }
        if (brain->kirby->base.flags & 0x10
            && (!brain->followKirby || !(brain->followKirby->base.flags & 0x10))
            && (brain->waypointDx >= 0 && brain->waypointDx <= 0x40))
            brain->actVar &= ~0x30;
        if (brain->kirby->base.unk62 & 3)
            brain->actTimer = 0;
        brain->keys = brain->actVar;
        if (!(brain->kirby->base.flags & 0x20)) {
            s8 var = CpuJumpNeeded(brain);

            if (var > 3) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActFly);
                return;
            }
            if (var > 0) {
                if (brain->action != CPU_ACT_JUMP) {
                    brain->actArg = (u8)var;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActJump);
                }
                return;
            }
        }
        if (!brain->actTimer) {
            if (brain->flags & CPU_FLAG_FREE && IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom) && brain->nodeChanges > 0x10) {
                u16 array[] = { brain->kirby->base.x >> 0xC, brain->kirby->base.y >> 0xC };

                if (array[0] < brain->levelInfo->roomWidth >> 4
                    && array[1] < brain->levelInfo->roomHeight >> 4
                    && gCollisionAttributes[sub_08002434(brain->levelInfo, array[0], array[1])] & 0x4000) {
                    struct Unk_0888562C_3 *unk = sub_080025AC(brain->kirby->base.unk56, array[0], array[1]).pat3;
                    const struct Unk_08D6CD0C *a = gUnk_08D6CD0C[unk->unk08];

                    if (!(a->unk47 & 0x83)) {
                        u8 m = unk->unk0A, n = unk->unk0B;
                        const struct Unk_08D6CD0C *b = gUnk_08D6CD0C[brain->levelInfo->currentRoom];

                        if (sub_080551FC(brain->kirby, unk->unk08, m, n)) {
                            if (b->unk46 == a->unk46)
                                ++brain->areaWarpCount;
                            else
                                brain->areaWarpCount = 0;
                            return;
                        }
                    }
                }
            }
            if (brain->curNode == brain->goalNode && brain->goalDx < 0x30 && brain->goalDy < 0x20) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
                return;
            }
            if (brain->waypointAngle > 0x180 && brain->waypointAngle < 0x280
                && (brain->waypointDy < 0 || brain->waypointDy > 0x60)) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActFly);
                return;
            }
            if (brain->waypointDist > 0x20 && (brain->waypointAngle < 0xE0 || brain->waypointAngle > 0x320)) {
                if (CpuIsOnLadder(brain)) {
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActLadder);
                    return;
                }
            }
            if ((brain->waypointAngle < 0x3D || brain->waypointAngle > 0x3BF)
                && sub_080035F4(&brain->kirby->base)) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActHoldDown);
                brain->actArg = (Rand16() & 0x1F) + 0x3C;
            } else {
                if (!CALL(gCpuBreakBlockChecks[brain->kirby->ability])(brain))
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActWalkStride);
            }
        } else {
            --brain->actTimer;
        }
    }
}

void CpuActLadder(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_LADDER;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    brain->actTimer = 0;
    if (brain->kirby->animationIndex == 0x2D || brain->kirby->animationIndex == 0x2E)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActLadderStart);
    else if (!CpuIsAboveLadder(brain))
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActLadderGrab);
}

void CpuActLadderClimbTick(struct CpuBrain *brain) {
    if (brain->kirby->animationIndex != 0x2D && brain->kirby->animationIndex != 0x2E)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    brain->keys = brain->actVar;
    if (!brain->actTimer) {
        if (brain->actCount > 3)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        else
            ++brain->actCount;
        if (brain->goalDxDyDiff < 0x20
            && ((brain->waypointAngle > 0xE0 && brain->waypointAngle < 0x120) || (brain->waypointAngle > 0x2E0 && brain->waypointAngle < 0x320)))
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActLadderClimbBegin);
    } else {
        --brain->actTimer;
    }
}

void CpuActJump(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_JUMP;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    if (!brain->actArg || brain->kirby->base.flags & 0x60)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else {
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_STONE:
        case KIRBY_ABILITY_SMASH:
            if (brain->kirby->animationIndex == 0x34) {
                if (brain->action != CPU_ACT_STONE_TOGGLE)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
                return;
            }
            break;
        case KIRBY_ABILITY_THROW:
            if (CpuIsHoldingThrow(brain)) {
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
                return;
            }
            break;
        }
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActJumpBegin);
    }
}

void CpuActJumpRise(struct CpuBrain *brain) {
    brain->keys |= 1;
    brain->keys |= brain->waypointAngle < 0x200 ? 0x10 : 0x20;
    if (!--brain->actTimer) {
        switch (brain->actArg) {
        case 1:
            brain->actTimer = (Rand16() & 7) + 6;
            break;
        case 2:
            brain->actTimer = (Rand16() & 0xF) + 0xC;
            break;
        case 3:
            { /* PORT-FORK: RNG sequenced in agbcc's order (random.h) */
                u32 rnd = Rand16Twice();
                brain->actTimer = (rnd & 7) + 0xF + (rnd & 0xF);
            }
            break;
        default:
            brain->actTimer = (Rand16() & 0x1F) + 0x2C;
            break;
        }
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActJumpAir);
    }
}

void CpuActJumpAir(struct CpuBrain *brain) {
    if (brain->terrain[2] != 1)
        brain->keys |= 1;
    if (brain->actVar) {
        if (brain->waypointAngle > 0x20 && brain->waypointAngle < 0x200) {
            brain->keys |= 0x10;
            if (brain->kirby->base.flags & 1)
                --brain->actVar;
        } else if (brain->waypointAngle > 0x200 && brain->waypointAngle < 0x3E0) {
            brain->keys |= 0x20;
            if (!(brain->kirby->base.flags & 1))
                --brain->actVar;
        }
    } else {
        brain->keys |= brain->kirby->base.flags & 1 ? 0x20 : 0x10;
    }
    if (brain->kirby->base.flags & 0x20 && !(brain->kirby->base.flags & 0x40)) {
        if (brain->kirby->base.yspeed < 0) {
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActFly);
            return;
        }
    } else {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        return;
    }
    if (!CALL(gCpuBreakBlockChecks[brain->kirby->ability])(brain) && !--brain->actTimer) {
        if (brain->kirby->base.unk62 & 3)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActFly);
        else
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    }
}

void CpuActFly(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_FLY;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->base.flags & 0x40)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySteerBegin);
    else if (brain->kirby->base.flags & 0x80) {
        if (Rand16() & 1)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwallow);
        else
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSpit);
    } else if (brain->kirby->base.flags & 0x20) {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlyPuffBegin);
    } else {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlyFromGround);
    }
}

void CpuActFlySteerTick(struct CpuBrain *brain) {
    if (!(brain->kirby->base.flags & 0x40) || brain->kirby->base.unk62 & 4) {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        return;
    }
    switch (brain->kirby->ability) {
    case KIRBY_ABILITY_STONE:
    case KIRBY_ABILITY_SMASH:
        if (brain->kirby->animationIndex == 0x34) {
            if (brain->action != CPU_ACT_STONE_TOGGLE)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
            return;
        }
        break;
    case KIRBY_ABILITY_THROW:
        if (CpuIsHoldingThrow(brain)) {
            if (brain->action != CPU_ACT_THROW) {
                brain->actArg = 0xFFFF;
                brain->actArg2 = 0;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
            }
            return;
        }
        break;
    }
    if (CpuShouldAnswerPhone(brain)) {
        if (brain->action != CPU_ACT_ANSWER_PHONE)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActAnswerPhone);
        return;
    }
    brain->keys = brain->actVar;
    if (!--brain->actTimer) {
        if (brain->flags & CPU_FLAG_FREE && IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom) && brain->nodeChanges > 0x10) {
            u16 array[] = { brain->kirby->base.x >> 0xC, brain->kirby->base.y >> 0xC };

            if (array[0] < brain->levelInfo->roomWidth >> 4
                && array[1] < brain->levelInfo->roomHeight >> 4
                && gCollisionAttributes[sub_08002434(brain->levelInfo, array[0], array[1])] & 0x4000) {
                struct Unk_0888562C_3 *unk = sub_080025AC(brain->kirby->base.unk56, array[0], array[1]).pat3;
                const struct Unk_08D6CD0C *a = gUnk_08D6CD0C[unk->unk08];

                if (!(a->unk47 & 0x83)) {
                    u8 m = unk->unk0A, n = unk->unk0B;
                    const struct Unk_08D6CD0C *b = gUnk_08D6CD0C[brain->levelInfo->currentRoom];

                    if (sub_080551FC(brain->kirby, unk->unk08, m, n)) {
                        if (b->unk46 == a->unk46)
                            ++brain->areaWarpCount;
                        else
                            brain->areaWarpCount = 0;
                        return;
                    }
                }
            }
        }
        if (brain->kirby->base.unk58 & 2) {
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
            return;
        }
        if (brain->terrainBelow[7] != 1 && brain->terrainBelow[7] != 0xFF && (brain->goalAngle < 0x100 || brain->goalAngle > 0x300)) {
            if (brain->waypointAngle < 0x80 || brain->waypointAngle > 0x380) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
                return;
            }
            if (gCollisionAttributes[sub_080024F0(brain->levelInfo, brain->kirby->base.x >> 0xC, (brain->kirby->base.y >> 0xC) + 1)] & 2) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
                return;
            }
            if (brain->curNode == brain->goalNode && brain->goalDx < 0x40 && brain->goalDy < 0x20) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
                return;
            }
        }
        if (CpuShouldAnswerPhone(brain)) {
            if (brain->action != CPU_ACT_ANSWER_PHONE)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAnswerPhone);
            return;
        }
        if (!brain->actCount) {
            u16 unk9E = brain->keys;
            u8 unk62 = brain->kirby->base.unk62;

            if (unk9E & 0x40 && unk62 & 8) {
                brain->actTimer = 0x40;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySweepBegin);
                return;
            } else if (unk9E & 0x80 && unk62 & 4) {
                brain->actTimer = 0x80;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySweepBegin);
                return;
            } else if (unk9E & 0x20 && unk62 & 1) {
                brain->actTimer = 0x20;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySweepBegin);
                return;
            } else if (unk9E & 0x10 && unk62 & 1) {
                brain->actTimer = 0x10;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySweepBegin);
                return;
            }
        } else {
            --brain->actCount;
        }
        if (!CALL(gCpuBreakBlockChecks[brain->kirby->ability])(brain))
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySteerLeg);
    }
}

void CpuActFlySweepBegin(struct CpuBrain *brain) {
    switch (brain->actTimer) {
    case 0x10:
    case 0x20:
        brain->actVar = brain->kirby->base.yspeed <= 0;
        break;
    case 0x40:
    case 0x80:
        brain->actVar = brain->kirby->base.xspeed <= 0;
        break;
    }
    brain->actCount = (Rand16() & 1) + 2;
    brain->actTimer2 = (Rand16() & 0xF) + 4;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySweepTick);
}

void CpuActFlySweepTick(struct CpuBrain *brain) {
    if (!(brain->kirby->base.flags & 0x40) || brain->kirby->base.unk62 & 4) {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        return;
    }
    switch (brain->kirby->ability) {
    case KIRBY_ABILITY_STONE:
    case KIRBY_ABILITY_SMASH:
        if (brain->kirby->animationIndex    == 0x34) {
            if (brain->action != CPU_ACT_STONE_TOGGLE)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
            return;
        }
        break;
    case KIRBY_ABILITY_THROW:
        if (CpuIsHoldingThrow(brain)) {
            if (brain->action != CPU_ACT_THROW) {
                brain->actArg = 0xFFFF;
                brain->actArg2 = 0;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
            }
            return;
        }
        break;
    }
    if (CpuShouldAnswerPhone(brain)) {
        if (brain->action != CPU_ACT_ANSWER_PHONE)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActAnswerPhone);
        return;
    }
    switch (brain->actTimer) {
    case 0x10:
    case 0x20:
        brain->keys |= brain->actTimer | (brain->actVar ? 0x80 : 0x40);
        if (!--brain->actTimer2) {
            brain->actTimer2 = (Rand16() & 0xF) + 4;
            if ((brain->actTimer == 0x20 && !brain->terrainRow[1])
                || (brain->actTimer == 0x10 && !brain->terrainRow[3]))
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySteerBegin);
            else {
                if (brain->actVar) {
                    if (!brain->terrainBelow[2]) {
                        while (0) ; // required for matching
                        break;
                    }
                } else if (!brain->terrain[7]) {
                    break;
                }
                if (brain->actCount-- == 0)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
                else
                    brain->actVar ^= 1;
            }
        }
        break;
    case 0x40:
    case 0x80:
        brain->keys |= brain->actTimer | (brain->actVar ? 0x20 : 0x10);
        if (!--brain->actTimer2) {
            brain->actTimer2 = (Rand16() & 0xF) + 4;
            if ((brain->actTimer == 0x40 && !brain->terrain[7])
                || (brain->actTimer == 0x80 && !brain->terrainBelow[2]))
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySteerBegin);
            else {
                if (brain->actVar) {
                    if (!brain->terrainRow[1]) {
                        while (0) ; // required for matching
                        break;
                    }
                } else if (!brain->terrainRow[3]) {
                    break;
                }
                if (brain->actCount-- == 0)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
                else
                    brain->actVar ^= 1;
            }
        }
        break;
    }
}

void CpuActSwimTick(struct CpuBrain *brain) {
    brain->keys = brain->actVar;
    if (!--brain->actTimer) {
        if (brain->flags & CPU_FLAG_FREE && IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom) && brain->nodeChanges > 0x10) {
            u16 array[] = { brain->kirby->base.x >> 0xC, brain->kirby->base.y >> 0xC };

            if (array[0] < brain->levelInfo->roomWidth >> 4
                && array[1] < brain->levelInfo->roomHeight >> 4
                && gCollisionAttributes[sub_08002434(brain->levelInfo, array[0], array[1])] & 0x4000) {
                struct Unk_0888562C_3 *unk = sub_080025AC(brain->kirby->base.unk56, array[0], array[1]).pat3;
                const struct Unk_08D6CD0C *a = gUnk_08D6CD0C[unk->unk08];

                if (!(a->unk47 & 0x83)) {
                    u8 m = unk->unk0A, n = unk->unk0B;
                    const struct Unk_08D6CD0C *b = gUnk_08D6CD0C[brain->levelInfo->currentRoom];

                    if (sub_080551FC(brain->kirby, unk->unk08, m, n)) {
                        if (b->unk46 == a->unk46)
                            ++brain->areaWarpCount;
                        else
                            brain->areaWarpCount = 0;
                        return;
                    }
                }
            }
        }
        if (brain->terrainBelow[7] != 1 && (brain->goalAngle < 0x100 || brain->goalAngle > 0x300)) {
            if (brain->curNode == brain->goalNode && brain->goalDx < 0x40 && brain->goalDy < 0x20) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
                return;
            }
        }
        if (!(brain->kirby->base.unk58 & 2)) {
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActFly);
            return;
        }
        if (CpuShouldAnswerPhone(brain)) {
            if (brain->action != CPU_ACT_ANSWER_PHONE)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAnswerPhone);
            return;
        }
        if ((brain->waypointAngle < 0x3D || brain->waypointAngle >= 0x3C0)
            && sub_080035F4(&brain->kirby->base)) {
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActHoldDown);
            brain->actArg = (Rand16() & 0x1F) + 0x3C;
            return;
        }
        if (!CALL(gCpuBreakBlockChecks[brain->kirby->ability])(brain))
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwimLeg);
    }
}

void CpuActUfoMoveTick(struct CpuBrain *brain) {
    if (brain->kirby->ability != KIRBY_ABILITY_UFO) {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        return;
    }
    brain->keys = brain->actVar;
    if (!--brain->actTimer) {
        if (brain->flags & CPU_FLAG_FREE && IsOneOfTheCurrentRooms(brain->levelInfo->currentRoom) && brain->nodeChanges > 0x10) {
            u16 array[] = { brain->kirby->base.x >> 0xC, brain->kirby->base.y >> 0xC };

            if (array[0] < brain->levelInfo->roomWidth >> 4
                && array[1] < brain->levelInfo->roomHeight >> 4
                && gCollisionAttributes[sub_08002434(brain->levelInfo, array[0], array[1])] & 0x4000) {
                struct Unk_0888562C_3 *unk = sub_080025AC(brain->kirby->base.unk56, array[0], array[1]).pat3;
                const struct Unk_08D6CD0C *a = gUnk_08D6CD0C[unk->unk08];

                if (!(a->unk47 & 0x83)) {
                    u8 m = unk->unk0A, n = unk->unk0B;
                    const struct Unk_08D6CD0C *b = gUnk_08D6CD0C[brain->levelInfo->currentRoom];

                    if (sub_080551FC(brain->kirby, unk->unk08, m, n)) {
                        if (b->unk46 == a->unk46)
                            ++brain->areaWarpCount;
                        else
                            brain->areaWarpCount = 0;
                        return;
                    }
                }
            }
        }
        if (brain->terrainRow[2] == 1 && brain->goalDxDyDiff < 0x20) {
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
            return;
        }
        if (CpuShouldAnswerPhone(brain)) {
            if (brain->action != CPU_ACT_ANSWER_PHONE)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAnswerPhone);
            return;
        }
        if (!CALL(gCpuBreakBlockChecks[brain->kirby->ability])(brain))
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoMoveLeg);
    }
}

void CpuActSlide(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_SLIDE;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->base.flags & 0x60)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->kirby->base.flags & 0x80) {
        if (brain->action != CPU_ACT_SPIT) {
            brain->actArg = 0xFFFF;
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSpit);
        }
    } else {
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_STONE:
        case KIRBY_ABILITY_SMASH:
            if (brain->kirby->animationIndex == 0x34) {
                if (brain->action != CPU_ACT_STONE_TOGGLE)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
                return;
            }
            break;
        case KIRBY_ABILITY_THROW:
            if (CpuIsHoldingThrow(brain)) {
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
                return;
            }
            break;
        }
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_SLEEP:
        case KIRBY_ABILITY_UFO:
        case KIRBY_ABILITY_MINI:
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
            break;
        default:
            if (brain->actArg != 0xFFFF) {
                brain->actVar = brain->actArg;
                if (brain->action != CPU_ACT_TURN) {
                    brain->actArg = brain->actArg; // required for matching
                    brain->actNext.ptr = FN(CpuActSlideAfterTurn);
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
                }
            } else {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlideBegin);
            }
            break;
        }
    }
}

void CpuActAirPuff(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_AIR_PUFF;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (!(brain->kirby->base.flags & 0x40))
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else {
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_STONE:
        case KIRBY_ABILITY_SMASH:
            if (brain->kirby->animationIndex == 0x34) {
                if (brain->action != CPU_ACT_STONE_TOGGLE)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
                return;
            }
            break;
        case KIRBY_ABILITY_THROW:
            if (CpuIsHoldingThrow(brain)) {
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
                return;
            }
            break;
        }
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_SLEEP:
        case KIRBY_ABILITY_UFO:
        case KIRBY_ABILITY_CUPID:
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
            break;
        default:
            if (brain->actArg != 0xFFFF) {
                brain->actVar = brain->actArg;
                if (brain->action != CPU_ACT_TURN) {
                    brain->actArg = brain->actArg; // required for matching
                    brain->actNext.ptr = FN(CpuActAirPuffAfterTurn);
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
                }
            } else {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActAirPuffBegin);
            }
            break;
        }
    }
}

void CpuActSpit(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_SPIT;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (!(brain->kirby->base.flags & 0x80))
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else {
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_STONE:
        case KIRBY_ABILITY_SMASH:
            if (brain->kirby->animationIndex == 0x34) {
                if (brain->action != CPU_ACT_STONE_TOGGLE)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
                return;
            }
            break;
        case KIRBY_ABILITY_THROW:
            if (CpuIsHoldingThrow(brain)) {
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
                return;
            }
            break;
        }
        if (brain->actArg != 0xFFFF) {
            brain->actVar = brain->actArg;
            if (brain->action != CPU_ACT_TURN) {
                brain->actArg = brain->actArg; // required for matching
                brain->actNext.ptr = FN(CpuActSpitAfterTurn);
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
            }
        } else {
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActSpitBegin);
        }
    }
}

void CpuActInhale(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_INHALE;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->base.flags & 0x80)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else {
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_STONE:
        case KIRBY_ABILITY_SMASH:
            if (brain->kirby->animationIndex == 0x34) {
                if (brain->action != CPU_ACT_STONE_TOGGLE)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
                return;
            }
            break;
        case KIRBY_ABILITY_THROW:
            if (CpuIsHoldingThrow(brain)) {
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
                return;
            }
            break;
        }
        if (brain->actArg != 0xFFFF) {
            brain->actVar = brain->actArg;
            brain->actCount = brain->actArg2;
            if (brain->action != CPU_ACT_TURN) {
                brain->actArg = brain->actArg; // required for matching
                brain->actNext.ptr = FN(CpuActInhaleAfterTurn);
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
            }
        } else {
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActInhaleBegin);
        }
    }
}

void CpuActInhaleHold(struct CpuBrain *brain) {
    bool32 var = FALSE;

    brain->keys |= 2;
    if (++brain->actVar > 0x1E && brain->kirby->animationIndex == 0)
        var = TRUE;
    else if (!--brain->actTimer)
        var = TRUE;
    else if (brain->kirby->base.flags & 0x80)
        var = TRUE;
    if (var) {
        brain->swallowCooldown = (Rand16() & 0x7F) + 0x1E;
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    }
}

void CpuActSwallow(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_SWALLOW;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    if (!(brain->kirby->base.flags & 0x80)
        || (brain->kirby->base.flags & 0x20))
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else {
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_STONE:
        case KIRBY_ABILITY_SMASH:
            if (brain->kirby->animationIndex == 0x34) {
                if (brain->action != CPU_ACT_STONE_TOGGLE)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
                return;
            }
            break;
        case KIRBY_ABILITY_THROW:
            if (CpuIsHoldingThrow(brain)) {
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
                return;
            }
            break;
        }
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwallowBegin);
    }
}

void CpuActAttack(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_ATTACK;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    switch (brain->kirby->ability) {
    case KIRBY_ABILITY_STONE:
    case KIRBY_ABILITY_SMASH:
        if (brain->kirby->animationIndex == 0x34) {
            if (brain->action != CPU_ACT_STONE_TOGGLE)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
            return;
        }
        break;
    case KIRBY_ABILITY_THROW:
        if (CpuIsHoldingThrow(brain)) {
            if (brain->action != CPU_ACT_THROW) {
                brain->actArg = 0xFFFF;
                brain->actArg2 = 0;
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
            }
            return;
        }
        break;
    }
    if (brain->actArg != 0xFFFF) {
        brain->actVar = brain->actArg;
        if (brain->action != CPU_ACT_TURN) {
            brain->actArg = brain->actArg; // required for matching
            brain->actNext.ptr = FN(CpuActAttackAfterTurn);
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
        }
    } else {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttackBegin);
    }
}

void CpuActWaterAttack(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_WATER_ATTACK;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (!(brain->kirby->base.unk58 & 2))
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else {
        switch (brain->kirby->ability) {
        case KIRBY_ABILITY_STONE:
        case KIRBY_ABILITY_SMASH:
            if (brain->kirby->animationIndex == 0x34) {
                if (brain->action != CPU_ACT_STONE_TOGGLE)
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggle);
                return;
            }
            break;
        case KIRBY_ABILITY_THROW:
            if (CpuIsHoldingThrow(brain)) {
                if (brain->action != CPU_ACT_THROW) {
                    brain->actArg = 0xFFFF;
                    brain->actArg2 = 0;
                    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrow);
                }
                return;
            }
            break;
        }
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActWaterAttackBegin);
    }
}

void CpuActWaterAttackHold(struct CpuBrain *brain) {
    brain->keys |= 2;
    if (brain->actArg == 1)
        brain->keys |= 0x20;
    else if (brain->actArg == 0)
        brain->keys |= 0x10;
    if (brain->actArg2 == 2)
        brain->keys |= 0x40;
    else if (brain->actArg2 == 3)
        brain->keys |= 0x80;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActTurnTick(struct CpuBrain *brain) {
    bool32 r4 = FALSE;

    if (brain->actArg == 1) {
        if (!(brain->kirby->base.flags & 1))
            brain->keys |= 0x20;
        else
            r4 = TRUE;
    } else if (brain->actArg == 0) {
        if (brain->kirby->base.flags & 1)
            brain->keys |= 0x10;
        else
            r4 = TRUE;
    } else {
        r4 = TRUE;
    }
    if (!--brain->actTimer)
        r4 = TRUE;
    if (r4) {
        if (!brain->actNext.ptr)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        else
            brain->layers[CPU_LAYER_ACTION] = brain->actNext.ptr;
    }
}

void CpuActBomb(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_BOMB;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->ability != KIRBY_ABILITY_BOMB)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else {
        if (brain->actArg != 0xFFFF) {
            brain->actVar = brain->actArg;
            if (brain->action != CPU_ACT_TURN) {
                brain->actArg = brain->actArg; // required for matching
                brain->actNext.ptr = FN(CpuActBombAfterTurn);
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
            }
            return;
        }
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActBombDraw);
    }
}

void CpuActStoneDropJump(struct CpuBrain *brain) {
    if (brain->terrain[2] != 1)
        brain->keys |= 1;
    if (brain->actArg == 1)
        brain->keys |= 0x20;
    else if (brain->actArg == 0)
        brain->keys |= 0x10;
    else
        brain->keys |= brain->kirby->base.flags & 1 ? 0x20 : 0x10;
    if (brain->kirby->ability != KIRBY_ABILITY_STONE)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->kirby->base.flags & 0x40)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else {
        if (!brain->actTimer) {
            if (!(brain->kirby->base.flags & 0x20))
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
            else {
                s16 a = brain->kirby->base.y >> 0xC;
                s32 b;

                if (a < brain->levelInfo->roomHeight >> 4
                    && !(gCollisionAttributes[sub_080024F0(brain->levelInfo, brain->kirby->base.x >> 0xC, a)] & 0x200)) {
#if KATAM_WIDE
                    /* PORT: the wide build reaches this with no target (the
                     * console reads BIOS memory through NULL here; a native
                     * host faults).  No target: as if far away. */
                    if (brain->target == NULL)
                        b = 0x10;
                    else
#endif
                    b = ((brain->kirby->base.x - brain->target->base.x) * 0x100) >> 0x10;
                    if (b < 0) b = -b;
                    if (b < 0x10)
                        brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropPress);
                }
            }
        } else {
            --brain->actTimer;
        }
    }
}

void CpuActStoneDropAim(struct CpuBrain *brain) {
    if (brain->actArg == 1)
        brain->keys |= 0x20;
    else if (brain->actArg == 0)
        brain->keys |= 0x10;
    else
        brain->keys |= brain->kirby->base.flags & 1 ? 0x20 : 0x10;
    {
        s16 a = brain->kirby->base.y >> 0xC;
        s32 b;

        if (a < brain->levelInfo->roomHeight >> 4
            && !(gCollisionAttributes[sub_080024F0(brain->levelInfo, brain->kirby->base.x >> 0xC, a)] & 0x200)) {
            b = ((brain->kirby->base.x - brain->target->base.x) * 0x100) >> 0x10;
            if (b < 0) b = -b;
            if (b < 0x10) {
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropPress);
                return;
            }
        }
        if (brain->kirby->base.flags & 0x40 || !(brain->kirby->base.flags & 0x20))
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    }
}

void CpuActThrow(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_THROW;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->ability != KIRBY_ABILITY_THROW)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (CpuIsHoldingThrow(brain))
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowAim);
    else if (brain->actArg != 0xFFFF) {
        brain->actVar = brain->actArg;
        brain->actCount = brain->actArg2;
        if (brain->action != CPU_ACT_TURN) {
            brain->actArg = brain->actArg; // required for matching
            brain->actNext.ptr = FN(CpuActThrowAfterTurn);
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
        }
    } else {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowBegin);
    }
}

void CpuActThrowGrab(struct CpuBrain *brain) {
    bool32 r4 = FALSE, r5 = FALSE;

    brain->keys |= 2;
    if (CpuIsHoldingThrow(brain))
        r5 = TRUE;
    if ((++brain->actVar > 0x1E && !brain->kirby->animationIndex))
        r4 = TRUE;
    else if (!--brain->actTimer)
        r4 = TRUE;
    if (r5)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowHoldBegin);
    else if (r4)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActThrowAim(struct CpuBrain *brain) {
    u16 r;

    if (brain->kirby->ability != KIRBY_ABILITY_THROW)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else {
        brain->actTimer = 5;
        brain->actVar = 0;
        switch (Rand16() & 3) {
        case 0:
            brain->actVar = 0x40;
            break;
        case 1:
            brain->actVar = 0x80;
            break;
        }
        r = Rand16() & 3;
        if (r < 2)
            brain->actVar |= 0x20;
        else if (r < 4) // always true
            brain->actVar |= 0x10;
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowAimHold);
    }
}

void CpuActUfoShot(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_UFO_SHOT;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->ability != KIRBY_ABILITY_UFO)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->actArg != 0xFFFF) {
        brain->actVar = brain->actArg;
        brain->actCount = brain->actArg2;
        if (brain->action != CPU_ACT_TURN) {
            brain->actArg = brain->actArg; // required for matching
            brain->actNext.ptr = FN(CpuActUfoShotAfterTurn);
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
        }
    } else {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoShotBegin);
    }
}

void CpuActCupidShot(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_CUPID_SHOT;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->ability != KIRBY_ABILITY_CUPID)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->actArg != 0xFFFF) {
        brain->actVar = brain->actArg;
        brain->actCount = brain->actArg2;
        brain->actTimer2 = brain->actNext.data;
        if (brain->action != CPU_ACT_TURN) {
            brain->actArg = brain->actArg; // required for matching
            brain->actNext.ptr = FN(CpuActCupidShotAfterTurn);
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
        }
    } else {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActCupidShotBegin);
    }
}

void CpuActCupidShotCharge(struct CpuBrain *brain) {
    switch (brain->actArg2) {
    case 2:
        brain->keys |= 0x40;
        break;
    case 3:
        brain->keys |= 0x80;
        break;
    }
    if (brain->actTimer > 5)
        brain->keys |= 2;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActMissile(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_MISSILE;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->ability != KIRBY_ABILITY_MISSILE)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->actArg != 0xFFFF) {
        brain->actVar = brain->actArg;
        brain->actCount = brain->actArg2;
        brain->actTimer2 = brain->actNext.data;
        if (brain->action != CPU_ACT_TURN) {
            brain->actArg = brain->actArg; // required for matching
            brain->actNext.ptr = FN(CpuActMissileAfterTurn);
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
        }
    } else {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActMissileBegin);
    }
}

void CpuActMissileLaunch(struct CpuBrain *brain) {
    if (brain->actArg == 1)
        brain->keys |= 0x20;
    else if (brain->actArg == 0)
        brain->keys |= 0x10;
    if (brain->actArg2 == 2)
        brain->keys |= 0x40;
    else if (brain->actArg2 == 3)
        brain->keys |= 0x80;
    brain->keys |= 2;
    if (++brain->actTimer > 5)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActMissileFly);
}

void CpuActMissileFly(struct CpuBrain *brain) {
    if (brain->actArg == 1)
        brain->keys |= 0x20;
    else if (brain->actArg == 0)
        brain->keys |= 0x10;
    if (brain->actArg2 == 2)
        brain->keys |= 0x40;
    else if (brain->actArg2 == 3)
        brain->keys |= 0x80;
    if (++brain->actTimer > brain->actNext.data) {
        brain->keys |= 2;
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    }
}

void CpuActMissileHomingLaunch(struct CpuBrain *brain) {
    u16 ret = sub_08154FE8(brain->actArg - (brain->kirby->base.x >> 8), brain->actArg2 - (brain->kirby->base.y >> 8)) & 0x3FF;

    brain->keys |= 2;
    if (ret > 0x200)
        brain->keys |= 0x20;
    else
        brain->keys |= 0x10;
    if (ret > 0x100 && ret < 0x300)
        brain->keys |= 0x40;
    else
        brain->keys |= 0x80;
    if (++brain->actTimer > 5)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActMissileHomingSteer);
}

void CpuActMissileHomingSteer(struct CpuBrain *brain) {
    s16 a = brain->kirby->base.x >> 8;
    s16 b = brain->kirby->base.y >> 8;
    u16 ret = sub_08154FE8(brain->actArg - a, brain->actArg2 - b) & 0x3FF;
    s16 dx = a > brain->actArg ? a - brain->actArg : brain->actArg - a;
    s16 dy = b > brain->actArg2 ? b - brain->actArg2 : brain->actArg2 - b;
    s16 max = dy < dx ? dx : dy;

    if (ret > 0x200)
        brain->keys |= 0x20;
    else
        brain->keys |= 0x10;
    if (ret > 0x100 && ret < 0x300)
        brain->keys |= 0x40;
    else
        brain->keys |= 0x80;
    if (++brain->actTimer > 0x12C || max < 0xC) {
        brain->keys |= 2;
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    }
}

void CpuActRoom397(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_ROOM_397;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    brain->actTimer = 0;
    { /* PORT-FORK: RNG sequenced in agbcc's order (random.h) */
        u32 rnd = Rand16Twice();
        brain->actVar = ((rnd & 0x1F) + 0x10) | (((rnd & 0x1F) + 0x10) << 8);
    }
    brain->actCount = (Rand16() & 0x1F) + 0x5A;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActRoom397Tick);
}

void CpuActRoom397Tick(struct CpuBrain *brain) {
    if (brain->levelInfo->currentRoom != 0x397) {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        return;
    }
    if (!brain->actCount) {
        brain->keys |= 2;
        brain->actCount = (Rand16() & 0x1F) + 0x5A;
    } else {
        --brain->actCount;
    }
    if (!(brain->actVar & 0xFF)) {
        u32 flags;

        brain->actTimer &= ~0x30;
        flags = 0;
        if (brain->kirby->base.x < 0x2000)
            flags = 0x10;
        else if (brain->kirby->base.x > 0xD000)
            flags = 0x20;
        else {
            u16 r = Rand16() & 7;

            if (r < 2)
                flags = 0x20;
            else if (r < 4)
                flags = 0x10;
        }
        brain->actTimer |= flags;
        brain->actVar |= (Rand16() & 0x1F) + 0x10;
    } else {
        --brain->actVar;
    }
    if (!(brain->actVar & 0xFF00)) {
        u32 flags;

        brain->actTimer &= ~0xC0;
        flags = 0;
        if (brain->kirby->base.y < 0x7800)
            flags = 0x80;
        else if (brain->kirby->base.y > 0x9800)
            flags = 0x40;
        else {
            switch (Rand16() & 0x1F) {
            case 0:
                flags = 0x40;
                break;
            case 1:
                flags = 0x80;
                break;
            }
        }
        brain->actTimer |= flags;
        brain->actVar |= ((Rand16() & 0xF) + 8) << 8;
    } else {
        brain->actVar -= 0x100;
    }
    brain->keys |= brain->actTimer;
}

bool16 CpuBreakBlockNone(struct CpuBrain *brain __attribute__((unused))) {
    return FALSE;
}

void CpuScanNone(struct CpuBrain *brain) {
    brain->target = NULL;
}

void CpuScanMini(struct CpuBrain *brain) {
    brain->target = NULL;
    CpuTryHopOverTarget(brain, NULL);
}

void CpuBrainTaskDestroy(struct Task *brain __attribute__((unused))) {}

void CpuScriptOpNop(struct CpuBrain *brain) {
    ++brain->scriptPc;
}

void CpuScriptOpEnd(struct CpuBrain *brain) {
    brain->flags |= (CPU_FLAG_FREE | CPU_FLAG_SCRIPT_WAITING);
}

void CpuScriptOpWait(struct CpuBrain *brain) {
    u16 r1;

    ++brain->scriptPc;
    r1 = brain->script[brain->scriptPc];
    ++brain->scriptPc;
    brain->flags |= CPU_FLAG_SCRIPT_WAITING;
    CpuScriptWaitBegin(brain, r1);
}

void CpuScriptOpGoToNode(struct CpuBrain *brain) {
    u16 r1;

    ++brain->scriptPc;
    r1 = brain->script[brain->scriptPc];
    ++brain->scriptPc;
    brain->flags |= CPU_FLAG_SCRIPT_WAITING;
    CpuScriptGoToBegin(brain, r1);
}

void CpuScriptOpSetAbility(struct CpuBrain *brain) {
    ++brain->scriptPc;
    brain->wantAbility = brain->script[brain->scriptPc];
    ++brain->scriptPc;
}

void CpuScriptOpFace(struct CpuBrain *brain) {
    u16 r1;

    ++brain->scriptPc;
    r1 = brain->script[brain->scriptPc];
    ++brain->scriptPc;
    brain->flags |= CPU_FLAG_SCRIPT_WAITING;
    CpuScriptFaceBegin(brain, r1);
}

void CpuScriptOpInhale(struct CpuBrain *brain) {
    u16 r1;

    ++brain->scriptPc;
    r1 = brain->script[brain->scriptPc];
    ++brain->scriptPc;
    brain->flags |= CPU_FLAG_SCRIPT_WAITING;
    CpuScriptInhaleBegin(brain, r1);
}

void CpuScriptOpSlide(struct CpuBrain *brain) {
    ++brain->scriptPc;
    brain->flags |= CPU_FLAG_SCRIPT_WAITING;
    CpuScriptSlideBegin(brain);
}

void CpuScriptOpSkip(struct CpuBrain *brain) {
    u16 r1;

    ++brain->scriptPc;
    r1 = brain->scriptPc;
    brain->scriptPc = ++r1 + brain->script[brain->scriptPc];
}

void CpuScriptOpRoam(struct CpuBrain *brain) {
    ++brain->scriptPc;
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoam);
}

void CpuGoalNone(struct CpuBrain *brain __attribute__((unused))) {}

void CpuGoalRoam(struct CpuBrain *brain) {
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoamBegin);
}

void CpuGoalFollowBegin(struct CpuBrain *brain) {
    brain->flags &= ~CPU_FLAG_GOAL_REACHED;
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseMoving);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalFollowTick);
}

void CpuGoalRoom397Wait(struct CpuBrain *brain) {
    if (brain->levelInfo->currentRoom != 0x397)
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalHoldBegin);
}

void CpuSenseNone(struct CpuBrain *brain __attribute__((unused))) {}

void CpuSenseIdle(struct CpuBrain *brain) {
    if (brain->levelInfo->currentRoom == 0x397)
        brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseRoom397);
    else {
        if ((gUnk_0203AD40 & 3) == (brain->kirby->base.unk56 & 3))
            CpuSenseSurroundings(brain);
        if (!(brain->flags & CPU_FLAG_ACTION_BUSY))
            CpuScanForTarget(brain);
        CpuDecideMouthful(brain);
    }
}

void CpuSenseMoving(struct CpuBrain *brain) {
    if (brain->levelInfo->currentRoom == 0x397)
        brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseRoom397);
    else {
        if ((gUnk_0203AD40 & 1) == (brain->kirby->base.unk56 & 1))
            CpuSenseSurroundingsAndPath(brain);
        if (!(brain->flags & CPU_FLAG_ACTION_BUSY))
            CpuScanForTarget(brain);
        CpuDecideMouthful(brain);
    }
}

void CpuSenseRoom397(struct CpuBrain *brain) {
    if (brain->levelInfo->currentRoom != 0x397)
        brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseIdle);
}

void CpuSenseSurroundings(struct CpuBrain *brain) {
    u8 unk = CpuNavNodeAt(&brain->kirby->base);

    if (unk != brain->curNode)
        ++brain->nodeChanges;
    brain->curNode = unk;
    CpuScanTerrain(brain);
    CpuUpdateTargetMemory(brain);
}

void CpuSenseSurroundingsAndPath(struct CpuBrain *brain) {
    u8 unk;
    bool32 b = FALSE;

    CpuScanTerrain(brain);
    unk = CpuNavNodeAt(&brain->kirby->base);
    if (unk != brain->curNode)
        ++brain->nodeChanges;
    brain->curNode = unk;
    if (brain->nextNode != 0xFF)
        b = TRUE;
    if (b)
        brain->nextNode = brain->levelInfo->unk1E8->unk14[brain->curNode].unk0[brain->goalNode];
    CpuUpdateGoalVectors(brain);
    CpuUpdateTargetMemory(brain);
}

void CpuActLeaveFlight(struct CpuBrain *brain) {
    if (brain->kirby->ability == KIRBY_ABILITY_CUPID)
        brain->keys |= 1;
    else
        brain->keys |= 2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActDecide);
}

void CpuActDropAbility(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_DROP_ABILITY;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    brain->keys |= 4;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActAnswerPhone(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_ANSWER_PHONE;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    brain->actArg = Rand16() & 0xF;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAnswerPhoneWait);
}

void CpuActBackToThink(struct CpuBrain *brain) {
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActMove(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_MOVE;
    if (brain->kirby->ability == KIRBY_ABILITY_UFO)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoMove);
    else if (brain->kirby->base.unk58 & 2)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwim);
    else
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActWalk);
}

void CpuActDashFirstTap(struct CpuBrain *brain) {
    u16 r3;

    brain->actTimer = 4;
    r3 = brain->waypointAngle < 0x200 ? 0x10 : 0x20;
    brain->actVar = r3;
    brain->keys = r3;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActDashGap);
}

void CpuActDashGap(struct CpuBrain *brain) {
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActDashSecondTap);
}

void CpuActDashSecondTap(struct CpuBrain *brain) {
    brain->actTimer = 4;
    brain->keys = brain->actVar;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActDashHold);
}

void CpuActDashHold(struct CpuBrain *brain) {
    brain->keys = brain->actVar;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActWalkStride);
}

void CpuActWalkPauseBegin(struct CpuBrain *brain) {
    brain->actTimer = 0x5A;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActWalkPause);
}

void CpuActWalkPause(struct CpuBrain *brain) {
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActWalkStride);
}

void CpuActHoldDown(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_HOLD_DOWN;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    brain->actTimer = 0;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActHoldDownTick);
}

void CpuActHoldDownTick(struct CpuBrain *brain) {
    brain->keys = 0x80;
    if (brain->actArg < brain->actTimer++)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActLadderGrab(struct CpuBrain *brain) {
    if (brain->actTimer++ < 10) {
        brain->keys = 0x80;
        if (brain->kirby->animationIndex == 0x2D || brain->kirby->animationIndex == 0x2E)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActLadderStart);
    } else {
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    }
}

void CpuActLadderStart(struct CpuBrain *brain) {
    brain->actCount = 0;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActLadderClimbBegin);
}

void CpuActLadderClimbBegin(struct CpuBrain *brain) {
    brain->actTimer = (Rand16() & 0x3F) + 0x1E;
    brain->actVar = brain->waypointAngle > 0x100 && brain->waypointAngle < 0x300 ? 0x40 : 0x80;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActLadderClimbTick);
}

void CpuActJumpBegin(struct CpuBrain *brain) {
    brain->actTimer = 4;
    brain->actVar = Rand16() & 3;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActJumpRise);
}

void CpuActFlyFromGround(struct CpuBrain *brain) {
    if (brain->terrain[7]) {
        if (brain->action != CPU_ACT_WALK)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActWalk);
    } else {
        brain->actTimer = 8;
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlyJump);
    }
}

void CpuActFlyJump(struct CpuBrain *brain) {
    brain->keys |= 1;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlyPuffBegin);
}

void CpuActFlyPuffBegin(struct CpuBrain *brain) {
    brain->actTimer = 0x10;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlyPuff);
}

void CpuActFlyPuff(struct CpuBrain *brain) {
    brain->keys |= 0x40;
    if (!--brain->actTimer) {
        if (!(brain->kirby->base.flags & 0x40)) {
            if (brain->action != CPU_ACT_MOVE)
                brain->layers[CPU_LAYER_ACTION] = FN(CpuActMove);
        } else {
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySteerBegin);
        }
    }
}

void CpuActFlySteerBegin(struct CpuBrain *brain) {
    brain->actCount = (Rand16() & 0xF) + 8;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySteerLeg);
}

void CpuActFlySteerLeg(struct CpuBrain *brain) {
    brain->actTimer = (Rand16() & 0x1F) + 0xA;
    brain->actVar = CpuFlyKeysToWaypoint(brain);
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActFlySteerTick);
}

void CpuActSwim(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_SWIM;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    if (!(brain->kirby->base.unk58 & 2))
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActFly);
    else if (brain->kirby->base.flags & 0xC0)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwimStrokeBegin);
    else
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwimLeg);
}

void CpuActSwimStrokeBegin(struct CpuBrain *brain) {
    brain->actTimer = 5;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwimStroke);
}

void CpuActSwimStroke(struct CpuBrain *brain) {
    brain->keys |= 2;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwimLeg);
}

void CpuActSwimLeg(struct CpuBrain *brain) {
    brain->actTimer = (Rand16() & 0xF) + 8;
    brain->actVar = CpuSwimKeysToWaypoint(brain);
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwimTick);
}

void CpuActUfoMove(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_UFO_MOVE;
    brain->flags &= ~CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->ability != KIRBY_ABILITY_UFO)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoMoveLeg);
}

void CpuActUfoMoveLeg(struct CpuBrain *brain) {
    brain->actTimer = (Rand16() & 0xF) + 8;
    brain->actVar = CpuUfoKeysToWaypoint(brain);
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoMoveTick);
}

void CpuActSlideAfterTurn(struct CpuBrain *brain) {
    brain->actArg = brain->actVar;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlideBegin);
}

void CpuActSlideBegin(struct CpuBrain *brain) {
    brain->actTimer = 0x10;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlideCrouch);
}

void CpuActSlideCrouch(struct CpuBrain *brain) {
    brain->keys |= 0x80;
    if (!--brain->actTimer) {
        brain->actTimer = 0x1E;
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlideKick);
    }
}

void CpuActSlideKick(struct CpuBrain *brain) {
    brain->keys |= 0x81;
    if (!brain->actTimer) {
        if (brain->kirby->animationIndex != 0xD && brain->kirby->animationIndex != 0xE)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    } else {
        --brain->actTimer;
    }
}

void CpuActAirPuffAfterTurn(struct CpuBrain *brain) {
    brain->actArg = brain->actVar;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAirPuffBegin);
}

void CpuActAirPuffBegin(struct CpuBrain *brain) {
    brain->actTimer = 2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAirPuffPress);
}

void CpuActAirPuffPress(struct CpuBrain *brain) {
    brain->keys |= 2;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActSpitAfterTurn(struct CpuBrain *brain) {
    brain->actArg = brain->actVar;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSpitBegin);
}

void CpuActSpitBegin(struct CpuBrain *brain) {
    brain->actTimer = 2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSpitPress);
}

void CpuActSpitPress(struct CpuBrain *brain) {
    brain->keys |= 2;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActInhaleAfterTurn(struct CpuBrain *brain) {
    brain->actArg = brain->actVar;
    brain->actArg2 = brain->actCount;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActInhaleBegin);
}

void CpuActInhaleBegin(struct CpuBrain *brain) {
    if (!brain->actArg2)
        brain->actTimer = (Rand16() & 0xFF) + 0x3C;
    else
        brain->actTimer = brain->actArg2;
    brain->actVar = 0;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActInhaleHold);
}

void CpuActSwallowBegin(struct CpuBrain *brain) {
    brain->actTimer = 2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSwallowPress);
}

void CpuActSwallowPress(struct CpuBrain *brain) {
    brain->keys |= 0x80;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActAttackAfterTurn(struct CpuBrain *brain) {
    brain->actArg = brain->actVar;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttackBegin);
}

void CpuActAttackBegin(struct CpuBrain *brain) {
    brain->actTimer = (Rand16() & 0x7F) + 5;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActAttackHold);
}

void CpuActAttackHold(struct CpuBrain *brain) {
    brain->keys |= 2;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActWaterAttackBegin(struct CpuBrain *brain) {
    brain->actTimer = (Rand16() & 0x3F) + 0x10;
    brain->keys |= 2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActWaterAttackHold);
}

void CpuActTurn(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_TURN;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurnBegin);
}

void CpuActTurnBegin(struct CpuBrain *brain) {
    brain->actTimer = 0xB4;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurnTick);
}

void CpuActBombAfterTurn(struct CpuBrain *brain) {
    brain->actArg = brain->actVar;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActBombDraw);
}

void CpuActBombDraw(struct CpuBrain *brain) {
    brain->actTimer = 0x10;
    brain->keys |= 2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActBombWait);
}

void CpuActBombWait(struct CpuBrain *brain) {
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActBombThrowBegin);
}

void CpuActBombThrowBegin(struct CpuBrain *brain) {
    brain->actTimer = 4;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActBombThrow);
}

void CpuActBombThrow(struct CpuBrain *brain) {
    brain->keys |= 2;
    if (brain->actArg2 == 2)
        brain->keys |= 0x40;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActBombFollowBegin);
}

void CpuActBombFollowBegin(struct CpuBrain *brain) {
    brain->actTimer = 4;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActBombFollow);
    CpuActBombFollow(brain);
}

void CpuActBombFollow(struct CpuBrain *brain) {
    if (brain->actArg2 == 2)
        brain->keys |= 0x40;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActBombCooldownBegin);
}

void CpuActBombCooldownBegin(struct CpuBrain *brain) {
    brain->actTimer = 0x3C;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActBombCooldown);
}

void CpuActBombCooldown(struct CpuBrain *brain) {
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActStoneToggle(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_STONE_TOGGLE;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    if (brain->kirby->ability != KIRBY_ABILITY_STONE)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->kirby->animationIndex == 0x34)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggleRevert);
    else
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggleBecome);
}

void CpuActStoneToggleBecome(struct CpuBrain *brain) {
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggleBecomePress);
}

void CpuActStoneToggleBecomePress(struct CpuBrain *brain) {
    brain->keys |= 2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggleFall);
}

void CpuActStoneToggleFall(struct CpuBrain *brain) {
    if (!brain->actTimer) {
        if (!(brain->kirby->base.flags & 0x20))
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggleRevert);
    } else {
        --brain->actTimer;
    }
}

void CpuActStoneToggleRevert(struct CpuBrain *brain) {
    brain->keys |= 2;
    brain->actTimer = (Rand16() & 0x3F) + 5;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneToggleRevertWait);
}

void CpuActStoneToggleRevertWait(struct CpuBrain *brain) {
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActStoneDrop(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_STONE_DROP;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    switch (brain->kirby->ability) {
    default:
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        break;
    case KIRBY_ABILITY_STONE:
    case KIRBY_ABILITY_SMASH:
        if (brain->kirby->animationIndex == 0x34)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropRevert);
        else
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropDispatch);
        break;
    }
}

void CpuActStoneDropDispatch(struct CpuBrain *brain) {
    if (brain->kirby->ability == KIRBY_ABILITY_STONE && brain->kirby->animationIndex == 0x34)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropRevert);
    else if (brain->kirby->base.flags & 0x40)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else if (brain->kirby->base.flags & 0x20)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropAim);
    else
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropFromGround);
}

void CpuActStoneDropFromGround(struct CpuBrain *brain) {
    switch (brain->kirby->ability) {
    default:
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        break;
    case KIRBY_ABILITY_STONE:
    case KIRBY_ABILITY_SMASH:
        if (brain->kirby->base.flags & 0x60)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        else
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropJumpBegin);
        break;
    }
}

void CpuActStoneDropJumpBegin(struct CpuBrain *brain) {
    brain->actTimer = (Rand16() & 0xF) + 0xA;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropJump);
}

void CpuActStoneDropPress(struct CpuBrain *brain) {
    brain->keys |= 2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropFall);
}

void CpuActStoneDropFall(struct CpuBrain *brain) {
    switch (brain->kirby->ability) {
    default:
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        break;
    case KIRBY_ABILITY_STONE:
    case KIRBY_ABILITY_SMASH:
        if (!(brain->kirby->base.flags & 0x20) || brain->kirby->base.flags & 0x40)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        else if (!(brain->kirby->base.flags & 0x20) && !brain->kirby->base.yspeed)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropSitBegin);
        break;
    }
}

void CpuActStoneDropSitBegin(struct CpuBrain *brain) {
    brain->actTimer = (Rand16() & 0x3F) + 0xF;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropSit);
}

void CpuActStoneDropSit(struct CpuBrain *brain) {
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropRevert);
}

void CpuActStoneDropRevert(struct CpuBrain *brain) {
    switch (brain->kirby->ability) {
    default:
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        break;
    case KIRBY_ABILITY_STONE:
    case KIRBY_ABILITY_SMASH:
        if (brain->kirby->animationIndex != 0x34)
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        else
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropRevertPress);
        break;
    }
}

void CpuActStoneDropRevertPress(struct CpuBrain *brain) {
    brain->keys |= 2;
    brain->actTimer = (Rand16() & 0x1F) + 0x1E;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropRevertWait);
}

void CpuActStoneDropRevertWait(struct CpuBrain *brain) {
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActStoneDropRevert);
}

void CpuActThrowAfterTurn(struct CpuBrain *brain) {
    brain->actArg = brain->actVar;
    brain->actArg2 = brain->actCount;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowBegin);
}

void CpuActThrowBegin(struct CpuBrain *brain) {
    if (!brain->actArg2)
        brain->actTimer = (Rand16() & 0x3F) + 0x1E;
    else
        brain->actTimer = brain->actArg2;
    brain->actVar = 0;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowGrab);
}

void CpuActThrowHoldBegin(struct CpuBrain *brain) {
    brain->actTimer = (Rand16() & 0x1F) + 0x1E;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowHold);
}

void CpuActThrowHold(struct CpuBrain *brain) {
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowAim);
}

void CpuActThrowAimHold(struct CpuBrain *brain) {
    brain->keys = brain->actVar;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowRelease);
}

void CpuActThrowRelease(struct CpuBrain *brain) {
    brain->keys = brain->actVar | 2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowCooldownBegin);
}

void CpuActThrowCooldownBegin(struct CpuBrain *brain) {
    brain->actTimer = (Rand16() & 0x3F) + 0x1E;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowCooldown);
}

void CpuActThrowCooldown(struct CpuBrain *brain) {
    if (!--brain->actTimer) {
        if (CpuIsHoldingThrow(brain))
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThrowCooldownBegin);
        else {
            brain->flags &= ~CPU_FLAG_ACTION_BUSY;
            brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
        }
    }
}

void CpuActUfoShotAfterTurn(struct CpuBrain *brain) {
    brain->actArg = brain->actVar;
    brain->actArg2 = brain->actCount;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoShotBegin);
}

void CpuActUfoShotBegin(struct CpuBrain *brain) {
    switch (brain->actArg2) {
    case 0:
        brain->actTimer = 4;
        break;
    case 1:
        brain->actTimer = 0x1E;
        break;
    case 2:
        brain->actTimer = 0x3C;
        break;
    default:
        brain->actTimer = 0x78;
        break;
    }
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActUfoShotCharge);
}

void CpuActUfoShotCharge(struct CpuBrain *brain) {
    brain->keys |= 2;
    if (!--brain->actTimer)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
}

void CpuActCupidShotAfterTurn(struct CpuBrain *brain) {
    brain->actArg = brain->actVar;
    brain->actArg2 = brain->actCount;
    brain->actNext.data = brain->actTimer2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActCupidShotBegin);
}

void CpuActCupidShotBegin(struct CpuBrain *brain) {
    switch (brain->actNext.data) {
    default:
        brain->actTimer = 0x96;
        break;
    case 0:
        brain->actTimer = 0x1E;
        break;
    case 1:
        brain->actTimer = 0x3C;
        break;
    case 2:
        brain->actTimer = 0x64;
        break;
    }
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActCupidShotCharge);
}

void CpuActMissileAfterTurn(struct CpuBrain *brain) {
    brain->actArg = brain->actVar;
    brain->actArg2 = brain->actCount;
    brain->actNext.data = brain->actTimer2;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActMissileBegin);
}

void CpuActMissileBegin(struct CpuBrain *brain) {
    brain->actTimer = 0;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActMissileLaunch);
}

void CpuActMissileHoming(struct CpuBrain *brain) {
    brain->prevAction = brain->action;
    brain->action = CPU_ACT_MISSILE_HOMING;
    brain->flags |= CPU_FLAG_ACTION_BUSY;
    brain->actTimer = 0;
    if (brain->kirby->ability != KIRBY_ABILITY_MISSILE)
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    else
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActMissileHomingLaunch);
}

void CpuScriptWaitBegin(struct CpuBrain *brain, u16 a2) {
    brain->scriptArg = 0x3C * a2;
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptWaitTick);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalHoldBegin);
}

void CpuScriptWaitTick(struct CpuBrain *brain) {
    if (!--brain->scriptArg) {
        brain->flags &= ~CPU_FLAG_SCRIPT_WAITING;
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptStep);
    }
}

void CpuScriptGoToBegin(struct CpuBrain *brain, u16 a2) {
    brain->goalNode = a2;
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalScriptNodeBegin);
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptGoToTick);
}

void CpuScriptGoToTick(struct CpuBrain *brain) {
    if (brain->flags & CPU_FLAG_GOAL_REACHED) {
        brain->flags &= ~CPU_FLAG_SCRIPT_WAITING;
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptStep);
    }
}

void CpuScriptFaceBegin(struct CpuBrain *brain, u16 a2) {
    brain->scriptArg = a2;
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptFaceStart);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalNone);
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseNone);
}

void CpuScriptFaceStart(struct CpuBrain *brain) {
    brain->scriptTimeout = 0x258;
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptFaceTick);
    if (brain->action != CPU_ACT_TURN) {
        brain->actArg = brain->scriptArg;
        brain->actNext.data = 0;
        brain->layers[CPU_LAYER_ACTION] = FN(CpuActTurn);
    }
}

void CpuScriptFaceTick(struct CpuBrain *brain) {
    if (brain->action != CPU_ACT_TURN || !--brain->scriptTimeout) {
        brain->flags &= ~CPU_FLAG_SCRIPT_WAITING;
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptStep);
    }
}

void CpuScriptInhaleBegin(struct CpuBrain *brain, u16 a2) {
    brain->scriptArg = 0x3C * a2;
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptInhalePrepare);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalNone);
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseNone);
}

void CpuScriptInhalePrepare(struct CpuBrain *brain) {
    if (brain->kirby->base.flags & 0x80)
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptInhaleSpitFirst);
    else {
        switch (brain->kirby->ability) {
        default:
            if (brain->kirby->base.flags & 0x40)
                brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptInhaleLandFirst);
            else
                brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptInhaleStart);
            break;
        case KIRBY_ABILITY_UFO:
        case KIRBY_ABILITY_MINI:
            brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptInhaleStart);
            break;
        }
    }
}

void CpuScriptInhaleSpitFirst(struct CpuBrain *brain) {
    brain->scriptTimeout = 0x12C;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSpit);
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptInhaleSpitWait);
}

void CpuScriptInhaleSpitWait(struct CpuBrain *brain) {
    if ((brain->action != CPU_ACT_SPIT && brain->kirby->animationIndex != 0x26) || !--brain->scriptTimeout)
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptInhaleStart);
}

void CpuScriptInhaleLandFirst(struct CpuBrain *brain) {
    brain->scriptTimeout = 0x12C;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptInhaleLandWait);
}

void CpuScriptInhaleLandWait(struct CpuBrain *brain) {
    if (!(brain->kirby->base.flags & 0x40) || !--brain->scriptTimeout)
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptInhaleStart);
}

void CpuScriptInhaleStart(struct CpuBrain *brain) {
    brain->scriptTimeout = 0x3C * (brain->scriptArg + 5);
    brain->actArg = 0xFFFF;
    brain->actArg2 = brain->scriptArg;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActInhale);
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptInhaleTick);
}

void CpuScriptInhaleTick(struct CpuBrain *brain) {
    if (!brain->action || !--brain->scriptTimeout) {
        brain->flags &= ~CPU_FLAG_SCRIPT_WAITING;
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptStep);
    }
}

void CpuScriptSlideBegin(struct CpuBrain *brain) {
    brain->scriptArg = 4;
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlidePrepare);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalNone);
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseNone);
}

void CpuScriptSlideSpitFirst(struct CpuBrain *brain) {
    brain->scriptTimeout = 0x12C;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSpit);
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlideSpitWait);
}

void CpuScriptSlideSpitWait(struct CpuBrain *brain) {
    if ((brain->action != CPU_ACT_SPIT && brain->kirby->animationIndex != 0x26) || !--brain->scriptTimeout)
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlidePrepare);
}

void CpuScriptSlideLandFirst(struct CpuBrain *brain) {
    brain->scriptTimeout = 0x12C;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlideLandWait);
}

void CpuScriptSlideLandWait(struct CpuBrain *brain) {
    if (!(brain->kirby->base.flags & 0x40) || !--brain->scriptTimeout)
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlidePrepare);
}

void CpuScriptSlideFallFirst(struct CpuBrain *brain) {
    brain->scriptTimeout = 0x12C;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlideFallWait);
}

void CpuScriptSlideFallWait(struct CpuBrain *brain) {
    if (!(brain->kirby->base.flags & 0x20) || !--brain->scriptTimeout)
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlidePrepare);
}

void CpuScriptSlideStart(struct CpuBrain *brain) {
    brain->scriptTimeout = 0x12C;
    brain->actArg = 0xFFFF;
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActSlide);
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptSlideTick);
}

void CpuScriptSlideTick(struct CpuBrain *brain) {
    if (brain->action != CPU_ACT_SLIDE || !--brain->scriptTimeout) {
        brain->flags &= ~CPU_FLAG_SCRIPT_WAITING;
        brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptStep);
    }
}

void CpuGoalIdle(struct CpuBrain *brain __attribute__((unused))) {}

void CpuGoalScriptNodeTick(struct CpuBrain *brain) {
    if (!(brain->flags & CPU_FLAG_GOAL_REACHED)) {
        if (brain->goalTimer) --brain->goalTimer;
        if (!(brain->flags & CPU_FLAG_GOAL_REACHED) && (brain->curNode == brain->nextNode || brain->goalNode == 0xFF || !brain->goalTimer)) {
            brain->flags |= CPU_FLAG_GOAL_REACHED;
            brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseIdle);
        }
    }
}

void CpuGoalRoamPauseBegin(struct CpuBrain *brain) {
    if (brain->kirby->base.flags & 0x60)
        brain->goalTimer = (Rand16() & 0x1F) + 0x1E;
    else
        brain->goalTimer = (Rand16() & 0x7F) + 0x1E;
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseIdle);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoamPauseTick);
}

void CpuGoalFollowTick(struct CpuBrain *brain) {
    if (!CpuUpdateFollow(brain))
        brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalRoam);
    else {
        brain->goalNode = CpuNavNodeAt(&brain->followKirby->base);
        brain->goalX = brain->followKirby->base.x;
        brain->goalY = brain->followKirby->base.y;
        brain->nextNode = brain->levelInfo->unk1E8->unk14[brain->curNode].unk0[brain->goalNode];
        CpuUpdateGoalVectors(brain);
    }
}

void CpuGoalOffscreenPauseBegin(struct CpuBrain *brain) {
    if (brain->kirby->base.flags & 0x60)
        brain->goalTimer = (Rand16() & 0x1F) + 0x1E;
    else
        brain->goalTimer = (Rand16() & 0x7F) + 0x1E;
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseIdle);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalOffscreenPauseTick);
}

#ifdef __cplusplus
}
#endif
/* PORT-FNID: these functions were renamed from their address names, so
 * katam.elf no longer names them; pin each code ID to its ROM address
 * (tools/codemod_fnid.py reads these):
 * PORT_FNID_ROM(CpuScanNormal, 0x08010AC9u)
 * PORT_FNID_ROM(CpuScanFire, 0x08010B81u)
 * PORT_FNID_ROM(CpuScanIce, 0x08010C35u)
 * PORT_FNID_ROM(CpuScanBurning, 0x08010CE9u)
 * PORT_FNID_ROM(CpuScanWheel, 0x08010E3Du)
 * PORT_FNID_ROM(CpuScanParasol, 0x08010F91u)
 * PORT_FNID_ROM(CpuScanCutter, 0x08011045u)
 * PORT_FNID_ROM(CpuScanBeam, 0x0801119Du)
 * PORT_FNID_ROM(CpuScanStone, 0x08011251u)
 * PORT_FNID_ROM(CpuScanBomb, 0x08011325u)
 * PORT_FNID_ROM(CpuScanThrow, 0x0801149Du)
 * PORT_FNID_ROM(CpuScanCook, 0x08011559u)
 * PORT_FNID_ROM(CpuScanLaser, 0x080115FDu)
 * PORT_FNID_ROM(CpuScanUfo, 0x08011755u)
 * PORT_FNID_ROM(CpuScanSpark, 0x08011A29u)
 * PORT_FNID_ROM(CpuScanTornado, 0x08011AD9u)
 * PORT_FNID_ROM(CpuScanHammer, 0x08011C2Du)
 * PORT_FNID_ROM(CpuScanSword, 0x08011CE1u)
 * PORT_FNID_ROM(CpuScanCupid, 0x08011D95u)
 * PORT_FNID_ROM(CpuScanFighter, 0x08011F01u)
 * PORT_FNID_ROM(CpuScanMagic, 0x08011FB5u)
 * PORT_FNID_ROM(CpuScanSmash, 0x08012049u)
 * PORT_FNID_ROM(CpuScanCrash, 0x080120FDu)
 * PORT_FNID_ROM(CpuScanMissile, 0x08012191u)
 * PORT_FNID_ROM(CpuScanMaster, 0x0801232Du)
 * PORT_FNID_ROM(CpuScanUnderwater, 0x080123E1u)
 * PORT_FNID_ROM(CpuScanNone, 0x0801836Du)
 * PORT_FNID_ROM(CpuScanMini, 0x08018375u)
 * PORT_FNID_ROM(CpuBreakBlockNormal, 0x080124E1u)
 * PORT_FNID_ROM(CpuBreakBlockFireIce, 0x080126CDu)
 * PORT_FNID_ROM(CpuBreakBlockBurning, 0x08012885u)
 * PORT_FNID_ROM(CpuBreakBlockWheel, 0x08012981u)
 * PORT_FNID_ROM(CpuBreakBlockParasol, 0x08012A89u)
 * PORT_FNID_ROM(CpuBreakBlockShot, 0x08012BC9u)
 * PORT_FNID_ROM(CpuBreakBlockBeam, 0x08012C5Du)
 * PORT_FNID_ROM(CpuBreakBlockStone, 0x08012DD5u)
 * PORT_FNID_ROM(CpuBreakBlockSparkSword, 0x08012EADu)
 * PORT_FNID_ROM(CpuBreakBlockCupid, 0x08012FD9u)
 * PORT_FNID_ROM(CpuBreakBlockHammer, 0x08013175u)
 * PORT_FNID_ROM(CpuBreakBlockThrow, 0x08013319u)
 * PORT_FNID_ROM(CpuBreakBlockMissile, 0x08013519u)
 * PORT_FNID_ROM(CpuBreakBlockTornado, 0x08013721u)
 * PORT_FNID_ROM(CpuBreakBlockNone, 0x08018369u)
 * PORT_FNID_ROM(CpuScriptStep, 0x08013805u)
 * PORT_FNID_ROM(CpuScriptWaitTick, 0x08019759u)
 * PORT_FNID_ROM(CpuScriptGoToTick, 0x08019799u)
 * PORT_FNID_ROM(CpuScriptFaceStart, 0x080197DDu)
 * PORT_FNID_ROM(CpuScriptFaceTick, 0x08019819u)
 * PORT_FNID_ROM(CpuScriptInhalePrepare, 0x08019879u)
 * PORT_FNID_ROM(CpuScriptInhaleSpitFirst, 0x080198C5u)
 * PORT_FNID_ROM(CpuScriptInhaleSpitWait, 0x080198E1u)
 * PORT_FNID_ROM(CpuScriptInhaleLandFirst, 0x08019915u)
 * PORT_FNID_ROM(CpuScriptInhaleLandWait, 0x08019931u)
 * PORT_FNID_ROM(CpuScriptInhaleStart, 0x0801995Du)
 * PORT_FNID_ROM(CpuScriptInhaleTick, 0x08019999u)
 * PORT_FNID_ROM(CpuScriptSlidePrepare, 0x08013925u)
 * PORT_FNID_ROM(CpuScriptSlideSpitFirst, 0x080199F1u)
 * PORT_FNID_ROM(CpuScriptSlideSpitWait, 0x08019A0Du)
 * PORT_FNID_ROM(CpuScriptSlideLandFirst, 0x08019A41u)
 * PORT_FNID_ROM(CpuScriptSlideLandWait, 0x08019A5Du)
 * PORT_FNID_ROM(CpuScriptSlideFallFirst, 0x08019A89u)
 * PORT_FNID_ROM(CpuScriptSlideFallWait, 0x08019AA5u)
 * PORT_FNID_ROM(CpuScriptSlideStart, 0x08019AD1u)
 * PORT_FNID_ROM(CpuScriptSlideTick, 0x08019AF9u)
 * PORT_FNID_ROM(CpuBrainTaskDestroy, 0x08018385u)
 * PORT_FNID_ROM(CpuGoalNone, 0x080184D9u)
 * PORT_FNID_ROM(CpuSenseNone, 0x08018529u)
 * PORT_FNID_ROM(CpuGoalIdle, 0x08019B2Du)
 * PORT_FNID_ROM(CpuGoalHoldBegin, 0x08013A61u)
 * PORT_FNID_ROM(CpuGoalRoom397Wait, 0x08018505u)
 * PORT_FNID_ROM(CpuGoalScriptNodeBegin, 0x08013B01u)
 * PORT_FNID_ROM(CpuGoalScriptNodeTick, 0x08019B31u)
 * PORT_FNID_ROM(CpuGoalRoam, 0x080184DDu)
 * PORT_FNID_ROM(CpuGoalRoamBegin, 0x08013B99u)
 * PORT_FNID_ROM(CpuGoalRoamTick, 0x08013C4Du)
 * PORT_FNID_ROM(CpuGoalRoamPauseBegin, 0x08019B85u)
 * PORT_FNID_ROM(CpuGoalRoamPauseTick, 0x08013D0Du)
 * PORT_FNID_ROM(CpuGoalFollowBegin, 0x080184E9u)
 * PORT_FNID_ROM(CpuGoalFollowTick, 0x08019BF1u)
 * PORT_FNID_ROM(CpuGoalOffscreenBegin, 0x08014929u)
 * PORT_FNID_ROM(CpuGoalOffscreenRoamBegin, 0x080149CDu)
 * PORT_FNID_ROM(CpuGoalOffscreenRoamTick, 0x08014A81u)
 * PORT_FNID_ROM(CpuGoalOffscreenPauseBegin, 0x08019C4Du)
 * PORT_FNID_ROM(CpuGoalOffscreenPauseTick, 0x08014B29u)
 * PORT_FNID_ROM(CpuSenseIdle, 0x0801852Du)
 * PORT_FNID_ROM(CpuSenseMoving, 0x0801858Du)
 * PORT_FNID_ROM(CpuSenseRoom397, 0x080185EDu)
 * PORT_FNID_ROM(CpuActThink, 0x0801519Du)
 * PORT_FNID_ROM(CpuActDecide, 0x080152B1u)
 * PORT_FNID_ROM(CpuActLeaveFlight, 0x080186A5u)
 * PORT_FNID_ROM(CpuActDropAbility, 0x080186DDu)
 * PORT_FNID_ROM(CpuActAnswerPhone, 0x0801870Du)
 * PORT_FNID_ROM(CpuActAnswerPhoneWait, 0x080156FDu)
 * PORT_FNID_ROM(CpuActBackToThink, 0x08018759u)
 * PORT_FNID_ROM(CpuActMove, 0x08018765u)
 * PORT_FNID_ROM(CpuActWalk, 0x08015971u)
 * PORT_FNID_ROM(CpuActWalkStride, 0x08015A4Du)
 * PORT_FNID_ROM(CpuActWalkTick, 0x08015AF1u)
 * PORT_FNID_ROM(CpuActDashFirstTap, 0x080187B1u)
 * PORT_FNID_ROM(CpuActDashGap, 0x080187E9u)
 * PORT_FNID_ROM(CpuActDashSecondTap, 0x08018809u)
 * PORT_FNID_ROM(CpuActDashHold, 0x08018829u)
 * PORT_FNID_ROM(CpuActWalkPauseBegin, 0x08018851u)
 * PORT_FNID_ROM(CpuActWalkPause, 0x08018865u)
 * PORT_FNID_ROM(CpuActHoldDown, 0x08018885u)
 * PORT_FNID_ROM(CpuActHoldDownTick, 0x080188BDu)
 * PORT_FNID_ROM(CpuActLadder, 0x08015FA1u)
 * PORT_FNID_ROM(CpuActLadderGrab, 0x080188F1u)
 * PORT_FNID_ROM(CpuActLadderStart, 0x08018931u)
 * PORT_FNID_ROM(CpuActLadderClimbBegin, 0x08018945u)
 * PORT_FNID_ROM(CpuActLadderClimbTick, 0x0801602Du)
 * PORT_FNID_ROM(CpuActJump, 0x080160B9u)
 * PORT_FNID_ROM(CpuActJumpBegin, 0x080189A1u)
 * PORT_FNID_ROM(CpuActJumpRise, 0x0801617Du)
 * PORT_FNID_ROM(CpuActJumpAir, 0x08016289u)
 * PORT_FNID_ROM(CpuActFly, 0x0801639Du)
 * PORT_FNID_ROM(CpuActFlyFromGround, 0x080189DDu)
 * PORT_FNID_ROM(CpuActFlyJump, 0x08018A11u)
 * PORT_FNID_ROM(CpuActFlyPuffBegin, 0x08018A3Du)
 * PORT_FNID_ROM(CpuActFlyPuff, 0x08018A51u)
 * PORT_FNID_ROM(CpuActFlySteerBegin, 0x08018A9Du)
 * PORT_FNID_ROM(CpuActFlySteerLeg, 0x08018AD1u)
 * PORT_FNID_ROM(CpuActFlySteerTick, 0x08016429u)
 * PORT_FNID_ROM(CpuActFlySweepBegin, 0x080168BDu)
 * PORT_FNID_ROM(CpuActFlySweepTick, 0x0801694Du)
 * PORT_FNID_ROM(CpuActSwim, 0x08018B19u)
 * PORT_FNID_ROM(CpuActSwimStrokeBegin, 0x08018B6Du)
 * PORT_FNID_ROM(CpuActSwimStroke, 0x08018B81u)
 * PORT_FNID_ROM(CpuActSwimLeg, 0x08018BADu)
 * PORT_FNID_ROM(CpuActSwimTick, 0x08016C25u)
 * PORT_FNID_ROM(CpuActUfoMove, 0x08018BF5u)
 * PORT_FNID_ROM(CpuActUfoMoveLeg, 0x08018C39u)
 * PORT_FNID_ROM(CpuActUfoMoveTick, 0x08016EF1u)
 * PORT_FNID_ROM(CpuActSlide, 0x08017145u)
 * PORT_FNID_ROM(CpuActSlideAfterTurn, 0x08018C81u)
 * PORT_FNID_ROM(CpuActSlideBegin, 0x08018C99u)
 * PORT_FNID_ROM(CpuActSlideCrouch, 0x08018CADu)
 * PORT_FNID_ROM(CpuActSlideKick, 0x08018CDDu)
 * PORT_FNID_ROM(CpuActAirPuff, 0x08017275u)
 * PORT_FNID_ROM(CpuActAirPuffAfterTurn, 0x08018D19u)
 * PORT_FNID_ROM(CpuActAirPuffBegin, 0x08018D31u)
 * PORT_FNID_ROM(CpuActAirPuffPress, 0x08018D45u)
 * PORT_FNID_ROM(CpuActSpit, 0x0801737Du)
 * PORT_FNID_ROM(CpuActSpitAfterTurn, 0x08018D71u)
 * PORT_FNID_ROM(CpuActSpitBegin, 0x08018D89u)
 * PORT_FNID_ROM(CpuActSpitPress, 0x08018D9Du)
 * PORT_FNID_ROM(CpuActInhale, 0x0801745Du)
 * PORT_FNID_ROM(CpuActInhaleAfterTurn, 0x08018DC9u)
 * PORT_FNID_ROM(CpuActInhaleBegin, 0x08018DE9u)
 * PORT_FNID_ROM(CpuActInhaleHold, 0x08017549u)
 * PORT_FNID_ROM(CpuActSwallow, 0x080175D1u)
 * PORT_FNID_ROM(CpuActSwallowBegin, 0x08018E3Du)
 * PORT_FNID_ROM(CpuActSwallowPress, 0x08018E51u)
 * PORT_FNID_ROM(CpuActAttack, 0x08017685u)
 * PORT_FNID_ROM(CpuActAttackAfterTurn, 0x08018E7Du)
 * PORT_FNID_ROM(CpuActAttackBegin, 0x08018E95u)
 * PORT_FNID_ROM(CpuActAttackHold, 0x08018EC9u)
 * PORT_FNID_ROM(CpuActWaterAttack, 0x08017751u)
 * PORT_FNID_ROM(CpuActWaterAttackBegin, 0x08018EF5u)
 * PORT_FNID_ROM(CpuActWaterAttackHold, 0x08017801u)
 * PORT_FNID_ROM(CpuActTurn, 0x08018F35u)
 * PORT_FNID_ROM(CpuActTurnBegin, 0x08018F51u)
 * PORT_FNID_ROM(CpuActTurnTick, 0x0801786Du)
 * PORT_FNID_ROM(CpuActBomb, 0x080178DDu)
 * PORT_FNID_ROM(CpuActBombAfterTurn, 0x08018F65u)
 * PORT_FNID_ROM(CpuActBombDraw, 0x08018F7Du)
 * PORT_FNID_ROM(CpuActBombWait, 0x08018F9Du)
 * PORT_FNID_ROM(CpuActBombThrowBegin, 0x08018FBDu)
 * PORT_FNID_ROM(CpuActBombThrow, 0x08018FD1u)
 * PORT_FNID_ROM(CpuActBombFollowBegin, 0x08019011u)
 * PORT_FNID_ROM(CpuActBombFollow, 0x0801902Du)
 * PORT_FNID_ROM(CpuActBombCooldownBegin, 0x08019061u)
 * PORT_FNID_ROM(CpuActBombCooldown, 0x08019075u)
 * PORT_FNID_ROM(CpuActStoneToggle, 0x08019095u)
 * PORT_FNID_ROM(CpuActStoneToggleBecome, 0x080190E9u)
 * PORT_FNID_ROM(CpuActStoneToggleBecomePress, 0x080190F5u)
 * PORT_FNID_ROM(CpuActStoneToggleFall, 0x0801910Du)
 * PORT_FNID_ROM(CpuActStoneToggleRevert, 0x08019139u)
 * PORT_FNID_ROM(CpuActStoneToggleRevertWait, 0x08019179u)
 * PORT_FNID_ROM(CpuActStoneDrop, 0x08019199u)
 * PORT_FNID_ROM(CpuActStoneDropDispatch, 0x080191F1u)
 * PORT_FNID_ROM(CpuActStoneDropFromGround, 0x08019249u)
 * PORT_FNID_ROM(CpuActStoneDropJumpBegin, 0x08019289u)
 * PORT_FNID_ROM(CpuActStoneDropJump, 0x08017951u)
 * PORT_FNID_ROM(CpuActStoneDropAim, 0x08017A4Du)
 * PORT_FNID_ROM(CpuActStoneDropPress, 0x080192BDu)
 * PORT_FNID_ROM(CpuActStoneDropFall, 0x080192D5u)
 * PORT_FNID_ROM(CpuActStoneDropSitBegin, 0x08019325u)
 * PORT_FNID_ROM(CpuActStoneDropSit, 0x08019359u)
 * PORT_FNID_ROM(CpuActStoneDropRevert, 0x08019379u)
 * PORT_FNID_ROM(CpuActStoneDropRevertPress, 0x080193B9u)
 * PORT_FNID_ROM(CpuActStoneDropRevertWait, 0x080193F9u)
 * PORT_FNID_ROM(CpuActThrow, 0x08017B0Du)
 * PORT_FNID_ROM(CpuActThrowAfterTurn, 0x08019419u)
 * PORT_FNID_ROM(CpuActThrowBegin, 0x08019439u)
 * PORT_FNID_ROM(CpuActThrowGrab, 0x08017BA9u)
 * PORT_FNID_ROM(CpuActThrowHoldBegin, 0x0801948Du)
 * PORT_FNID_ROM(CpuActThrowHold, 0x080194C1u)
 * PORT_FNID_ROM(CpuActThrowAim, 0x08017C25u)
 * PORT_FNID_ROM(CpuActThrowAimHold, 0x080194E1u)
 * PORT_FNID_ROM(CpuActThrowRelease, 0x08019509u)
 * PORT_FNID_ROM(CpuActThrowCooldownBegin, 0x08019525u)
 * PORT_FNID_ROM(CpuActThrowCooldown, 0x08019559u)
 * PORT_FNID_ROM(CpuActUfoShot, 0x08017CC5u)
 * PORT_FNID_ROM(CpuActUfoShotAfterTurn, 0x080195A5u)
 * PORT_FNID_ROM(CpuActUfoShotBegin, 0x080195C5u)
 * PORT_FNID_ROM(CpuActUfoShotCharge, 0x0801960Du)
 * PORT_FNID_ROM(CpuActCupidShot, 0x08017D41u)
 * PORT_FNID_ROM(CpuActCupidShotAfterTurn, 0x08019639u)
 * PORT_FNID_ROM(CpuActCupidShotBegin, 0x08019665u)
 * PORT_FNID_ROM(CpuActCupidShotCharge, 0x08017DC5u)
 * PORT_FNID_ROM(CpuActMissile, 0x08017E1Du)
 * PORT_FNID_ROM(CpuActMissileAfterTurn, 0x080196A5u)
 * PORT_FNID_ROM(CpuActMissileBegin, 0x080196D1u)
 * PORT_FNID_ROM(CpuActMissileLaunch, 0x08017EA1u)
 * PORT_FNID_ROM(CpuActMissileFly, 0x08017F11u)
 * PORT_FNID_ROM(CpuActMissileHoming, 0x080196E5u)
 * PORT_FNID_ROM(CpuActMissileHomingLaunch, 0x08017F89u)
 * PORT_FNID_ROM(CpuActMissileHomingSteer, 0x08018025u)
 * PORT_FNID_ROM(CpuActRoom397, 0x08018111u)
 * PORT_FNID_ROM(CpuActRoom397Tick, 0x0801818Du) */

/* PORT-FNID begin: this file's functions the game stores as code
 * IDs, so the lookup can name statics (tools/codemod_fnid.py
 * --gen rewrites this block). */
PORT_FNID_TABLE(game_src_code_0800ECAC_2) {
    PORT_FNID(CpuScanNormal),
    PORT_FNID(CpuScanFire),
    PORT_FNID(CpuScanIce),
    PORT_FNID(CpuScanBurning),
    PORT_FNID(CpuScanWheel),
    PORT_FNID(CpuScanParasol),
    PORT_FNID(CpuScanCutter),
    PORT_FNID(CpuScanBeam),
    PORT_FNID(CpuScanStone),
    PORT_FNID(CpuScanBomb),
    PORT_FNID(CpuScanThrow),
    PORT_FNID(CpuScanCook),
    PORT_FNID(CpuScanLaser),
    PORT_FNID(CpuScanUfo),
    PORT_FNID(CpuScanSpark),
    PORT_FNID(CpuScanTornado),
    PORT_FNID(CpuScanHammer),
    PORT_FNID(CpuScanSword),
    PORT_FNID(CpuScanCupid),
    PORT_FNID(CpuScanFighter),
    PORT_FNID(CpuScanMagic),
    PORT_FNID(CpuScanSmash),
    PORT_FNID(CpuScanCrash),
    PORT_FNID(CpuScanMissile),
    PORT_FNID(CpuScanMaster),
    PORT_FNID(CpuScanUnderwater),
    PORT_FNID(CpuBreakBlockNormal),
    PORT_FNID(CpuBreakBlockFireIce),
    PORT_FNID(CpuBreakBlockBurning),
    PORT_FNID(CpuBreakBlockWheel),
    PORT_FNID(CpuBreakBlockParasol),
    PORT_FNID(CpuBreakBlockShot),
    PORT_FNID(CpuBreakBlockBeam),
    PORT_FNID(CpuBreakBlockStone),
    PORT_FNID(CpuBreakBlockSparkSword),
    PORT_FNID(CpuBreakBlockCupid),
    PORT_FNID(CpuBreakBlockHammer),
    PORT_FNID(CpuBreakBlockThrow),
    PORT_FNID(CpuBreakBlockMissile),
    PORT_FNID(CpuBreakBlockTornado),
    PORT_FNID(CpuScriptStep),
    PORT_FNID(CpuScriptSlidePrepare),
    PORT_FNID(CpuGoalHoldBegin),
    PORT_FNID(CpuGoalScriptNodeBegin),
    PORT_FNID(CpuGoalRoamBegin),
    PORT_FNID(CpuGoalRoamTick),
    PORT_FNID(CpuGoalRoamPauseTick),
    PORT_FNID(CpuGoalOffscreenBegin),
    PORT_FNID(CpuGoalOffscreenRoamBegin),
    PORT_FNID(CpuGoalOffscreenRoamTick),
    PORT_FNID(CpuGoalOffscreenPauseTick),
    PORT_FNID(CpuActThink),
    PORT_FNID(CpuActDecide),
    PORT_FNID(CpuActAnswerPhoneWait),
    PORT_FNID(CpuActWalk),
    PORT_FNID(CpuActWalkStride),
    PORT_FNID(CpuActWalkTick),
    PORT_FNID(CpuActLadder),
    PORT_FNID(CpuActLadderClimbTick),
    PORT_FNID(CpuActJump),
    PORT_FNID(CpuActJumpRise),
    PORT_FNID(CpuActJumpAir),
    PORT_FNID(CpuActFly),
    PORT_FNID(CpuActFlySteerTick),
    PORT_FNID(CpuActFlySweepBegin),
    PORT_FNID(CpuActFlySweepTick),
    PORT_FNID(CpuActSwimTick),
    PORT_FNID(CpuActUfoMoveTick),
    PORT_FNID(CpuActSlide),
    PORT_FNID(CpuActAirPuff),
    PORT_FNID(CpuActSpit),
    PORT_FNID(CpuActInhale),
    PORT_FNID(CpuActInhaleHold),
    PORT_FNID(CpuActSwallow),
    PORT_FNID(CpuActAttack),
    PORT_FNID(CpuActWaterAttack),
    PORT_FNID(CpuActWaterAttackHold),
    PORT_FNID(CpuActTurnTick),
    PORT_FNID(CpuActBomb),
    PORT_FNID(CpuActStoneDropJump),
    PORT_FNID(CpuActStoneDropAim),
    PORT_FNID(CpuActThrow),
    PORT_FNID(CpuActThrowGrab),
    PORT_FNID(CpuActThrowAim),
    PORT_FNID(CpuActUfoShot),
    PORT_FNID(CpuActCupidShot),
    PORT_FNID(CpuActCupidShotCharge),
    PORT_FNID(CpuActMissile),
    PORT_FNID(CpuActMissileLaunch),
    PORT_FNID(CpuActMissileFly),
    PORT_FNID(CpuActMissileHomingLaunch),
    PORT_FNID(CpuActMissileHomingSteer),
    PORT_FNID(CpuActRoom397),
    PORT_FNID(CpuActRoom397Tick),
    PORT_FNID(CpuBreakBlockNone),
    PORT_FNID(CpuScanNone),
    PORT_FNID(CpuScanMini),
    PORT_FNID(CpuBrainTaskDestroy),
    PORT_FNID(CpuGoalNone),
    PORT_FNID(CpuGoalRoam),
    PORT_FNID(CpuGoalFollowBegin),
    PORT_FNID(CpuGoalRoom397Wait),
    PORT_FNID(CpuSenseNone),
    PORT_FNID(CpuSenseIdle),
    PORT_FNID(CpuSenseMoving),
    PORT_FNID(CpuSenseRoom397),
    PORT_FNID(CpuActLeaveFlight),
    PORT_FNID(CpuActDropAbility),
    PORT_FNID(CpuActAnswerPhone),
    PORT_FNID(CpuActBackToThink),
    PORT_FNID(CpuActMove),
    PORT_FNID(CpuActDashFirstTap),
    PORT_FNID(CpuActDashGap),
    PORT_FNID(CpuActDashSecondTap),
    PORT_FNID(CpuActDashHold),
    PORT_FNID(CpuActWalkPauseBegin),
    PORT_FNID(CpuActWalkPause),
    PORT_FNID(CpuActHoldDown),
    PORT_FNID(CpuActHoldDownTick),
    PORT_FNID(CpuActLadderGrab),
    PORT_FNID(CpuActLadderStart),
    PORT_FNID(CpuActLadderClimbBegin),
    PORT_FNID(CpuActJumpBegin),
    PORT_FNID(CpuActFlyFromGround),
    PORT_FNID(CpuActFlyJump),
    PORT_FNID(CpuActFlyPuffBegin),
    PORT_FNID(CpuActFlyPuff),
    PORT_FNID(CpuActFlySteerBegin),
    PORT_FNID(CpuActFlySteerLeg),
    PORT_FNID(CpuActSwim),
    PORT_FNID(CpuActSwimStrokeBegin),
    PORT_FNID(CpuActSwimStroke),
    PORT_FNID(CpuActSwimLeg),
    PORT_FNID(CpuActUfoMove),
    PORT_FNID(CpuActUfoMoveLeg),
    PORT_FNID(CpuActSlideAfterTurn),
    PORT_FNID(CpuActSlideBegin),
    PORT_FNID(CpuActSlideCrouch),
    PORT_FNID(CpuActSlideKick),
    PORT_FNID(CpuActAirPuffAfterTurn),
    PORT_FNID(CpuActAirPuffBegin),
    PORT_FNID(CpuActAirPuffPress),
    PORT_FNID(CpuActSpitAfterTurn),
    PORT_FNID(CpuActSpitBegin),
    PORT_FNID(CpuActSpitPress),
    PORT_FNID(CpuActInhaleAfterTurn),
    PORT_FNID(CpuActInhaleBegin),
    PORT_FNID(CpuActSwallowBegin),
    PORT_FNID(CpuActSwallowPress),
    PORT_FNID(CpuActAttackAfterTurn),
    PORT_FNID(CpuActAttackBegin),
    PORT_FNID(CpuActAttackHold),
    PORT_FNID(CpuActWaterAttackBegin),
    PORT_FNID(CpuActTurn),
    PORT_FNID(CpuActTurnBegin),
    PORT_FNID(CpuActBombAfterTurn),
    PORT_FNID(CpuActBombDraw),
    PORT_FNID(CpuActBombWait),
    PORT_FNID(CpuActBombThrowBegin),
    PORT_FNID(CpuActBombThrow),
    PORT_FNID(CpuActBombFollowBegin),
    PORT_FNID(CpuActBombFollow),
    PORT_FNID(CpuActBombCooldownBegin),
    PORT_FNID(CpuActBombCooldown),
    PORT_FNID(CpuActStoneToggle),
    PORT_FNID(CpuActStoneToggleBecome),
    PORT_FNID(CpuActStoneToggleBecomePress),
    PORT_FNID(CpuActStoneToggleFall),
    PORT_FNID(CpuActStoneToggleRevert),
    PORT_FNID(CpuActStoneToggleRevertWait),
    PORT_FNID(CpuActStoneDrop),
    PORT_FNID(CpuActStoneDropDispatch),
    PORT_FNID(CpuActStoneDropFromGround),
    PORT_FNID(CpuActStoneDropJumpBegin),
    PORT_FNID(CpuActStoneDropPress),
    PORT_FNID(CpuActStoneDropFall),
    PORT_FNID(CpuActStoneDropSitBegin),
    PORT_FNID(CpuActStoneDropSit),
    PORT_FNID(CpuActStoneDropRevert),
    PORT_FNID(CpuActStoneDropRevertPress),
    PORT_FNID(CpuActStoneDropRevertWait),
    PORT_FNID(CpuActThrowAfterTurn),
    PORT_FNID(CpuActThrowBegin),
    PORT_FNID(CpuActThrowHoldBegin),
    PORT_FNID(CpuActThrowHold),
    PORT_FNID(CpuActThrowAimHold),
    PORT_FNID(CpuActThrowRelease),
    PORT_FNID(CpuActThrowCooldownBegin),
    PORT_FNID(CpuActThrowCooldown),
    PORT_FNID(CpuActUfoShotAfterTurn),
    PORT_FNID(CpuActUfoShotBegin),
    PORT_FNID(CpuActUfoShotCharge),
    PORT_FNID(CpuActCupidShotAfterTurn),
    PORT_FNID(CpuActCupidShotBegin),
    PORT_FNID(CpuActMissileAfterTurn),
    PORT_FNID(CpuActMissileBegin),
    PORT_FNID(CpuActMissileHoming),
    PORT_FNID(CpuScriptWaitTick),
    PORT_FNID(CpuScriptGoToTick),
    PORT_FNID(CpuScriptFaceStart),
    PORT_FNID(CpuScriptFaceTick),
    PORT_FNID(CpuScriptInhalePrepare),
    PORT_FNID(CpuScriptInhaleSpitFirst),
    PORT_FNID(CpuScriptInhaleSpitWait),
    PORT_FNID(CpuScriptInhaleLandFirst),
    PORT_FNID(CpuScriptInhaleLandWait),
    PORT_FNID(CpuScriptInhaleStart),
    PORT_FNID(CpuScriptInhaleTick),
    PORT_FNID(CpuScriptSlideSpitFirst),
    PORT_FNID(CpuScriptSlideSpitWait),
    PORT_FNID(CpuScriptSlideLandFirst),
    PORT_FNID(CpuScriptSlideLandWait),
    PORT_FNID(CpuScriptSlideFallFirst),
    PORT_FNID(CpuScriptSlideFallWait),
    PORT_FNID(CpuScriptSlideStart),
    PORT_FNID(CpuScriptSlideTick),
    PORT_FNID(CpuGoalIdle),
    PORT_FNID(CpuGoalScriptNodeTick),
    PORT_FNID(CpuGoalRoamPauseBegin),
    PORT_FNID(CpuGoalFollowTick),
    PORT_FNID(CpuGoalOffscreenPauseBegin),
    PORT_FNID_END
};
/* PORT-FNID end */
