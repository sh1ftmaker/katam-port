# The CPU Kirby brain (`code_0800ECAC.c`, `code_0800ECAC_2.c`, `sub_0800F044.c`)

**Status: renamed in the tree** (branch `six-ainames`).  Renames only: no
behaviour change, every code ID kept at its ROM address (`PORT_FNID_ROM`
comments at the end of the two files), layout asserts unchanged in value.
Names marked *(guess)* below and in the code are best guesses.

## Shape

One `struct CpuBrain` per Kirby slot (`gUnk_02038590[slot]`), created by
`CpuBrainCreate` from the start of play (`code_080332BC.c`) for every slot at or
above the human count, reset by `CpuBrainReset` on every room arrival
(`kirby.c`).  The task `CpuBrainTask` runs every frame and, unless a person
holds the slot (`PortLmpBrainWaits`) or the Kirby is dead / between rooms,
clears `keys` and calls four layers in order:

| layer | job | functions |
|---|---|---|
| `CPU_LAYER_SCRIPT` | the room's per-slot script (`gUnk_08D63C28[room]->unk0[slot]`, `CPU_OP_*`) | `CpuScriptStep`, `CpuScriptOp*`, `CpuScript*Begin/Tick` |
| `CPU_LAYER_GOAL` | choose the goal: script node, hold, roam, follow a person, off-screen travel | `CpuGoal*`, `CpuTryRoomHop` |
| `CPU_LAYER_SENSE` | terrain grid, goal/waypoint vectors, target scan, mouthful decision | `CpuSense*`, `CpuScanTerrain`, `CpuScanForTarget`, `CpuDecideMouthful` |
| `CPU_LAYER_ACTION` | the action state machine (`action`, `CPU_ACT_*`); writes `keys` | `CpuAct*` |

`keys` is a GBA pad word; `sub_0803F324` (`kirby.c`) feeds it to the Kirby in
place of a pad.  The brain never writes the Kirby's state directly, except
`CpuWarpToRoom` (room hops while off-screen) and the off-screen ability gift
(`sub_0805BD4C` in `CpuActDecide`).

## Old -> new

### Functions, tables and types

