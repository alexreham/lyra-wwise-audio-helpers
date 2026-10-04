# Lyra + Wwise Audio Helpers

Small C++ helpers for integrating Wwise into Unreal Engine 5's **Lyra Starter Game**: player/bot-aware event posting, socket-based emitters with per-object switches, a Wwise AnimNotify for animation sounds, and a combat-intensity component that drives adaptive music.

Written by [Alex Reham](https://alexreham.com) while building a complete interactive audio system for Lyra with AI-assisted C++ (Claude Code). Free to use, modify and learn from.

**Tested with:** Unreal Engine 5.3.2 · Wwise 2023.1 · Lyra Starter Game

## What's inside

| File | What it does |
| --- | --- |
| `Source/Audio/WwiseAudioHelper.h/.cpp` | Blueprint function library with two nodes: **Post Event Smart** and **Post Event At Socket With Local Player Offset** (both described below) |
| `Source/Audio/AnimNotify_WwiseEvent.h/.cpp` | A **Wwise Event** AnimNotify for reloads, dry fire, equip and other animation sounds. Optional socket position and surface switch |
| `Source/Audio/LyraCombatIntensityComponent.h/.cpp` | Drives an adaptive music system: a smoothed `Combat_Intensity` RTPC, the `MusicState` (Idle / Combat1 / Combat2 / EndGame), plus damage, death, kill, respawn and hit SFX for the local player |

After a rebuild, both nodes appear in the Blueprint editor under the **Wwise** category.

**Post Event Smart** (`UWwiseAudioHelper::PostEventSmart`): posts an event on the PlayerCameraManager for the local player (where the Wwise listener lives) and on the actor itself for bots and everything else. Your own sounds stay centered, bots stay fully spatialized in 3D.

**Post Event At Socket With Local Player Offset** (`UWwiseAudioHelper::PostEventAtSocketWithLocalPlayerOffset`): spawns a temporary AkComponent on a skeletal mesh socket, sets an optional switch on it, posts the event, and destroys itself when the sound ends. Unlike `PostEventAtLocation`, per-object switches work. Used for surface-aware shell drops.

## Setup

1. Copy the files from `Source/Audio/` to `Source/LyraGame/Audio/` in your project. If you use another folder, update the `#include "Audio/..."` lines.
2. In `Source/LyraGame/LyraGame.Build.cs`, make sure `"AkAudio"` is listed in `PublicDependencyModuleNames`.
3. Close the editor and rebuild the project from your IDE or with `Build.bat`. Live Coding often fails when new classes are added.

The classes use the `LYRAGAME_API` macro. If your game module has a different name, change it to your module's API macro.

`AnimNotify_WwiseEvent` and `LyraCombatIntensityComponent` use Lyra classes, so they build only inside a Lyra-based project. `WwiseAudioHelper` has no Lyra dependency.

## Usage

**Weapon fire.** In each `GCN_Weapon_*_Fire` GameplayCue Blueprint, add a **Post Event Smart** node:

- **Event**: your Wwise fire event
- **Actor**: the firing **pawn** (the cue's target / instigator), not the weapon actor

**Animation sounds.** Open a montage (reload, equip, dry fire), add a notify of type **Wwise Event**, and pick the Wwise event in the Details panel. For shell drops, enable `bUseSocketLocation` with the ejection socket name and, if needed, `bUseSurfaceSwitch`.

**Adaptive music.** Add `LyraCombatIntensityComponent` to `B_Hero_ShooterMannequin` and assign the Wwise SFX events in its defaults. It expects these names in your Wwise project (change them in the `.cpp` if yours differ):

- RTPC `Combat_Intensity` (0–100)
- State Group `MusicState` with states `Idle`, `Combat1`, `Combat2`, `EndGame`

How intensity behaves (all values editable in the Details panel):

| Event | Intensity |
| --- | --- |
| Moving for 2 s | 33 (enters Combat) |
| Hitting an enemy | 66 |
| Getting a kill | 100 |
| 8 s without hitting anyone | eases back to 33 |
| 15 s without moving | eases back to 0 (Idle) |
| Your team reaches 10 kills | `EndGame`, locked until the match ends |

Rising and falling are smoothed with separate speeds. On each death the music state alternates between `Combat1` and `Combat2`, so you can use different death transitions in Wwise. Play your music event once from the Level Blueprint, not from the character.

The component relies on Lyra's gameplay messages (`Lyra.Damage.Message`, `Lyra.Elimination.Message`), health component and team subsystem, so it builds only in a Lyra-based project. Eliminations are broadcast on the server: it works in Standalone and Listen Server, not on dedicated-server clients.

## Lyra gotchas these helpers handle

- **Bots look like players.** `ALyraPlayerBotController` inherits from `APlayerController`, so `IsLocallyControlled()` and casts to PlayerController can return true for bots. The reliable check is `PlayerController->GetLocalPlayer() != nullptr`.
- **The listener is ~3 m from your character.** It sits on the PlayerCameraManager. Self-sounds posted on the character pan off-center and attenuate wrongly.
- **Switches are per game object.** Set a switch on the same object you post the event on. If the event goes to the camera, the switch must go there too.
- **`PostEventAtLocation` creates an anonymous emitter**, so per-object switches don't apply to it.
- **A new pawn is created on every respawn.** Anything that must survive death (kill count, which combat state is next) has to live outside the pawn — the component keeps it in static variables.
- **`BeginPlay` runs before the pawn is possessed.** The controller is still null there, so local-player checks have to wait until the first Tick.
- **Weapon sounds are owned by the weapon actor**, not the pawn. Walk up `GetOwner()` until you reach an `APawn` before checking for the local player.
- **Lyra's native audio keeps playing** under Wwise. Mute it, for example by setting the main submix output volume to -96 dB.

## More

- Demo reel: https://youtu.be/iRalK04OUps
- Write-up on Medium: https://medium.com/@alexreham/39ecdac1479d
- Case study: https://aiwg.miraheze.org/wiki/Lyra_Audio_Integration_Case_Study

Independent project. No affiliation with Epic Games, Audiokinetic or Anthropic.

## License

MIT. See [LICENSE](LICENSE).
