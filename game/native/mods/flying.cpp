// The "Flying" mod (native/mods.h): the player flies.
//
// - Taking off: press Jump in the air (jump, then Jump again).
// - In flight there's no gravity and no falling damage. The stick (or WASD)
//   flies where the player looks: forward and back follow the view's pitch,
//   and strafing stays level. Holding Jump climbs and holding Crouch
//   descends. With nothing held, the player hovers.
// - Landing: touch the ground without holding Jump, and the player walks
//   again (a dive with Crouch, or forward while looking down).
// - The collision is the player's own, so walls, floors and ceilings still
//   stop the player, as does the level's edge (CommitUpdate stops a player
//   with nothing below).
// - Not on a ladder, a mounted gun, while carried (vehicles, scripted rides)
//   or dead. Aiming hovers in place, as aiming on foot stands still.
//
// NativeModPlayerFly (mods.cpp) calls FlyUpdate from CPlayerObject::
// BeginUpdate each frame, after the game's gravity and falling damage.

#include "game/input.h"
#include "game/player.h"
#include "math.h"
#include "mods_internal.h"
#include "native/mods.h"

namespace {

// Input events (CInputManager::GetValue; player.cpp's INPUT_*).
const int kMoveY = 7;   // forward is negative
const int kStrafe = 8;
const int kJump = 0xB;
const int kCrouch = 0xC;

// Speeds, in units a frame (dt is in frames at 60 Hz). Running is 0.092,
// and the player's capsule is 0.4 wide each side, so even at 30 Hz a frame's
// move stays within it and the collision can't be passed through.
const float kSpeed = 0.2f;     // flying across
const float kClimb = 0.12f;    // climbing or descending, held
const float kMaxSpeed = 0.2f;  // in all, whatever's held
const float kResponse = 0.2f;  // of the way to the wanted velocity, a frame

bool s_flying;
CVector3 s_velocity;  // the flight's, in the player's space (as m_localVelocity)

bool CanFly(const CPlayerObject* player) {
    return !player->m_attached && !player->m_onMG && !player->m_mounting && !player->m_climbing &&
           !player->m_dead && player->m_deathTime <= 0.0f;
}

}  // namespace

bool nmod::FlyUpdate(CPlayerObject* player, float dt, bool grounded, bool jump) {
    if (!On(NATIVE_MOD_FLYING) || !CanFly(player)) {
        s_flying = false;
        return false;
    }
    int port = player->m_controllerPort;
    bool up = NativeEventHeld(port, kJump);
    bool down = NativeEventHeld(port, kCrouch);
    if (!s_flying) {
        if (!jump || grounded) {
            return false;
        }
        // Take off, carrying on as the jump was going (less its gravity).
        s_flying = true;
        s_velocity = player->m_localVelocity;
    } else if (grounded && !up) {
        // Landed: the game's own walking took this frame.
        s_flying = false;
        return false;
    }

    // Where the stick says, along the view (m_pitch is negative looking up).
    float strafe = 0.0f;
    float forward = 0.0f;
    if (!player->m_zoomed) {
        strafe = 0.0078125f * (float)g_inputMgr.GetValue(port, kStrafe);
        forward = 0.0078125f * (float)-g_inputMgr.GetValue(port, kMoveY);
    }
    float pitch = player->m_pitch;
    CVector3 want;
    want.x = kSpeed * strafe;
    want.y = kSpeed * forward * cosf(pitch);
    want.z = -kSpeed * forward * sinf(pitch) + kClimb * ((up ? 1.0f : 0.0f) - (down ? 1.0f : 0.0f));
    float length = sqrtf(want.x * want.x + want.y * want.y + want.z * want.z);
    if (length > kMaxSpeed) {
        float k = kMaxSpeed / length;
        want.x *= k;
        want.y *= k;
        want.z *= k;
    }

    float k = kResponse * dt;
    if (k > 1.0f) {
        k = 1.0f;
    }
    s_velocity.x += (want.x - s_velocity.x) * k;
    s_velocity.y += (want.y - s_velocity.y) * k;
    s_velocity.z += (want.z - s_velocity.z) * k;
    player->m_localVelocity = s_velocity;

    // No fall to hurt on landing, and no explosion's throw holding the controls.
    player->m_falling = 0;
    player->m_fallStart = player->m_tm.m[3][2];
    player->m_flying = 0;
    return true;
}
