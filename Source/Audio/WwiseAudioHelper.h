// Written by Alex Reham (alexreham.com) for a Lyra + Wwise integration.
// Shared for educational use: feel free to use and modify.

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"

#include "WwiseAudioHelper.generated.h"

class UAkAudioEvent;
class USkeletalMeshComponent;

/**
 * UWwiseAudioHelper
 *
 * Blueprint function library providing Lyra-aware Wwise helpers.
 *
 * PostEventSmart: drop-in replacement for the raw "Post Event" node in GameplayCue
 * Blueprints. For the local human player's pawn it redirects the emitter to the
 * PlayerCameraManager so the Wwise game object is co-located with the default
 * listener (distance = 0, no 3D attenuation artefacts). For every other actor
 * (bots, world objects) it posts directly on the actor, preserving full
 * distance-based spatialization.
 *
 * Usage in GCN_Weapon_*_Fire Blueprints:
 *   Use a single "Post Event Smart" node instead of the raw "Post Event" node
 *   (no Branch on player/bot needed):
 *     Event          → your UAkAudioEvent asset
 *     Actor          → the firing PAWN (e.g. MyTarget / Instigator), not the weapon actor
 *     WorldContext   → leave as Self / implicit
 */
UCLASS()
class LYRAGAME_API UWwiseAudioHelper : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/**
	 * Posts a Wwise event on Actor.
	 * If Actor is the local human player's pawn, the event is posted on the
	 * PlayerCameraManager instead (emitter co-located with the Wwise listener).
	 * For bots and all other actors the event is posted on Actor directly.
	 *
	 * Note: Actor must be the pawn itself. This function does not walk the owner
	 * chain, so passing a weapon actor will always take the "bot / world" path.
	 *
	 * @param Event              Wwise event to post.
	 * @param Actor              The actor that "owns" the sound (typically the firing pawn).
	 * @param WorldContextObject World context — leave as Self in Blueprint.
	 * @return Wwise PlayingID, or AK_INVALID_PLAYING_ID (0) on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wwise", meta = (WorldContext = "WorldContextObject"))
	static int32 PostEventSmart(UAkAudioEvent* Event, AActor* Actor, UObject* WorldContextObject = nullptr);

	/**
	 * Posts a Wwise event at a skeletal mesh socket position via a transient
	 * UAkComponent attached to the socket. Unlike PostEventAtLocation (which
	 * creates an anonymous emitter that ignores per-game-object switches),
	 * the AkComponent is itself a first-class Wwise game object, so optional
	 * per-event switches are applied on the same emitter that plays the sound.
	 *
	 * For bots: component sits exactly at the socket world location (full 3D
	 * spatialization).
	 * For the local human player: the component's world location is offset
	 * along the camera's right vector. This exaggerates the lateral displacement
	 * of the socket relative to the camera/listener, giving HRTF enough angular
	 * separation to correctly place close sounds (e.g. shell drops) in the right ear.
	 *
	 * The created UAkComponent is flagged SetAutoDestroy(true), so it cleans
	 * itself up once the Wwise event finishes playing.
	 *
	 * @param Event                  Wwise event to post.
	 * @param MeshComp               Skeletal mesh that owns the socket.
	 * @param SocketName             Name of the socket to use as the origin.
	 * @param SwitchGroup            Optional Wwise switch group to set on the
	 *                               transient component before posting. Leave
	 *                               as NAME_None to skip.
	 * @param SwitchValue            Switch value paired with SwitchGroup.
	 * @param LocalPlayerRightOffset Additional offset (cm) along the camera right
	 *                               vector applied only for the local player.
	 *                               Default 150 cm ≈ 1.5 m.
	 * @param WorldContextObject     World context — leave as Self in Blueprint.
	 * @return Wwise PlayingID, or 0 on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wwise", meta = (WorldContext = "WorldContextObject"))
	static int32 PostEventAtSocketWithLocalPlayerOffset(
		UAkAudioEvent* Event,
		USkeletalMeshComponent* MeshComp,
		FName SocketName,
		FName SwitchGroup = NAME_None,
		FName SwitchValue = NAME_None,
		float LocalPlayerRightOffset = 150.0f,
		UObject* WorldContextObject = nullptr);
};