| old | new |
|---|---|
| `sub_0800ECAC` | `CpuBrainCreate` |
| `sub_0800EE04` | `CpuBrainReset` |
| `sub_0800EEBC` | `CpuNavNodeAt` |
| `sub_0800EF60` | `CpuBrainTask` |
| `sub_0800F044` | `CpuScanTerrain` |
| `sub_0800FE50` | `CpuNearestHumanInRoom` |
| `sub_0800FF60` | `CpuDecideMouthful` |
| `sub_08010034` | `CpuPickRandomNode` |
| `sub_08010144` | `CpuUpdateGoalVectors` |
| `sub_08010294` | `CpuUpdateTargetMemory` |
| `sub_080103BC` | `CpuUpdateFollow` |
| `sub_08010590` | `CpuFindTarget` |
| `sub_08010818` | `CpuTryHopOverTarget` |
| `sub_080108B8` | `CpuScanForTarget` |
| `sub_08011754_helper` | `CpuRowIsClear` |
| `sub_08010944` | `CpuScanTargetFlying` |
| `sub_08010A34` | `CpuScanTargetMouthful` |
| `sub_08010AC8` | `CpuScanNormal` |
| `sub_08010B80` | `CpuScanFire` |
| `sub_08010C34` | `CpuScanIce` |
| `sub_08010CE8` | `CpuScanBurning` |
| `sub_08010E3C` | `CpuScanWheel` |
| `sub_08010F90` | `CpuScanParasol` |
| `sub_08011044` | `CpuScanCutter` |
| `sub_0801119C` | `CpuScanBeam` |
| `sub_08011250` | `CpuScanStone` |
| `sub_08011324` | `CpuScanBomb` |
| `sub_0801149C` | `CpuScanThrow` |
| `sub_08011558` | `CpuScanCook` |
| `sub_080115FC` | `CpuScanLaser` |
| `sub_08011754` | `CpuScanUfo` |
| `sub_08011A28` | `CpuScanSpark` |
| `sub_08011AD8` | `CpuScanTornado` |
| `sub_08011C2C` | `CpuScanHammer` |
| `sub_08011CE0` | `CpuScanSword` |
| `sub_08011D94` | `CpuScanCupid` |
| `sub_08011F00` | `CpuScanFighter` |
| `sub_08011FB4` | `CpuScanMagic` |
| `sub_08012048` | `CpuScanSmash` |
| `sub_080120FC` | `CpuScanCrash` |
| `sub_08012190` | `CpuScanMissile` |
| `sub_0801232C` | `CpuScanMaster` |
| `sub_080123E0` | `CpuScanUnderwater` |
| `sub_0801836C` | `CpuScanNone` |
| `sub_08018374` | `CpuScanMini` |
| `sub_080124E0` | `CpuBreakBlockNormal` |
| `sub_080126CC` | `CpuBreakBlockFireIce` |
| `sub_08012884` | `CpuBreakBlockBurning` |
| `sub_08012980` | `CpuBreakBlockWheel` |
| `sub_08012A88` | `CpuBreakBlockParasol` |
| `sub_08012BC8` | `CpuBreakBlockShot` |
| `sub_08012C5C` | `CpuBreakBlockBeam` |
| `sub_08012DD4` | `CpuBreakBlockStone` |
| `sub_08012EAC` | `CpuBreakBlockSparkSword` |
| `sub_08012FD8` | `CpuBreakBlockCupid` |
| `sub_08013174` | `CpuBreakBlockHammer` |
| `sub_08013318` | `CpuBreakBlockThrow` |
| `sub_08013518` | `CpuBreakBlockMissile` |
| `sub_08013720` | `CpuBreakBlockTornado` |
| `sub_08018368` | `CpuBreakBlockNone` |
| `sub_08013804` | `CpuScriptStep` |
| `sub_08018388` | `CpuScriptOpNop` |
| `sub_08018394` | `CpuScriptOpEnd` |
| `sub_080183A0` | `CpuScriptOpWait` |
| `sub_080183D0` | `CpuScriptOpGoToNode` |
| `sub_08018400` | `CpuScriptOpSetAbility` |
| `sub_08018428` | `CpuScriptOpFace` |
| `sub_08018458` | `CpuScriptOpInhale` |
| `sub_08018488` | `CpuScriptOpSlide` |
| `sub_080184A4` | `CpuScriptOpSkip` |
| `sub_0801399C` | `CpuScriptOpIfRoomState` |
| `sub_080184C4` | `CpuScriptOpRoam` |
| `sub_08019734` | `CpuScriptWaitBegin` |
| `sub_08019758` | `CpuScriptWaitTick` |
| `sub_08019784` | `CpuScriptGoToBegin` |
| `sub_08019798` | `CpuScriptGoToTick` |
| `sub_080197BC` | `CpuScriptFaceBegin` |
| `sub_080197DC` | `CpuScriptFaceStart` |
| `sub_08019818` | `CpuScriptFaceTick` |
| `sub_0801984C` | `CpuScriptInhaleBegin` |
| `sub_08019878` | `CpuScriptInhalePrepare` |
| `sub_080198C4` | `CpuScriptInhaleSpitFirst` |
| `sub_080198E0` | `CpuScriptInhaleSpitWait` |
| `sub_08019914` | `CpuScriptInhaleLandFirst` |
| `sub_08019930` | `CpuScriptInhaleLandWait` |
| `sub_0801995C` | `CpuScriptInhaleStart` |
| `sub_08019998` | `CpuScriptInhaleTick` |
| `sub_080199CC` | `CpuScriptSlideBegin` |
| `sub_08013924` | `CpuScriptSlidePrepare` |
| `sub_080199F0` | `CpuScriptSlideSpitFirst` |
| `sub_08019A0C` | `CpuScriptSlideSpitWait` |
| `sub_08019A40` | `CpuScriptSlideLandFirst` |
| `sub_08019A5C` | `CpuScriptSlideLandWait` |
| `sub_08019A88` | `CpuScriptSlideFallFirst` |
| `sub_08019AA4` | `CpuScriptSlideFallWait` |
| `sub_08019AD0` | `CpuScriptSlideStart` |
| `sub_08019AF8` | `CpuScriptSlideTick` |
| `nullsub_103` | `CpuBrainTaskDestroy` |
| `nullsub_104` | `CpuGoalNone` |
| `nullsub_105` | `CpuSenseNone` |
| `nullsub_106` | `CpuGoalIdle` |
| `sub_08013A60` | `CpuGoalHoldBegin` |
| `sub_08018504` | `CpuGoalRoom397Wait` |
| `sub_08013B00` | `CpuGoalScriptNodeBegin` |
| `sub_08013B00_helper` | `CpuNavGraph` |
| `sub_08019B30` | `CpuGoalScriptNodeTick` |
| `sub_080184DC` | `CpuGoalRoam` |
| `sub_08013B98` | `CpuGoalRoamBegin` |
| `sub_08013C4C` | `CpuGoalRoamTick` |
| `sub_08019B84` | `CpuGoalRoamPauseBegin` |
| `sub_08013D0C` | `CpuGoalRoamPauseTick` |
| `sub_080184E8` | `CpuGoalFollowBegin` |
| `sub_08019BF0` | `CpuGoalFollowTick` |
| `sub_08014928` | `CpuGoalOffscreenBegin` |
| `sub_080149CC` | `CpuGoalOffscreenRoamBegin` |
| `sub_08014A80` | `CpuGoalOffscreenRoamTick` |
| `sub_08019C4C` | `CpuGoalOffscreenPauseBegin` |
| `sub_08014B28` | `CpuGoalOffscreenPauseTick` |
| `sub_08014454` | `CpuTryRoomHop` |
| `sub_08014454_helper` | `CpuLinkedRoomIndex` |
| `sub_08013DB4` | `CpuPickLeader` |
| `sub_08013F48` | `CpuWarpToRoom` |
| `sub_08013FC4` | `CpuPickAreaExit` |
| `sub_080140B0` | `CpuPickNeighborRoom` |
| `sub_080141EC` | `CpuNextRoomToward` |
| `sub_0801852C` | `CpuSenseIdle` |
| `sub_0801858C` | `CpuSenseMoving` |
| `sub_080185EC` | `CpuSenseRoom397` |
| `sub_08018610` | `CpuSenseSurroundings` |
| `sub_08018640` | `CpuSenseSurroundingsAndPath` |
| `sub_0801519C` | `CpuActThink` |
| `sub_0801519C_helper` | `CpuIsHoldingThrow` |
| `sub_080152B0` | `CpuActDecide` |
| `sub_080152B0_helper` | `CpuShouldAnswerPhone` |
| `sub_080152B0_helper_2` | `CpuIsAboveLadder` |
| `sub_08015AF0_helper` | `CpuIsOnLadder` |
| `sub_080186A4` | `CpuActLeaveFlight` |
| `sub_080186DC` | `CpuActDropAbility` |
| `sub_0801870C` | `CpuActAnswerPhone` |
| `sub_080156FC` | `CpuActAnswerPhoneWait` |
| `sub_08018758` | `CpuActBackToThink` |
| `sub_08018764` | `CpuActMove` |
| `sub_08015970` | `CpuActWalk` |
| `sub_08015A4C` | `CpuActWalkStride` |
| `sub_08015AF0` | `CpuActWalkTick` |
| `sub_080157B0` | `CpuJumpNeeded` |
| `sub_080187B0` | `CpuActDashFirstTap` |
| `sub_080187E8` | `CpuActDashGap` |
| `sub_08018808` | `CpuActDashSecondTap` |
| `sub_08018828` | `CpuActDashHold` |
| `sub_08018850` | `CpuActWalkPauseBegin` |
| `sub_08018864` | `CpuActWalkPause` |
| `sub_08018884` | `CpuActHoldDown` |
| `sub_080188BC` | `CpuActHoldDownTick` |
| `sub_08015FA0` | `CpuActLadder` |
| `sub_080188F0` | `CpuActLadderGrab` |
| `sub_08018930` | `CpuActLadderStart` |
| `sub_08018944` | `CpuActLadderClimbBegin` |
| `sub_0801602C` | `CpuActLadderClimbTick` |
| `sub_080160B8` | `CpuActJump` |
| `sub_080189A0` | `CpuActJumpBegin` |
| `sub_0801617C` | `CpuActJumpRise` |
| `sub_08016288` | `CpuActJumpAir` |
| `sub_0801639C` | `CpuActFly` |
| `sub_080189DC` | `CpuActFlyFromGround` |
| `sub_08018A10` | `CpuActFlyJump` |
| `sub_08018A3C` | `CpuActFlyPuffBegin` |
| `sub_08018A50` | `CpuActFlyPuff` |
| `sub_08018A9C` | `CpuActFlySteerBegin` |
| `sub_08018AD0` | `CpuActFlySteerLeg` |
| `sub_08016428` | `CpuActFlySteerTick` |
| `sub_080168BC` | `CpuActFlySweepBegin` |
| `sub_0801694C` | `CpuActFlySweepTick` |
| `sub_08014B9C` | `CpuFlyKeysToWaypoint` |
| `sub_08018B18` | `CpuActSwim` |
| `sub_08018B6C` | `CpuActSwimStrokeBegin` |
| `sub_08018B80` | `CpuActSwimStroke` |
| `sub_08018BAC` | `CpuActSwimLeg` |
| `sub_08016C24` | `CpuActSwimTick` |
| `sub_08014DA4` | `CpuSwimKeysToWaypoint` |
| `sub_08018BF4` | `CpuActUfoMove` |
| `sub_08018C38` | `CpuActUfoMoveLeg` |
| `sub_08016EF0` | `CpuActUfoMoveTick` |
| `sub_08014FA0` | `CpuUfoKeysToWaypoint` |
| `sub_08017144` | `CpuActSlide` |
| `sub_08018C80` | `CpuActSlideAfterTurn` |
| `sub_08018C98` | `CpuActSlideBegin` |
| `sub_08018CAC` | `CpuActSlideCrouch` |
| `sub_08018CDC` | `CpuActSlideKick` |
| `sub_08017274` | `CpuActAirPuff` |
| `sub_08018D18` | `CpuActAirPuffAfterTurn` |
| `sub_08018D30` | `CpuActAirPuffBegin` |
| `sub_08018D44` | `CpuActAirPuffPress` |
| `sub_0801737C` | `CpuActSpit` |
| `sub_08018D70` | `CpuActSpitAfterTurn` |
| `sub_08018D88` | `CpuActSpitBegin` |
| `sub_08018D9C` | `CpuActSpitPress` |
| `sub_0801745C` | `CpuActInhale` |
| `sub_08018DC8` | `CpuActInhaleAfterTurn` |
| `sub_08018DE8` | `CpuActInhaleBegin` |
| `sub_08017548` | `CpuActInhaleHold` |
| `sub_080175D0` | `CpuActSwallow` |
| `sub_08018E3C` | `CpuActSwallowBegin` |
| `sub_08018E50` | `CpuActSwallowPress` |
| `sub_08017684` | `CpuActAttack` |
| `sub_08018E7C` | `CpuActAttackAfterTurn` |
| `sub_08018E94` | `CpuActAttackBegin` |
| `sub_08018EC8` | `CpuActAttackHold` |
| `sub_08017750` | `CpuActWaterAttack` |
| `sub_08018EF4` | `CpuActWaterAttackBegin` |
| `sub_08017800` | `CpuActWaterAttackHold` |
| `sub_08018F34` | `CpuActTurn` |
| `sub_08018F50` | `CpuActTurnBegin` |
| `sub_0801786C` | `CpuActTurnTick` |
| `sub_080178DC` | `CpuActBomb` |
| `sub_08018F64` | `CpuActBombAfterTurn` |
| `sub_08018F7C` | `CpuActBombDraw` |
| `sub_08018F9C` | `CpuActBombWait` |
| `sub_08018FBC` | `CpuActBombThrowBegin` |
| `sub_08018FD0` | `CpuActBombThrow` |
| `sub_08019010` | `CpuActBombFollowBegin` |
| `sub_0801902C` | `CpuActBombFollow` |
| `sub_08019060` | `CpuActBombCooldownBegin` |
| `sub_08019074` | `CpuActBombCooldown` |
| `sub_08019094` | `CpuActStoneToggle` |
| `sub_080190E8` | `CpuActStoneToggleBecome` |
| `sub_080190F4` | `CpuActStoneToggleBecomePress` |
| `sub_0801910C` | `CpuActStoneToggleFall` |
| `sub_08019138` | `CpuActStoneToggleRevert` |
| `sub_08019178` | `CpuActStoneToggleRevertWait` |
| `sub_08019198` | `CpuActStoneDrop` |
| `sub_080191F0` | `CpuActStoneDropDispatch` |
| `sub_08019248` | `CpuActStoneDropFromGround` |
| `sub_08019288` | `CpuActStoneDropJumpBegin` |
| `sub_08017950` | `CpuActStoneDropJump` |
| `sub_08017A4C` | `CpuActStoneDropAim` |
| `sub_080192BC` | `CpuActStoneDropPress` |
| `sub_080192D4` | `CpuActStoneDropFall` |
| `sub_08019324` | `CpuActStoneDropSitBegin` |
| `sub_08019358` | `CpuActStoneDropSit` |
| `sub_08019378` | `CpuActStoneDropRevert` |
| `sub_080193B8` | `CpuActStoneDropRevertPress` |
| `sub_080193F8` | `CpuActStoneDropRevertWait` |
| `sub_08017B0C` | `CpuActThrow` |
| `sub_08019418` | `CpuActThrowAfterTurn` |
| `sub_08019438` | `CpuActThrowBegin` |
| `sub_08017BA8` | `CpuActThrowGrab` |
| `sub_0801948C` | `CpuActThrowHoldBegin` |
| `sub_080194C0` | `CpuActThrowHold` |
| `sub_08017C24` | `CpuActThrowAim` |
| `sub_080194E0` | `CpuActThrowAimHold` |
| `sub_08019508` | `CpuActThrowRelease` |
| `sub_08019524` | `CpuActThrowCooldownBegin` |
| `sub_08019558` | `CpuActThrowCooldown` |
| `sub_08017CC4` | `CpuActUfoShot` |
| `sub_080195A4` | `CpuActUfoShotAfterTurn` |
| `sub_080195C4` | `CpuActUfoShotBegin` |
| `sub_0801960C` | `CpuActUfoShotCharge` |
| `sub_08017D40` | `CpuActCupidShot` |
| `sub_08019638` | `CpuActCupidShotAfterTurn` |
| `sub_08019664` | `CpuActCupidShotBegin` |
| `sub_08017DC4` | `CpuActCupidShotCharge` |
| `sub_08017E1C` | `CpuActMissile` |
| `sub_080196A4` | `CpuActMissileAfterTurn` |
| `sub_080196D0` | `CpuActMissileBegin` |
| `sub_08017EA0` | `CpuActMissileLaunch` |
| `sub_08017F10` | `CpuActMissileFly` |
| `sub_080196E4` | `CpuActMissileHoming` |
| `sub_08017F88` | `CpuActMissileHomingLaunch` |
| `sub_08018024` | `CpuActMissileHomingSteer` |
| `sub_08018110` | `CpuActRoom397` |
| `sub_0801818C` | `CpuActRoom397Tick` |
| `gUnk_082DDE84` | `gCpuBreakBlockChecks` |
| `gUnk_082DDEF0` | `gCpuTargetScansLand` |
| `gUnk_082DDF5C` | `gCpuTargetScansWater` |
| `gUnk_082DDFC8` | `gCpuAbilityAttackMasks` |
| `gUnk_082DE034` | `gCpuOffscreenAbilityPool` |
| `gUnk_082DE074` | `gCpuLinkedRooms` |
| `Unk_02038590_4C` | `CpuTargetMemory` |
| `Unk_02038590` | `CpuBrain` |
| `Unk_0800ECAC` | `CpuBrainTaskData` |

