// MBF21 codepointers - a subset adequate for most modern DEH mods.
//
// MBF21 actions are bound to states like vanilla actions, but consume
// per-state parameters from state_t.args[8]. The DEH parser populates
// args via the "Args1..Args8" fields (see deh_frame.c), and the BEX
// [CODEPTR] section binds these functions to frame numbers by name
// (see deh_codeptr.c).
//
// Coverage:
//   monster: A_SpawnObject, A_MonsterProjectile, A_MonsterBulletAttack,
//            A_MonsterMeleeAttack, A_RadiusDamage, A_NoiseAlert,
//            A_JumpIfHealthBelow, A_JumpIfTargetInSight,
//            A_JumpIfTargetCloser, A_AddFlags, A_RemoveFlags
//   weapon:  A_WeaponProjectile, A_WeaponBulletAttack,
//            A_WeaponMeleeAttack, A_WeaponSound, A_WeaponJump
//
// Skipped (rarely needed, complex, or require flags2/weapon flag fields
// we don't have): A_HealChase, A_SeekTracer, A_FindTracer, A_ClearTracer,
// A_JumpIfTracer*, A_JumpIfFlagsSet, A_CheckAmmo, A_RefireTo,
// A_GunFlashTo, A_ConsumeAmmo, A_WeaponAlert.

#include <stdlib.h>     /* abs */

#include "doomdef.h"
#include "doomstat.h"
#include "p_local.h"
#include "p_mobj.h"
#include "info.h"
#include "tables.h"
#include "m_random.h"
#include "m_fixed.h"
#include "s_sound.h"
#include "d_player.h"
#include "p_pspr.h"
#include "sounds.h"
#include "r_main.h"     /* R_PointToAngle2 */

/* Forward decls - these are defined in p_enemy.c / p_pspr.c but lack
 * prototypes in the headers. */
void A_FaceTarget(mobj_t *actor);
void P_BulletSlope(mobj_t *mo);
void P_SetPsprite(player_t *player, int position, statenum_t stnum);

/* MBF21 angles are 16.16 fixed-point degrees; convert to engine angle_t. */
static angle_t mbf21_fixed_deg_to_angle(int fixed_deg)
{
    /* angle_t covers 360deg in 2^32, so 1deg == ANG1 (= ANG45 / 45).
     * fixed_deg is degrees << 16, so divide by FRACUNIT after scaling. */
    int64_t whole = (int64_t)fixed_deg * ANG1;
    return (angle_t)(whole >> FRACBITS);
}

/* MBF21 pitch is 16.16 fixed-point degrees; convert to a slope (Doom's
 * vertical-aim representation, 0 == straight ahead, +/- finetangent at
 * angle from horizontal). Approximate with the tangent of the angle. */
static fixed_t mbf21_pitch_to_slope(int fixed_deg)
{
    angle_t a = mbf21_fixed_deg_to_angle(fixed_deg);
    /* slope = tan(angle); use Doom's finetangent table.
     * finetangent is indexed by angle >> ANGLETOFINESHIFT in the
     * range (-ANG90+1)..(ANG90-1); clamp to keep within table. */
    if (a > ANG90 && a < (angle_t)(-(int)ANG90))
        return 0;
    return finetangent[(a >> ANGLETOFINESHIFT) & 8191];
}

// ---------------------------------------------------------------------
// Monster actions
// ---------------------------------------------------------------------

// A_SpawnObject(type, angle, x_off, y_off, z_off, x_vel, y_vel, z_vel)
void A_SpawnObject(mobj_t *actor)
{
    if (!actor) return;
    int *args = actor->state->args;
    mobjtype_t type = (mobjtype_t)args[0];
    angle_t angle = actor->angle + mbf21_fixed_deg_to_angle(args[1]);
    fixed_t x_off = args[2];
    fixed_t y_off = args[3];
    fixed_t z_off = args[4];
    fixed_t x_vel = args[5];
    fixed_t y_vel = args[6];
    fixed_t z_vel = args[7];

    if (type < 0 || type >= NUMMOBJTYPES) return;

    /* Offsets are in actor-relative coordinates: x_off is forward, y_off
     * is right, z_off is up. Rotate by angle into world space. */
    angle_t fine = angle >> ANGLETOFINESHIFT;
    fixed_t fx = FixedMul(x_off, finecosine[fine]) - FixedMul(y_off, finesine[fine]);
    fixed_t fy = FixedMul(x_off, finesine[fine])   + FixedMul(y_off, finecosine[fine]);

    mobj_t *spawned = P_SpawnMobj(actor->x + fx, actor->y + fy,
                                  actor->z + z_off, type);
    if (!spawned) return;

    spawned->angle = angle;
    /* Same rotation for velocity. */
    spawned->momx = FixedMul(x_vel, finecosine[fine]) - FixedMul(y_vel, finesine[fine]);
    spawned->momy = FixedMul(x_vel, finesine[fine])   + FixedMul(y_vel, finecosine[fine]);
    spawned->momz = z_vel;

    /* Inherit target from the actor so the spawned thing can do damage
     * attribution / track who fired it. */
    spawned->target = actor->target;
    spawned->tracer = actor;
}

