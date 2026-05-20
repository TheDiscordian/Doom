// BEX-style [CODEPTR] section parser.
//
// Vanilla DEH has a "Pointer N (Frame M)" syntax that says "use the
// action that frame M had originally for frame N." That requires the
// mod author to know an existing frame that uses each action, which is
// awkward and doesn't work at all for actions added by extensions like
// MBF21 (the new actions aren't bound to any vanilla frame).
//
// BEX added a [CODEPTR] section that assigns actions by name:
//   [CODEPTR]
//   FRAME 967 = A_SpawnObject
//   FRAME 968 = A_FaceTarget
//
// This file implements that section. The action name table covers all
// the commonly-used vanilla actions plus the MBF21 subset implemented
// in p_mbf21.c.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "doomtype.h"
#include "info.h"

#include "deh_defs.h"
#include "deh_io.h"
#include "deh_main.h"

/* Vanilla and MBF21 action prototypes. The actionf_t union allows both
 * actionf_p1 (mobj_t*) and actionf_p2 (player_t*, pspdef_t*) signatures,
 * so we use a void* cast at table-build time. */
struct mobj_s; typedef struct mobj_s mobj_t;
struct player_s; typedef struct player_s player_t;
struct pspdef_s; typedef struct pspdef_s pspdef_t;

/* Vanilla - defined in p_enemy.c, p_pspr.c, p_inter.c. */
void A_Light0(player_t *, pspdef_t *);
void A_WeaponReady(player_t *, pspdef_t *);
void A_Lower(player_t *, pspdef_t *);
void A_Raise(player_t *, pspdef_t *);
void A_Punch(player_t *, pspdef_t *);
void A_ReFire(player_t *, pspdef_t *);
void A_FirePistol(player_t *, pspdef_t *);
void A_Light1(player_t *, pspdef_t *);
void A_FireShotgun(player_t *, pspdef_t *);
void A_Light2(player_t *, pspdef_t *);
void A_FireShotgun2(player_t *, pspdef_t *);
void A_CheckReload(player_t *, pspdef_t *);
void A_OpenShotgun2(player_t *, pspdef_t *);
void A_LoadShotgun2(player_t *, pspdef_t *);
void A_CloseShotgun2(player_t *, pspdef_t *);
void A_FireCGun(player_t *, pspdef_t *);
void A_GunFlash(player_t *, pspdef_t *);
void A_FireMissile(player_t *, pspdef_t *);
void A_Saw(player_t *, pspdef_t *);
void A_FirePlasma(player_t *, pspdef_t *);
void A_BFGsound(player_t *, pspdef_t *);
void A_FireBFG(player_t *, pspdef_t *);
void A_BFGSpray(mobj_t *);
void A_Explode(mobj_t *);
void A_Pain(mobj_t *);
void A_PlayerScream(mobj_t *);
void A_Fall(mobj_t *);
void A_XScream(mobj_t *);
void A_Look(mobj_t *);
void A_Chase(mobj_t *);
void A_FaceTarget(mobj_t *);
void A_PosAttack(mobj_t *);
void A_Scream(mobj_t *);
void A_SPosAttack(mobj_t *);
void A_VileChase(mobj_t *);
void A_VileStart(mobj_t *);
void A_VileTarget(mobj_t *);
void A_VileAttack(mobj_t *);
void A_StartFire(mobj_t *);
void A_Fire(mobj_t *);
void A_FireCrackle(mobj_t *);
void A_Tracer(mobj_t *);
void A_SkelWhoosh(mobj_t *);
void A_SkelFist(mobj_t *);
void A_SkelMissile(mobj_t *);
void A_FatRaise(mobj_t *);
void A_FatAttack1(mobj_t *);
void A_FatAttack2(mobj_t *);
void A_FatAttack3(mobj_t *);
void A_BossDeath(mobj_t *);
void A_CPosAttack(mobj_t *);
void A_CPosRefire(mobj_t *);
void A_TroopAttack(mobj_t *);
void A_SargAttack(mobj_t *);
void A_HeadAttack(mobj_t *);
void A_BruisAttack(mobj_t *);
void A_SkullAttack(mobj_t *);
void A_Metal(mobj_t *);
void A_SpidRefire(mobj_t *);
void A_BabyMetal(mobj_t *);
void A_BspiAttack(mobj_t *);
void A_Hoof(mobj_t *);
void A_CyberAttack(mobj_t *);
void A_PainAttack(mobj_t *);
void A_PainDie(mobj_t *);
void A_KeenDie(mobj_t *);
void A_BrainPain(mobj_t *);
void A_BrainScream(mobj_t *);
void A_BrainDie(mobj_t *);
void A_BrainAwake(mobj_t *);
void A_BrainSpit(mobj_t *);
void A_SpawnSound(mobj_t *);
void A_SpawnFly(mobj_t *);
void A_BrainExplode(mobj_t *);

/* MBF21 - defined in p_mbf21.c. */
void A_SpawnObject(mobj_t *);
void A_MonsterProjectile(mobj_t *);
void A_MonsterBulletAttack(mobj_t *);
void A_MonsterMeleeAttack(mobj_t *);
void A_RadiusDamage(mobj_t *);
void A_NoiseAlert(mobj_t *);
void A_JumpIfHealthBelow(mobj_t *);
void A_JumpIfTargetInSight(mobj_t *);
void A_JumpIfTargetCloser(mobj_t *);
void A_AddFlags(mobj_t *);
void A_RemoveFlags(mobj_t *);
void A_WeaponProjectile(player_t *, pspdef_t *);
void A_WeaponBulletAttack(player_t *, pspdef_t *);
void A_WeaponMeleeAttack(player_t *, pspdef_t *);
void A_WeaponSound(player_t *, pspdef_t *);
void A_WeaponJump(player_t *, pspdef_t *);