### `struct CpuBrain` fields (was `struct Unk_02038590`)

| old | new |
|---|---|
| `unk0` | `layers` |
| `unk14` | `levelInfo` |
| `unk18` | `homeRoom` |
| `unk1A` | `curNode` |
| `unk1B` | `goalNode` |
| `unk1C` | `terrain` |
| `unk26` | `terrainRow` |
| `unk2B` | `terrainBelow` |
| `unk3C` | `nodeChanges` |
| `unk3E` | `wantAbility` |
| `unk3F` | `nextNode` |
| `unk40` | `kirby` |
| `unk44` | `followKirby` |
| `unk48` | `target` |
| `unk4C` | `targetMemory` |
| `unk7C` | `goalX` |
| `unk80` | `goalY` |
| `unk84` | `goalDx` |
| `unk86` | `goalDy` |
| `unk88` | `goalDxDyDiff` |
| `unk8A` | `goalAngle` |
| `unk8C` | `waypointX` |
| `unk90` | `waypointY` |
| `unk94` | `waypointDx` |
| `unk96` | `waypointDy` |
| `unk98` | `waypointDist` |
| `unk9A` | `waypointAngle` |
| `unk9C` | `prevKeys` |
| `unk9E` | `keys` |
| `unkA0` | `swallowCooldown` |
| `unkA2` | `nearHumanMeter` |
| `unkA4` | `followStillFrames` |
| `unkA8` | `leader` |
| `unkAC` | `roomHopTimer` |
| `unkAE` | `destRoom` |
| `unkB0` | `areaWarpCount` |
| `unkB4` | `script` |
| `unkB8` | `scriptPc` |
| `unkBC` | `scriptArg` |
| `unkBE` | `scriptTimeout` |
| `unkCC` | `goalTimer` |
| `unkDC` | `action` |
| `unkDD` | `prevAction` |
| `unkDE` | `actArg` |
| `unkE0` | `actArg2` |
| `unkE4` | `actNext` |
| `unkE8` | `actTimer` |
| `unkEA` | `actVar` |
| `unkEC` | `actCount` |
| `unkEE` | `actTimer2` |

