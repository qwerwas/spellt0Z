#include "ScarletFantasy.hpp"

#include "BulletManager.hpp"
#include "EffectManager.hpp"
#include "Enemy.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "Player.hpp"
#include "Rng.hpp"
#include "SoundPlayer.hpp"
#include "utils.hpp"
#include "ZunMath.hpp"

// BulletManager.cpp 里定义的"本次生成的是符卡弹"标记（SpawnSingleBullet 会读取）。
extern i32 g_SpellBulletSpawn;
extern u8 g_SpellBulletKind;

namespace ScarletFantasy
{

// 弹种类标记（写入 Bullet::spellKind）
enum SpellKind
{
    SCARLET_KIND_NONE = 0,
    SCARLET_KIND_BIG = 1,  // 波次生成的大玉（BUBBLE）
    SCARLET_KIND_SMALL = 2 // 转化生成的静止小玉（BALL）
};

i32 g_ScarletFantasyEnabled = 1;

// ---------------------------------------------------------------------------
// Lunatic 参数（ecldata6.ecl sub#44，skipForDifficulty 中 L 位）
// ---------------------------------------------------------------------------

// 原版 4 个 BULLETCIRCLE：sprite=9(BUBBLE etama4) color=1(红)，CIRCLE 模式。
struct WaveDef
{
    i32 count1;   // 每层弹数
    i32 count2;   // 层数
    f32 speed1;   // 第一层速度
    f32 speed2;   // 最后一层速度
    f32 angle2;   // 层间角偏移
    u32 flags;    // exFlags
    i32 exInt0;   // 0x20 行为持续帧数
    f32 exFloat0; // 0x20 行为：每帧速度增量
    f32 exFloat1; // 0x20 行为：每帧角度增量
};

static const WaveDef k_Waves[4] = {
    // A: t=180 首波 14×4，speed 4.0→1.8，angle2=-0.31416（扇形展开），flags=0x200（仅出生音效）
    {14, 4, 4.0f, 1.8f, -0.31416f, 0x200, 0, 0.0f, 0.0f},
    // B: t=180 12×1，flags=0x220 = 0x200|0x20（每帧旋转+加速），80 帧，+0.023 / -0.0245
    {12, 1, 2.0f, 1.0f, -0.01571f, 0x220, 80, 0.023f, -0.0245f},
    // C: t=260 17×1，60 帧，+0.026 / +0.0245
    {17, 1, 2.0f, 1.0f, -0.01571f, 0x220, 60, 0.026f, 0.0245f},
    // D: t=260 16×1，speed 1.0→1.0，80 帧，+0.023 / -0.0245
    {16, 1, 1.0f, 1.0f, -0.01571f, 0x220, 80, 0.023f, -0.0245f},
};

// 出生半径：原版从 Boss 中心射出；Mod 里绕角色一周出生，
// 取自机精灵外沿，视觉上即"从角色四周发射"。
static const f32 k_SpawnRadius = 20.0f;

// 转化（ExInsStage6Func8）：场上每个大玉 → 1 个静止小玉。
// sprite=3(BALL) color=1(红) speed=0 flags=8(慢出生特效) aimMode=1(FAN)。
static const i16 k_ConvertSprite = 3;
static const i16 k_ConvertColor = 1;

// 激活（ExInsStage6Func9/11）：全部静止小玉。
static const i16 k_ActivateColor = 2;   // 激活后变 color 2
static const f32 k_ActivateSpeed = 0.01f;
static const f32 k_ActivateAccel = 0.01f;
static const i32 k_ActivateFrames = 120; // 0x10 行为持续帧数（ex5Int0）

// 转化子程序 sub#41：最多 20 遍、每遍间隔 10 帧。
static const i32 k_MaxConvertPasses = 20;
static const i32 k_ConvertInterval = 10;

// 伤害。娱乐模组：把符卡弹伤害拉到极大，敌人"碰到就死"，
// 连原版要耗好几分钟的 Boss 也一发即杀（CalcDamageToEnemy 无每帧伤害封顶，
// damage 累加后直接返回）。spellHitCooldown 仅控制单发弹命中频率，不影响秒杀。
// 想调威力改这两个数即可（当前已设为不可能不秒杀的量级）。
static const i32 k_DamageBig = 10000000;
static const i32 k_DamageSmall = 10000000;
static const i32 k_HitInterval = 30;

// 单帧内允许连续推进的阶段数上限（防弹池满时状态机空转）。
static const i32 k_MaxPhaseAdvancePerFrame = 8;

// ---------------------------------------------------------------------------
// 状态机：对应 sub#44 主循环
//   波A → 转化 → ex11 → 波B → 转化 → ex9 → 波C → 转化 → ex11 → 波D → 转化 → ex9 → 回到波A
// 每段"转化"对应 CALL sub#41：第 0/10/20... 帧各转化一遍，
// 某一遍转化数为 0（大玉全部离场）或转化满 20 遍时立即结束并激活。
// ---------------------------------------------------------------------------

static bool s_Running = false;
static i32 s_WaveIdx = 0;    // 当前阶段 0..3
static i32 s_PhaseFrame = 0; // 当前阶段已经历帧数
static i32 s_PassesLeft = 0; // 本阶段剩余转化遍数

static void SpawnWave(Player *player, i32 waveIdx);
static i32 ConvertBigBubbles();
static void ActivateSmallBalls(Player *player, bool useDistanceAngle);
static void EnterPhase(Player *player, i32 waveIdx);

static void EnterPhase(Player *player, i32 waveIdx)
{
    // 与 ECL 一致：BULLETCIRCLE 之后紧跟 CALL sub#41，
    // 子程序的第一遍转化和波次生成在同帧完成；
    // 若第一遍就转化数为 0，sub#41 立即返回，主脚本同帧继续下一波。
    for (i32 advance = 0; advance < k_MaxPhaseAdvancePerFrame; advance++)
    {
        s_WaveIdx = waveIdx;
        s_PhaseFrame = 0;
        s_PassesLeft = k_MaxConvertPasses;

        SpawnWave(player, waveIdx);

        i32 converted = ConvertBigBubbles();
        s_PassesLeft--;

        if (converted != 0 && s_PassesLeft > 0)
        {
            return; // 进入 10 帧等待，下一遍转化在 s_PhaseFrame == 10
        }

        // 转化结束：激活全部静止小玉（波 0/2 用 ex11 随机角，波 1/3 用 ex9 距离定角）
        ActivateSmallBalls(player, (s_WaveIdx & 1) == 0);
        waveIdx = (s_WaveIdx + 1) & 3;
    }
}

void Update(Player *player)
{
    if (g_ScarletFantasyEnabled == 0)
    {
        return;
    }

    // 只适配博丽灵梦；马里萨完全走原版。
    if (g_GameManager.character != CHARA_REIMU)
    {
        s_Running = false;
        return;
    }

    // 死亡 / 出生中不发射；有对话时也不发射（与原版开火条件一致）。
    // 松开 Z 只重置状态机，已射出的弹继续按原轨迹飞。
    if (player->playerState != PLAYER_STATE_ALIVE && player->playerState != PLAYER_STATE_INVULNERABLE)
    {
        s_Running = false;
        return;
    }
    if (!IS_PRESSED(TH_BUTTON_SHOOT) || g_Gui.HasCurrentMsgIdx())
    {
        s_Running = false;
        return;
    }

    if (!s_Running)
    {
        s_Running = true;
        EnterPhase(player, 0);
        return;
    }

    s_PhaseFrame++;
    if (s_PhaseFrame % k_ConvertInterval == 0)
    {
        i32 converted = ConvertBigBubbles();
        s_PassesLeft--;
        if (converted == 0 || s_PassesLeft <= 0)
        {
            ActivateSmallBalls(player, (s_WaveIdx & 1) == 0);
            EnterPhase(player, (s_WaveIdx + 1) & 3);
        }
    }
}

void Reset()
{
    s_Running = false;
    s_WaveIdx = 0;
    s_PhaseFrame = 0;
    s_PassesLeft = 0;
}

// ---------------------------------------------------------------------------
// 一波大玉：CIRCLE 模式，绕角色一周出生
// ---------------------------------------------------------------------------

static void SpawnWave(Player *player, i32 waveIdx)
{
    const WaveDef &wave = k_Waves[waveIdx];

    // f32_0 = rand(0..2π) - π（SETFLOATRANDMIN 2π, -π）
    f32 baseAngle = g_Rng.GetRandomF32InRange(ZUN_PI * 2) - ZUN_PI;

    EnemyBulletShooter props;
    props.sprite = 9; // BUBBLE（etama4）
    props.spriteOffset = 1;
    props.aimMode = CIRCLE;
    props.angle1 = baseAngle;
    props.angle2 = wave.angle2;
    props.speed1 = wave.speed1;
    props.speed2 = wave.speed2;
    props.count1 = wave.count1;
    props.count2 = wave.count2;
    props.flags = wave.flags;
    props.sfx = SOUND_SHOOT; // 原版未发 BULLETSOUND，bulletProps 零初始化即 SOUND_SHOOT
    props.exInts[0] = wave.exInt0;
    props.exFloats[0] = wave.exFloat0;
    props.exFloats[1] = wave.exFloat1;

    f32 playerX = player->positionCenter.x;
    f32 playerY = player->positionCenter.y;

    for (i32 layer = 0; layer < wave.count2; layer++)
    {
        for (i32 i = 0; i < wave.count1; i++)
        {
            // 与 BulletManager::SpawnSingleBullet 的 CIRCLE 分支同一公式
            f32 angle = i * ZUN_2PI / wave.count1 + layer * wave.angle2 + baseAngle;
            // 每发在角色周围的环上、沿自己的发射方向出生
            props.position.x = playerX + ZUN_COSF(angle) * k_SpawnRadius;
            props.position.y = playerY + ZUN_SINF(angle) * k_SpawnRadius;
            props.position.z = 0.0f;

            g_SpellBulletSpawn = 1;
            g_SpellBulletKind = SCARLET_KIND_BIG;
            g_BulletManager.SpawnSingleBullet(&props, i, layer, 0.0f);
            g_SpellBulletSpawn = 0;
        }
    }

    // SpawnBulletPattern 原本负责的出生音效（flags & 0x200），每波只响一次
    if ((wave.flags & 0x200) != 0)
    {
        g_SoundPlayer.PlaySoundByIdx(props.sfx);
    }
}

// ---------------------------------------------------------------------------
// 转化：场上每个大玉 → 1 个静止小玉（ExInsStage6Func8）
// 只转化本符卡的大玉；原版会转化场上所有大弹，但那样会把关卡/Boss
// 的敌弹也变成小玉，改变原版游戏性，这里特意收窄。
// ---------------------------------------------------------------------------

static i32 ConvertBigBubbles()
{
    i32 converted = 0;

    EnemyBulletShooter props;
    props.sprite = k_ConvertSprite;
    props.spriteOffset = k_ConvertColor;
    props.speed1 = 0.0f;
    props.count1 = 1;
    props.count2 = 1;
    props.flags = 8;    // 慢出生特效
    props.aimMode = FAN;
    props.sfx = NO_SOUND;

    Bullet *bullet = &g_BulletManager.bullets[0];
    for (i32 i = 0; i < ARRAY_SIZE_SIGNED(g_BulletManager.bullets); i++, bullet++)
    {
        if (bullet->isPlayerSpell == 0 || bullet->spellKind != SCARLET_KIND_BIG)
        {
            continue;
        }
        if (bullet->state == 0 || bullet->state == 5)
        {
            continue;
        }
        if (bullet->sprites.spriteBullet.sprite == NULL)
        {
            continue;
        }

        props.position = bullet->pos;
        props.position.z = 0.0f;
        props.angle1 = g_Rng.GetRandomF32InRange(ZUN_PI * 2) - ZUN_PI;

        g_SpellBulletSpawn = 1;
        g_SpellBulletKind = SCARLET_KIND_SMALL;
        g_BulletManager.SpawnSingleBullet(&props, 0, 0, 0.0f);
        g_SpellBulletSpawn = 0;

        // 原版 ex8 也是不管弹池是否满都计数
        converted++;
    }
    return converted;
}

// ---------------------------------------------------------------------------
// 激活：全部静止小玉开始螺旋加速（ExInsStage6Func9/11）
// useDistanceAngle = true  → ex9：角度 = 与角色距离 × π/256 + 随机修正
// useDistanceAngle = false → ex11：纯随机角
// ---------------------------------------------------------------------------

static void ActivateSmallBalls(Player *player, bool useDistanceAngle)
{
    f32 randomAngleModifier = g_Rng.GetRandomF32InRange(ZUN_PI * 2) - ZUN_PI;
    g_EffectManager.SpawnParticles(PARTICLE_EFFECT_UNK_12, &player->positionCenter, 1, COLOR_WHITE);

    Bullet *bullet = &g_BulletManager.bullets[0];
    for (i32 i = 0; i < ARRAY_SIZE_SIGNED(g_BulletManager.bullets); i++, bullet++)
    {
        if (bullet->isPlayerSpell == 0 || bullet->spellKind != SCARLET_KIND_SMALL)
        {
            continue;
        }
        if (bullet->state == 0 || bullet->state == 5)
        {
            continue;
        }
        if (bullet->sprites.spriteBullet.sprite == NULL)
        {
            continue;
        }
        if (bullet->speed != 0.0f) // 只激活静止小玉
        {
            continue;
        }

        bullet->exFlags |= 0x10;
        bullet->spriteOffset = k_ActivateColor;
        g_AnmManager->SetActiveSprite(&bullet->sprites.spriteBullet,
                                      bullet->sprites.spriteBullet.baseSpriteIndex + k_ActivateColor);
        bullet->speed = k_ActivateSpeed;
        bullet->timer.InitializeForPopup();
        bullet->ex5Int0 = k_ActivateFrames;

        f32 angle;
        if (useDistanceAngle)
        {
            f32 dx = player->positionCenter.x - bullet->pos.x;
            f32 dy = player->positionCenter.y - bullet->pos.y;
            f32 distance = dx * dx + dy * dy;
            if (distance > 0.1f)
            {
                distance = ZUN_SQRTF(distance);
            }
            else
            {
                distance = 0.0f;
            }
            angle = (distance * ZUN_PI) / 256.0f + randomAngleModifier;
        }
        else
        {
            angle = g_Rng.GetRandomF32InRange(ZUN_PI * 2) - ZUN_PI;
        }
        sincosmul(&bullet->ex4Acceleration, angle, k_ActivateAccel);
    }
}

// ---------------------------------------------------------------------------
// 伤害：由 Player::CalcDamageToEnemy 对每个敌人调用
// ---------------------------------------------------------------------------

void DamageEnemy(const ZunVec3 *enemyPos, const ZunVec3 *enemyHitboxSize, i32 *damage, bool duringBomb)
{
    if (g_ScarletFantasyEnabled == 0)
    {
        return;
    }

    ZunVec3 enemyTopLeft;
    ZunVec3 enemyBottomRight;
    ZunVec3::SetVecCorners(&enemyTopLeft, &enemyBottomRight, enemyPos, enemyHitboxSize);

    Bullet *bullet = &g_BulletManager.bullets[0];
    for (i32 i = 0; i < ARRAY_SIZE_SIGNED(g_BulletManager.bullets); i++, bullet++)
    {
        if (bullet->isPlayerSpell == 0)
        {
            continue;
        }
        // 只统计已进入飞行状态的弹（出生特效中的 2/3/4 与消散中的 5 不判定）
        if (bullet->state != 1)
        {
            continue;
        }
        if (bullet->spellHitCooldown > 0)
        {
            continue;
        }

        ZunVec3 bulletTopLeft;
        ZunVec3 bulletBottomRight;
        ZunVec3::SetVecCorners(&bulletTopLeft, &bulletBottomRight, &bullet->pos, &bullet->sprites.grazeSize);

        if (bulletTopLeft.y > enemyBottomRight.y || bulletTopLeft.x > enemyBottomRight.x ||
            bulletBottomRight.y < enemyTopLeft.y || bulletBottomRight.x < enemyTopLeft.x)
        {
            continue;
        }

        i32 dmg = bullet->spellKind == SCARLET_KIND_BIG ? k_DamageBig : k_DamageSmall;
        if (duringBomb)
        {
            dmg = dmg / 3 != 0 ? dmg / 3 : 1;
        }
        *damage += dmg;
        bullet->spellHitCooldown = k_HitInterval;

        // 命中反馈（与自机弹命中粒子一致）
        g_EffectManager.SpawnParticles(PARTICLE_EFFECT_UNK_5, &bullet->pos, 1, COLOR_WHITE);
    }
}

} // namespace ScarletFantasy
