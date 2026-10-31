#pragma once

#include "Player.hpp"

extern const CharacterPowerData g_CharacterPowerDataReimuA[9];
extern const CharacterPowerData g_CharacterPowerDataReimuB[9];
extern const CharacterPowerData g_CharacterPowerDataMarisaA[9];
extern const CharacterPowerData g_CharacterPowerDataMarisaB[9];

/* --- Mod「超高火力」--- */
// 非 const：numBullets 在 InitPlayerOverdrive() 时按实际生成条数写入。
// 注意是单元素表，且 power=999999，保证 FireSingleBullet 的查表循环一定停在这里。
extern CharacterPowerData g_OverdrivePowerDataReimuA[1];
extern CharacterPowerData g_OverdrivePowerDataReimuB[1];
extern CharacterPowerData g_OverdrivePowerDataMarisaA[1];
extern CharacterPowerData g_OverdrivePowerDataMarisaB[1];

// 全部可调参数，定义在 BulletData.cpp 末尾
extern i32 g_PlayerOverdriveEnabled;
extern i32 g_OverdrivePowerThreshold;
extern i32 g_OverdriveBaseExtra;
extern f32 g_OverdrivePowerPerExtra;
extern f32 g_OverdriveSpreadDegrees;
extern f32 g_OverdriveOriginSpread;
extern f32 g_OverdriveOriginForward;
extern i32 g_OverdriveFireInterval;
extern i32 g_OverdriveBaseCount[4];

// 生成上面的扇形弹幕表。惰性初始化，可重复调用。
void InitPlayerOverdrive();

// 按当前火力算出总弹数（只改 numBullets，不重建表）。
i32 ComputeOverdriveBulletCount(i32 baseCount, i32 currentPower);
