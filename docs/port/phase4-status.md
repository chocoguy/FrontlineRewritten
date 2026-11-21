# Phase 4 status

Hand-maintained. Phase 4 is mods and the sandbox ([ROADMAP](../../ROADMAP.md#phase-4--mods-and-sandbox-l)).

## The mod system (Rewritten, started 2026-11-02)

**What a mod is:** a named change the player turns on in the app.
- They appear in the Mac's Settings window (a Mods section) and on the TV's setup screen.
- Mods are Rewritten single player only. Classic shows them greyed out (the TV hides them, as it hides all of Rewritten's options), and multiplayer never sees them.
- **The game takes the choice as each level boots,** and it holds for that level. A new mission or stage is a new boot (the in-place reboot), and the game's statics start over then. So turning a mod on mid-level applies from the next level started. That keeps a level consistent: whatever a mod sets up as the level loads stays true for all of it.

**Where it lives:**

| Part | File | What |
|---|---|---|
| Shared header | `FrontlineDecompiled/include/native/mods.h` | the mod list (`NATIVE_MOD_*`), `NativeMods`, and the game's hooks |
| Catalog | `platform/src/mods.cpp` | each mod's key, title and sentence; which are on; `FRONTLINE_MODS` |
| The mods | `game/native/mods/` | `mods.cpp`: the per-level latch, shared helpers, and the hooks passed on to the mods; one file per mod |
| App | `apps/shared/ModsSection.swift`, `SettingsKeys.swift` | the toggles, kept in user defaults as `mod.<key>` |

**Adding a mod:**
1. Add a `NATIVE_MOD_*` entry in `mods.h`.
2. Add its catalog line in `platform/src/mods.cpp`, in the same order. The key is what the app saves the choice under, so never change it once it has shipped.
3. Write the mod in `game/native/mods/`, checking `nmod::On(NATIVE_MOD_…)`.
4. If the mod needs a new hook in the shared source, put the hook under `FRONTLINE_NATIVE`, declare it in `mods.h`, and rerun `regress.py`.

The app lists the catalog by itself; there's no Swift to write.

**Headless:** `FRONTLINE_MODS=enemy-knives,enemy-fists,flying,ignore-player` (keys, comma-separated) overrides the app's choice. `FRONTLINE_MODS=none` turns every mod off. Like the app's choice, it needs `FRONTLINE_MODE=rewritten`.

## Mods

### Knife Throwers (`enemy-knives`, 2026-11-02)

Enemy soldiers throw knives at the player instead of shooting: rapidly, and from any distance (`game/native/mods/enemy_knives.cpp`).

**How it works:**
- **No bullets.** `CAISoldierDoodad::FireAtCurrentTarget` and `FireMMGAtCurrentTarget` are the only ways a soldier fires at its target. For an enemy (AI team 2) with the mod on, a shot that fires bullets is dropped (`NativeModHoldFire`).
  - Grenades still fly, since they go through the same call with the grenade as the current weapon.
  - The script still aims and plays its firing animations, and nothing comes out of the gun.
  - Tanks and their guns are left alone.
- **Knives, on a timer of their own.** `CAISoldierObject::Update` calls `NativeModSoldierUpdate` every frame, whether or not Rewritten's AI is on. A living enemy whose target is the player throws a knife every 0.5–0.7 s while it has a clear line of sight to the player's chest (`CheckVisionLineToEnvironment`). A newcomer waits up to one interval, so a squad doesn't throw as one, and an enemy that can't see the player looks again 0.2 s later.
- **The knife** is the level's own thrown bullet type 5. Every single-player level has its model (`knifethrown.msh`). Its damage is 15 in every level and difficulty, and it stops where it lands (bounce 0).
  - It's thrown from the soldier's gun muzzle (`GetBulletEmitterMatrix`) at 0.75 units a frame, at full power. The level's own knife throw is 0.8 at most.
  - It's aimed at the chest, 0.4 below the player's eye, with up to 0.015 rad of spread.
  - Its collision class is a soldier's thrown rounds' (10, as `CWeapon::Init` gives them).
- **Unlimited range.** Knives already fly without gravity. Each knife lives as long as it takes to reach the player plus 30 frames, instead of the level's 240 (`field_0x90`, the fuse; `SetDetonationTime` only sets timed rounds). So it arrives from anywhere, and a miss is soon gone.
- **The pool.** Grenades share the 64 thrown bullets, so the mod keeps at most 32 knives in flight (tracked by when each one is gone).
- Randomness is the mod's own, so the game's sequence is unchanged.

**Left as they were:**
- What the enemies notice and when. They throw once their AI has targeted the player, so their sight and hearing ranges decide when that happens; after that, distance doesn't matter.
- Throw sounds. A knife makes no throwing sound (`CWeapon::FireBullet` plays one through `AEMS_SendEvent`, which this path skips). Impacts are the bullet code's own.

**Status:** built (Mac debug and release, tvOS simulator). The GameCube build is unchanged (`regress.py`: 0 dropped). Not yet playtested.

**To check by playing:**
- Enemies that spot you throw a steady stream of knives, and stop when you break line of sight.
- Knives reach you across a whole level and hurt (15 each).
- No enemy gunfire, and no mounted-MG fire, while grenades still come.
- Misses disappear soon after they land, and grenades still get thrown during a heavy knife fight (the pool isn't exhausted).
- Classic, and the mod off: the original game.

**Tuning:** the constants at the top of `enemy_knives.cpp`: interval, jitter, speed, spread, cap.

### Fist Fighters (`enemy-fists`, 2026-11-02)

Enemy soldiers never fire. They run at the player and, once in reach, punch at a very fast rate, yelling with every swing (`game/native/mods/enemy_fists.cpp`). The punches are also Unfair AI's (below).

**How it works:**
- **No fire at all.** Gun, mounted-gun and grenade throws at the target are all dropped (`NativeModHoldFire`).
- **The charge is the soldier script's own.** `SoldierA_Roamer` (one file, the same in all 19 levels) is run, or derived from, by 1302 of the 1381 enemy placements. Its `ChargeTarget` (logic state 59) runs at the target with the soldier's own run animations and A* pathing, so the mod steers the script into it rather than moving soldiers itself.
  - **Why the first version didn't charge (user's playtest, 2026-11-02):** the Attack state (39, and state 45) goes to `ChargeTarget` on event 0x7A, which its animation thread sends now and then, but only when the soldier may charge and hasn't given up.
    - "May charge" is instance word 8, which the soldier's behaviour setup writes as 0 or 1; many soldiers have 0.
    - "Given up" is its logic global 25.
    - And `ChargeTarget` stops at 0.9 times the soldier's attack distance (instance word 21), so it wouldn't come all the way.
  - **Now**, for an enemy whose target is the player, and only for `SoldierA_Roamer` scripts (other scripts lay out their instance data differently):
    - word 8 is kept at 1 and word 21 at 1.5, so a charge comes right up to the player;
    - the script is sent 0x7A every 0.5 s while the soldier is out of reach. Only states 39 and 45 handle that event (both by charging), so in any other state nothing happens.
  - **No cover.** `CAIFilterGlobal::ChooseCoverPointByType` and `GetClosestUsableCoverPoint` find none (`NativeModNoCover`), so a soldier attacks in the open instead of holding cover.
  - **Script decisions** (`kSites`, checked against all 19 levels with `tools/bsdis.py --check-sites game/native/mods/enemy_fists.cpp`; new hooks `NativeModScriptRandom` and `NativeModScriptTimer` sit beside the AI's and work whether or not Rewritten's AI is on):
    - `0x030B`: the charge roll (1–100, low charges) always charges. It's rolled on entering an attack.
    - `0x220F`: no wait at cover with the target out of sight.
    - `0x24C3`, `0x25C7`: the "can't reach the target: stop charging for 10 s" timers end after a frame.
    - Sound thread `0x0182`: the melee voice's 8–15 s wait before it can play again is a frame.
- **The yell.** Each strike sends the script the event its own melee strike sends: 37 with data 9. Only the sound thread handles 37, by going to the state the data names. Its state 9 plays voice 7 unless its global 7 says it played recently, and the roll above clears that a frame later. So every swing yells, cutting off the last one. `SoldierA_Roamer` scripts only.
- **The fists** (`FistsUpdate`, each frame from `CAISoldierObject::Update`). An enemy whose target is the player, within 2.2 across the ground and 1.5 up or down, strikes every 0.35 s.
  - It plays the scripts' own melee strikes, 14053 and 14052 in turn. In the script they're animation-thread states 290 and 397; in every level they're `pathing_cb` states.
  - Its move target is set to the player, so the strike turns the soldier toward the player as it swings.
  - A strike that faces the player within about 45° lands the scripts' melee hit: `CImpact` with 10 damage, 20 from behind, plus the hit marker and rumble. It's delivered straight to the player (`DamageClosestObject`), the same event `CreateImpact(3, 0, 0, 0, 0, 10, 20)` gives.
- **Holding the soldier.** While a soldier is in reach, and for 0.5 s after, the script's own animation requests are ignored (`NativeModBlockAnimation`, in `PlayAnim`). That way they don't cut the strikes short.
  - Death animations (`death_cb`, callback index 3) and dead soldiers always go through.
  - The strikes still send the script its "animation done" event (27), so its animation thread keeps going.
- Scripts that don't derive from `SoldierA_Roamer` don't charge: D-Day's standing MMGs, the 2_2 bar, cooks, tank crews and the like. They still punch if the player comes into reach.

**Unfair AI's punches (2026-11-02, user's request).** With Unfair AI (Hard plus the setup screen's toggle, Rewritten's AI on its "unfair" profile), enemies keep their guns and their own behaviour. But an enemy in reach of the player strikes exactly as above: every 0.35 s, 10 a hit, and the script's animations held while it does. It also holds its gun fire while striking. There's no yell, and nothing about its charging or cover changes.

**Unsure until played:**
- Whether the charge holds now. If soldiers still stand and "fire", the next lever is `ChargeAndAttackTarget` (state 60, event 0x95 from Attack, gated on its global 47 being 9).
- Whether shrinking word 21 changes anything else in an attack (it's also the range of the script's cover search, which finds nothing under this mod anyway).
- Which strike looks right. 14053 and 14052 both come from the script's melee states, but one may be crouched; the strikes alternate.
- Hit reactions: while in reach, a soldier that's shot doesn't flinch (its script's reactions are ignored), though it takes the damage and dies normally.

**Status:**
- 2026-11-02, first playtest by the user: no fire works, but soldiers didn't charge; they kept trying their guns.
- The charge fix, the yell and Unfair AI's punches are built (Mac debug and release, tvOS simulator). `bsdis` site check: all 19 levels match. They change no shared source, so the GameCube build is unchanged (`regress.py`: 0 dropped). Not yet re-tested.

**To check by playing:**
- Enemies stop shooting and throwing grenades, and run at you once they've spotted you, rather than standing and "firing".
- Each swing yells.
- Unfair AI without the mod: enemies shoot as usual, but punch rapidly (no yell) when you're within reach.
- In reach, they face you and swing about three times a second, each hit 10 (20 from behind), with the hit marker.
- They still die normally when shot, including mid-swing, and their drops appear.
- Walk away: they chase again.
- With Knife Throwers on as well, they throw knives on the way in.

**Tuning:** the constants at the top of `enemy_fists.cpp`: reach, strike interval, facing, hold, damage.

### Flying (`flying`, 2026-11-13)

The player flies (`game/native/mods/flying.cpp`).

**Controls:**
- **Take off:** jump, then press Jump again in the air.
- **Fly:** the stick (WASD) flies where you look. Forward and back follow the view's pitch; strafing stays level.
- **Climb and descend:** hold Jump to climb, hold Crouch to descend. With nothing held you hover.
- **Land:** touch the ground without holding Jump (descend with Crouch, or fly forward while looking down). You walk again from there.

**How it works:**
- A new hook, `NativeModPlayerFly`, runs in `CPlayerObject::BeginUpdate` after the game's gravity and falling damage. While flying, the mod sets the player's velocity (`m_localVelocity`, in the player's yawed space) itself, and the frame takes no jump impulse.
- **No gravity and no fall damage.** The mod keeps its own flight velocity, so the gravity the game added that frame is discarded, and it clears `m_falling` and moves `m_fallStart` to the current height every frame. Landing from any height doesn't hurt.
- **Speed:** 0.2 units a frame across (running is 0.092), 0.12 climbing or descending, 0.2 at most in all. The velocity eases toward what's held (a fifth of the way a frame), so starts and stops aren't instant.
- **Collision is the game's own:** the player's capsule (radius 0.4) still hits walls, floors and ceilings, and a frame's move stays inside the capsule's width, so walls can't be passed through. The level's edge still holds: `CommitUpdate` stops a player with nothing below.
- **Held buttons:** Jump and Crouch are press-only input events (`INPUTEVENT_PRESS`: 1 only on the frame they're pressed). A new `NativeEventHeld` in `input.cpp` reads whether they're held, from the pad's raw value (kept in `prev`) or the keyboard's held actions.
- **When it's off:** not on a ladder, a mounted gun, while carried (vehicles, scripted rides) or dead. Aiming (zoomed) hovers in place, as aiming on foot stands still. An explosion's throw is ignored while flying.

**Status:** built (Mac debug and release, tvOS simulator). The shared-source edits (`player.cpp`, `input.cpp`, `input.h`, `mods.h`) are all under `FRONTLINE_NATIVE`, and the GameCube build is unchanged (`regress.py`: 0 dropped). Not yet playtested.

**To check by playing:**
- Jump, then Jump again in mid-air: you stay up. Let go of everything: you hover without sinking.
- Look up and push forward: you climb along the view; look down: you dive. Strafing stays level.
- Hold Jump: you rise. Hold Crouch: you descend, and you walk again on touching down, with no fall damage from any height.
- Walls and ceilings stop you; you can't fly out past the level's edge.
- Pad (controller) and keyboard (Space / C) both climb and descend.
- Ladders, mounted guns and vehicles work as before. Classic, and the mod off: the original game.

**Unsure until played:**
- Whether levels have invisible ceilings, or kill or trigger volumes, that react badly to a player in the air. Flying over walls may also skip a level's scripted triggers.
- Whether landing on a slope is detected (`grounded` is a short ray under the feet or an upward contact). On a steep slope you may keep flying until you're on flatter ground.
- The feel: speeds and easing.

**Tuning:** the constants at the top of `flying.cpp`: speed, climb, top speed, response.

### Enemies Ignore You (`ignore-player`, 2026-11-13)

Enemies never notice the player. They don't see or hear you, and they won't target you even when you shoot them. They still fight your allies, and your allies them. The mod is all hooks; its rule is `NativeModIgnores` in `game/native/mods/mods.cpp`.

**How it works:** an enemy (AI team 2), soldier or tank, ignores the player's AI object (`IsPlayer()`):
- **Never a target.** `CAIObject::SetTarget` turns the player into no target, which covers scripts that set targets themselves (`AIChooseTargetByID`, as a script does when shot). `CAISoldierObject::ChooseTarget` and `CAITankObject::ChooseTarget` skip the player, so an enemy picks an ally in sight instead, or nothing.
- **Never heard.** `CAIObject::PropagateSoundToAllObjects` and `NativeAITellHeard` (Rewritten AI's callouts) don't pass enemies a sound whose source is the player's AI. So your gunfire, footsteps and voice don't alert them.
- Since no enemy targets you, nothing aimed at the target comes your way: no bullets, grenades or mounted-gun fire, and the other mods' knives and punches (they need the player as the target) don't come either.

**Left as they were:**
- Sounds with no AI source (`MakeNoiseFromInputTrigger`, script noises without a filter) still reach enemies. They may come looking, but still can't target you.
- Being shot: enemies take the damage, play their hit reactions and die normally. Whatever else their script does when hit (taking cover, running) still happens; only targeting you is blocked.
- Scripted level events that don't go through targets, such as artillery, scripted explosions, or a scripted gunner firing at a fixed spot, still hurt.

**Status:** built (Mac debug and release, tvOS simulator). The shared-source hooks (`AIObject.cpp`, `AISoldierObject.cpp`, `AITankObject.cpp`, `mods.h`) are under `FRONTLINE_NATIVE`, and the GameCube build is unchanged (`regress.py`: 0 dropped). Not yet playtested.

**To check by playing:**
- Walk past enemies, in the open and in front of them: no one shoots, throws or turns on you.
- Fire next to them: they don't come for you. Shoot one: it reacts and dies, and the others still leave you alone.
- Allies and enemies still fight each other (D-Day's beach, levels with squadmates).
- Tanks don't fire at you.
- Classic, and the mod off: the original game.

**Unsure until played:**
- Whether a level's progress needs an enemy to come at you, for example a script waiting for a soldier to spot you before something opens or a cutscene starts.

## Removed

### StG 44s for Everyone (built and removed 2026-11-02)

Every enemy gun was swapped for the enemies' StG 44 (type 22). The user dropped it after the first playtest for Knife Throwers. Its "borrowed files" mechanism, which loaded a file a level lacked from another level's archive (`NativeModFile` in `TLT_LoadFileFromLevelBigFile`), went with it. What it found, kept for later weapon mods:
- **Soldiers' poses come from their script alone.** The engine never picks an animation by weapon: `pathing_cb` and `aim_cb` play the state the script asks for, and the soldier scripts test the weapon type only for grenades.
- **The weapon decides only the hand and the grip.** `m_hand` picks `lt_hand` (even) or `rt_hand` (odd) in `ArmSoldier`, `SwitchSoldierWeapon` and `GiveEnemyGrenadeWeapon`. The model's hard point for that hand (LEFT `4C83C769`, RIGHT `D58A96D3`; muzzle `E6E804D5`) is where it's gripped.
- **Rifle and pistol models grip left; SMG models grip right.** A gun with the wrong hand for a soldier's pose is held in the hand that pose leaves hanging.
- Swapping a gun across that line means also giving it the original gun's hand, plus a matching grip point. The mod did that with copies of the properties and the model header.
- The enemy StG 44's model is in only 7 of the 19 single-player levels.