typedef struct {
    const char *name;
    void (*func)();  /* unspecified signature; assigned into actionf_t.acv */
} action_entry_t;

#define A(name) { #name, (void (*)())name }
static const action_entry_t action_table[] = {
    /* Player/weapon */
    A(A_Light0), A(A_WeaponReady), A(A_Lower), A(A_Raise),
    A(A_Punch), A(A_ReFire), A(A_FirePistol), A(A_Light1),
    A(A_FireShotgun), A(A_Light2), A(A_FireShotgun2),
    A(A_CheckReload), A(A_OpenShotgun2), A(A_LoadShotgun2),
    A(A_CloseShotgun2), A(A_FireCGun), A(A_GunFlash),
    A(A_FireMissile), A(A_Saw), A(A_FirePlasma),
    A(A_BFGsound), A(A_FireBFG),
    /* Monster / general */
    A(A_BFGSpray), A(A_Explode), A(A_Pain), A(A_PlayerScream),
    A(A_Fall), A(A_XScream), A(A_Look), A(A_Chase),
    A(A_FaceTarget), A(A_PosAttack), A(A_Scream),
    A(A_SPosAttack), A(A_VileChase), A(A_VileStart),
    A(A_VileTarget), A(A_VileAttack), A(A_StartFire),
    A(A_Fire), A(A_FireCrackle), A(A_Tracer),
    A(A_SkelWhoosh), A(A_SkelFist), A(A_SkelMissile),
    A(A_FatRaise), A(A_FatAttack1), A(A_FatAttack2),
    A(A_FatAttack3), A(A_BossDeath), A(A_CPosAttack),
    A(A_CPosRefire), A(A_TroopAttack), A(A_SargAttack),
    A(A_HeadAttack), A(A_BruisAttack), A(A_SkullAttack),
    A(A_Metal), A(A_SpidRefire), A(A_BabyMetal),
    A(A_BspiAttack), A(A_Hoof), A(A_CyberAttack),
    A(A_PainAttack), A(A_PainDie), A(A_KeenDie),
    A(A_BrainPain), A(A_BrainScream), A(A_BrainDie),
    A(A_BrainAwake), A(A_BrainSpit), A(A_SpawnSound),
    A(A_SpawnFly), A(A_BrainExplode),
    /* MBF21 */
    A(A_SpawnObject), A(A_MonsterProjectile),
    A(A_MonsterBulletAttack), A(A_MonsterMeleeAttack),
    A(A_RadiusDamage), A(A_NoiseAlert),
    A(A_JumpIfHealthBelow), A(A_JumpIfTargetInSight),
    A(A_JumpIfTargetCloser), A(A_AddFlags), A(A_RemoveFlags),
    A(A_WeaponProjectile), A(A_WeaponBulletAttack),
    A(A_WeaponMeleeAttack), A(A_WeaponSound), A(A_WeaponJump),
    { NULL, NULL }
};
#undef A

static boolean FindAction(const char *name, void (**func)())
{
    if (!strcasecmp(name, "NULL")
     || !strcasecmp(name, "A_NULL")
     || !strcasecmp(name, "0"))
    {
        *func = NULL;
        return true;
    }

    /* Some BEX patches use the old Doom beta action name. We do not have
     * the beta projectile behavior, so bind it to Doom's shipped BFG fire
     * action rather than leaving the frame without a code pointer. */
    if (!strcasecmp(name, "FireOldBFG")
     || !strcasecmp(name, "A_FireOldBFG"))
    {
        *func = (void (*)()) A_FireBFG;
        return true;
    }

    for (const action_entry_t *e = action_table; e->name; ++e)
    {
        if (!strcasecmp(e->name, name)
         || (e->name[0] == 'A'
          && e->name[1] == '_'
          && !strcasecmp(e->name + 2, name)))
        {
            *func = e->func;
            return true;
        }
    }

    return false;
}

/* [CODEPTR] sections have no per-section state - every line is a
 * standalone "FRAME N = A_X" assignment. */
static void *DEH_CodeptrStart(deh_context_t *context, char *line)
{
    (void)context; (void)line;
    return (void *)(intptr_t)1; /* non-NULL tag so parser is called */
}

static void DEH_CodeptrParseLine(deh_context_t *context, char *line, void *tag)
{
    int frame_number;
    char action_name[64];
    char keyword[16];

    (void)tag;

    /* Match "FRAME N = A_X" with whitespace tolerance. */
    if (sscanf(line, " %15s %d = %63s",
               keyword, &frame_number, action_name) != 3
     || strcasecmp(keyword, "FRAME") != 0)
    {
        DEH_Warning(context, "[CODEPTR] line not understood: %s", line);
        return;
    }

    if (frame_number < 0 || frame_number >= NUMSTATES)
    {
        DEH_Warning(context, "[CODEPTR] invalid frame %d", frame_number);
        return;
    }

    void (*func)();
    if (!FindAction(action_name, &func))
    {
        DEH_Warning(context, "[CODEPTR] unknown action '%s'", action_name);
        return;
    }

    states[frame_number].action.acv = func;
}

deh_section_t deh_section_codeptr =
{
    "[CODEPTR]",
    NULL,
    DEH_CodeptrStart,
    DEH_CodeptrParseLine,
    NULL,
    NULL,
};