### `struct CpuTargetMemory` (was `Unk_02038590_4C`)

| old | new |
|---|---|
| `unk0` | `ignoring` |
| `unk4` | `obj` |
| `unk8` | `timer` |
| `unkA` | `lastHp` |

`struct CpuBrainTaskData` (was `Unk_0800ECAC`): `unk0` -> `slot`.  Constants:
`CPU_LAYER_*`, `CPU_FLAG_*`, `CPU_ACT_*` (the old `unkDC` numbers) and
`CPU_OP_*` in `code_0800ECAC.h`.  Kept: `gUnk_02038590` (a linker symbol,
`ram_symbols.h`), `unkBA` (only ever zeroed), and the nav-graph table
`gUnk_08D63C28` (see room-tables.md, which proposes `gRoomNavGraphs`).  The
review documents in this directory cite the decompilation and keep the old
names.

*(guess)*: `CPU_ACT_LADDER`, `CPU_ACT_SLIDE`, `CpuIsAboveLadder`,
`CpuIsOnLadder`, `CpuIsHoldingThrow`, `CpuActLeaveFlight`, `CpuActWalkPause*`,
`CpuActDash*`, `CPU_ACT_ROOM_397` (what room 0x397 is).

Confirmed from the ROM data (`docs/design/CPU-HINTS.md`, `tools/cpu_hints.py`):
`homeRoom` (the room a CPU falls back to: the hub once it leaves the
opening; the warp target from rooms flagged 0x02 in `gUnk_08D6CD0C`);
`gCpuLinkedRooms` (the eight (area entrance room, boss room) pairs: every
first room carries `gUnk_08D6CD0C` flag 0x08, every second 0x01, same area);
`CPU_OP_SET_ABILITY` (only ever used with 0xFFFF = drop the ability); the nav
graph's node layout including the neighbour list in `filler26`.