// A_MonsterProjectile(type, angle, pitch, hoffset, voffset)
void A_MonsterProjectile(mobj_t *actor)
{
    if (!actor || !actor->target) return;
    int *args = actor->state->args;
    mobjtype_t type = (mobjtype_t)args[0];
    if (type < 0 || type >= NUMMOBJTYPES) return;

    /* Spawn the missile aimed at the target, then perturb the angle and
     * pitch by the requested offsets. */
    mobj_t *mo = P_SpawnMissile(actor, actor->target, type);
    if (!mo) return;

    mo->angle += mbf21_fixed_deg_to_angle(args[1]);
    angle_t fine = mo->angle >> ANGLETOFINESHIFT;
    int speed = mo->info->speed;
    mo->momx = FixedMul(speed, finecosine[fine]);
    mo->momy = FixedMul(speed, finesine[fine]);

    /* Apply pitch as a vertical velocity adjustment. */
    mo->momz += FixedMul(speed, mbf21_pitch_to_slope(args[2]));

    /* Horizontal/vertical offsets, in actor-relative coordinates. */
    fixed_t hoff = args[3];
    fixed_t voff = args[4];
    angle_t sideangle = (actor->angle - ANG90) >> ANGLETOFINESHIFT;
    mo->x += FixedMul(hoff, finecosine[sideangle]);
    mo->y += FixedMul(hoff, finesine[sideangle]);
    mo->z += voff;
}

// A_MonsterBulletAttack(hspread, vspread, numbullets, damagebase, damagedice)
void A_MonsterBulletAttack(mobj_t *actor)
{
    if (!actor || !actor->target) return;
    int *args = actor->state->args;
    angle_t hspread = mbf21_fixed_deg_to_angle(args[0]);
    /* vspread unused - vanilla hitscan slope is fixed by aim. */
    int numbullets = args[2];
    int damagebase = args[3];
    int damagedice = args[4] > 0 ? args[4] : 1;

    A_FaceTarget(actor);
    /* Play the actor's attacksound if any. */
    if (actor->info->attacksound)
        S_StartSound(actor, actor->info->attacksound);

    fixed_t slope = P_AimLineAttack(actor, actor->angle, MISSILERANGE);
    for (int i = 0; i < numbullets; ++i)
    {
        angle_t a = actor->angle + (((P_Random() - P_Random()) * hspread) >> 8);
        int dmg = ((P_Random() % damagedice) + 1) * damagebase;
        P_LineAttack(actor, a, MISSILERANGE, slope, dmg);
    }
}

// A_MonsterMeleeAttack(damagebase, damagedice, hitsound, range)
void A_MonsterMeleeAttack(mobj_t *actor)
{
    if (!actor || !actor->target) return;
    int *args = actor->state->args;
    int damagebase = args[0];
    int damagedice = args[1] > 0 ? args[1] : 1;
    int hitsound = args[2];
    fixed_t range = args[3] > 0 ? args[3] : MELEERANGE;

    A_FaceTarget(actor);

    /* Crude melee check: target within range and roughly in front. */
    fixed_t dx = abs(actor->x - actor->target->x);
    fixed_t dy = abs(actor->y - actor->target->y);
    fixed_t dist = P_AproxDistance(dx, dy);
    if (dist > range) return;
    if (!P_CheckSight(actor, actor->target)) return;

    int dmg = ((P_Random() % damagedice) + 1) * damagebase;
    if (hitsound > 0) S_StartSound(actor, hitsound);
    P_DamageMobj(actor->target, actor, actor, dmg);
}

// A_RadiusDamage(damage, radius)
// NOTE: vanilla P_RadiusAttack conflates damage and radius (radius is
// derived from damage). Here we honor the damage parameter and ignore
// the explicit radius - the falloff range is "damage" pixels, same as
// vanilla. Mods that need a much larger radius than damage will see a
// smaller blast than authored.
void A_RadiusDamage(mobj_t *actor)
{
    if (!actor) return;
    int damage = actor->state->args[0];
    P_RadiusAttack(actor, actor->target, damage);
}

// A_NoiseAlert()
void A_NoiseAlert(mobj_t *actor)
{
    if (!actor || !actor->target) return;
    P_NoiseAlert(actor->target, actor);
}

// A_JumpIfHealthBelow(state, health)
void A_JumpIfHealthBelow(mobj_t *actor)
{
    if (!actor) return;
    int state = actor->state->args[0];
    int health = actor->state->args[1];
    if (actor->health < health && state >= 0 && state < NUMSTATES)
        P_SetMobjState(actor, state);
}

