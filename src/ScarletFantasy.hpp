#pragma once

#include "inttypes.hpp"
#include "ZunMath.hpp"

struct Player;

// Mod「紅色の幻想郷」
// 把博丽灵梦的普通攻击（按住 Z）替换成 6 面 Boss 芙兰朵璐的最终符卡
// 「紅色の幻想郷」最终阶段的弹幕（仅还原 Lunatic 难度）。
//
// 实现方式：参数逐条来自 紅魔郷ST.DAT -> ecldata6.ecl sub#44 的反汇编
// （Lunatic 分支），弹幕本体走游戏原版敵弹系统（BulletManager），
// 因此贴图 / 颜色 / 出生特效 / exFlags 运动与原版完全一致；
// 循环结构（4 波大玉 + 每波后转化 + ex9/ex11 交替激活）复刻 ECL 虚拟机时序。
namespace ScarletFantasy
{
    // 总开关：改成 0 即完全恢复原版自机弹（其它代码路径不受影响）。
    extern i32 g_ScarletFantasyEnabled;

    // 每帧驱动状态机，由 Player::OnUpdate 调用。
    void Update(Player *player);

    // 供 Player::CalcDamageToEnemy 调用：符卡弹对单个敌人造成伤害。
    // duringBomb = 玩家正在放炸弹（与原版自机弹一样伤害减为 1/3）。
    void DamageEnemy(const ZunVec3 *enemyPos, const ZunVec3 *enemyHitboxSize, i32 *damage, bool duringBomb);

    // 重置状态机（新开一轮 / 读档时调用）。
    void Reset();
}