// A_JumpIfTargetInSight(state, fov)
void A_JumpIfTargetInSight(mobj_t *actor)
{
    if (!actor || !actor->target) return;
    int state = actor->state->args[0];
    angle_t fov = mbf21_fixed_deg_to_angle(actor->state->args[1]);

    /* fov == 0 means "any direction" (no FOV check). */
    if (fov > 0)
    {
        angle_t to = R_PointToAngle2(actor->x, actor->y,
                                     actor->target->x, actor->target->y);
        angle_t delta = to - actor->angle;
        /* Use signed delta to bound by +/- fov/2. */
        if (delta > fov / 2 && delta < (angle_t)(-(int)(fov / 2))) return;
    }

    if (!P_CheckSight(actor, actor->target)) return;
    if (state >= 0 && state < NUMSTATES)
        P_SetMobjState(actor, state);
}

// A_JumpIfTargetCloser(state, distance)
void A_JumpIfTargetCloser(mobj_t *actor)
{
    if (!actor || !actor->target) return;
    int state = actor->state->args[0];
    fixed_t distance = actor->state->args[1];

    fixed_t dx = abs(actor->x - actor->target->x);
    fixed_t dy = abs(actor->y - actor->target->y);
    if (P_AproxDistance(dx, dy) < distance && state >= 0 && state < NUMSTATES)
        P_SetMobjState(actor, state);
}

// A_AddFlags(flags, flags2)
// flags2 is silently ignored - we don't have an MBF21 flags2 field.
void A_AddFlags(mobj_t *actor)
{
    if (!actor) return;
    actor->flags |= actor->state->args[0];
}

// A_RemoveFlags(flags, flags2)
void A_RemoveFlags(mobj_t *actor)
{
    if (!actor) return;
    actor->flags &= ~actor->state->args[0];
}

// ---------------------------------------------------------------------
// Weapon actions
// ---------------------------------------------------------------------

// A_WeaponProjectile(type, angle, pitch, hoffset, voffset)
void A_WeaponProjectile(player_t *player, pspdef_t *psp)
{
    if (!player || !psp || !psp->state) return;
    int *args = psp->state->args;
    mobjtype_t type = (mobjtype_t)args[0];
    if (type < 0 || type >= NUMMOBJTYPES) return;

    P_SpawnPlayerMissile(player->mo, type);
    /* P_SpawnPlayerMissile spawns at player angle aimed at autoaim; we
     * can't easily perturb the spawned missile from here without
     * refactoring it, so the angle/pitch/offset args are honored only
     * coarsely (ignored). Mods that rely heavily on per-shot angle
     * perturbation will see slight differences. */
    (void)args;
}

// A_WeaponBulletAttack(hspread, vspread, numbullets, damagebase, damagedice)
void A_WeaponBulletAttack(player_t *player, pspdef_t *psp)
{
    if (!player || !psp || !psp->state) return;
    int *args = psp->state->args;
    angle_t hspread = mbf21_fixed_deg_to_angle(args[0]);
    int numbullets = args[2];
    int damagebase = args[3];
    int damagedice = args[4] > 0 ? args[4] : 1;

    P_BulletSlope(player->mo);
    for (int i = 0; i < numbullets; ++i)
    {
        angle_t a = player->mo->angle + (((P_Random() - P_Random()) * hspread) >> 8);
        int dmg = ((P_Random() % damagedice) + 1) * damagebase;
        P_LineAttack(player->mo, a, MISSILERANGE, bulletslope, dmg);
    }
}

// A_WeaponMeleeAttack(damagebase, damagedice, berserkfactor, hitsound, range)
void A_WeaponMeleeAttack(player_t *player, pspdef_t *psp)
{
    if (!player || !psp || !psp->state) return;
    int *args = psp->state->args;
    int damagebase = args[0];
    int damagedice = args[1] > 0 ? args[1] : 1;
    int berserk = args[2] > 0 ? args[2] : 1;
    int hitsound = args[3];
    fixed_t range = args[4] > 0 ? args[4] : MELEERANGE;

    int dmg = ((P_Random() % damagedice) + 1) * damagebase;
    if (player->powers[pw_strength]) dmg *= berserk;

    angle_t angle = player->mo->angle;
    angle += (P_Random() - P_Random()) << 18;
    fixed_t slope = P_AimLineAttack(player->mo, angle, range);
    P_LineAttack(player->mo, angle, range, slope, dmg);

    if (linetarget)
    {
        if (hitsound > 0) S_StartSound(player->mo, hitsound);
        player->mo->angle = R_PointToAngle2(player->mo->x, player->mo->y,
                                            linetarget->x, linetarget->y);
    }
}

// A_WeaponSound(sound, fullvolume)
// fullvolume is ignored - vanilla S_StartSound always uses positional volume.
void A_WeaponSound(player_t *player, pspdef_t *psp)
{
    if (!player || !psp || !psp->state) return;
    int sound = psp->state->args[0];
    if (sound > 0) S_StartSound(player->mo, sound);
}

// A_WeaponJump(state, chance)
void A_WeaponJump(player_t *player, pspdef_t *psp)
{
    if (!player || !psp || !psp->state) return;
    int state = psp->state->args[0];
    int chance = psp->state->args[1];
    if (P_Random() < chance && state >= 0 && state < NUMSTATES)
        P_SetPsprite(player, psp - &player->psprites[0], state);
}
