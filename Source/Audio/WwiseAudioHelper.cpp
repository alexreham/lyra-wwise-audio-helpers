// Written by Alex Reham (alexreham.com) for a Lyra + Wwise integration.
// Shared for educational use: feel free to use and modify.

#include "Audio/WwiseAudioHelper.h"

#include "AkAudioEvent.h"
#include "AkComponent.h"
#include "AkGameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(WwiseAudioHelper)

int32 UWwiseAudioHelper::PostEventSmart(UAkAudioEvent* Event, AActor* Actor, UObject* WorldContextObject)
{
	if (!Event || !Actor)
	{
		return 0;
	}

	AActor* PostTarget = Actor;

	// Redirect local player pawn → PlayerCameraManager so the Wwise emitter is
	// co-located with the default listener. This removes the capsule-to-camera offset
	// (a few meters in Lyra's third-person camera) that causes orientation-dependent
	// volume drops and off-center panning on self-produced sounds.
	//
	// Local player check: IsLocallyControlled() is not enough, because Lyra's bots
	// (ALyraPlayerBotController, an AI controller) also count as "local" in standalone
	// and on a listen server. Bots fail the Cast<APlayerController>, and remote players
	// on a listen server have no ULocalPlayer, so Cast + GetLocalPlayer() != nullptr
	// matches only the human player on this machine.
	if (const APawn* Pawn = Cast<APawn>(Actor))
	{
		if (const APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
		{
			if (PC->GetLocalPlayer() != nullptr)
			{
				// Local human player — post on the camera manager (= the Wwise listener object).
				if (APlayerCameraManager* CamMgr = PC->PlayerCameraManager)
				{
					PostTarget = CamMgr;
				}
			}
		}
	}

	return UAkGameplayStatics::PostEvent(Event, PostTarget, 0, FOnAkPostEventCallback(), false);
}

int32 UWwiseAudioHelper::PostEventAtSocketWithLocalPlayerOffset(
	UAkAudioEvent* Event,
	USkeletalMeshComponent* MeshComp,
	FName SocketName,
	FName SwitchGroup,
	FName SwitchValue,
	float LocalPlayerRightOffset,
	UObject* WorldContextObject)
{
	if (!Event || !MeshComp)
	{
		return 0;
	}

	AActor* OwnerActor = MeshComp->GetOwner();
	if (!OwnerActor)
	{
		return 0;
	}

	// Determine whether this is the local human player.
	// OwnerActor is the mesh's owner — for weapons that's the weapon actor (e.g. B_Pistol_C),
	// not the pawn. Walk up the owner chain to find the pawn that actually drives the controller.
	// Same local player check as PostEventSmart: Cast<APlayerController> filters out
	// bots (they use an AI controller), GetLocalPlayer() != nullptr filters out remote players.
	AActor* PawnActor = OwnerActor;
	while (PawnActor && !Cast<APawn>(PawnActor))
	{
		PawnActor = PawnActor->GetOwner();
	}
	const APawn* Pawn = Cast<APawn>(PawnActor);

	bool bIsLocalPlayer = false;
	const APlayerController* LocalPC = nullptr;
	if (Pawn)
	{
		if (const APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
		{
			if (PC->GetLocalPlayer() != nullptr)
			{
				bIsLocalPlayer = true;
				LocalPC = PC;
			}
		}
	}

	// Resolve the final world-space position. The bot case is the raw socket world
	// location; the local player case is offset laterally along the camera-right
	// vector so HRTF has enough angular separation for close-proximity sounds.
	const FVector SocketLocation = MeshComp->DoesSocketExist(SocketName)
		? MeshComp->GetSocketLocation(SocketName)
		: OwnerActor->GetActorLocation();

	FVector FinalLocation = SocketLocation;
	if (bIsLocalPlayer && LocalPC && LocalPC->PlayerCameraManager)
	{
		const FVector CameraRight = LocalPC->PlayerCameraManager->GetActorRightVector();
		FinalLocation = SocketLocation + CameraRight * LocalPlayerRightOffset;
	}

	// Spawn a transient UAkComponent on the owner actor. UAkComponent is itself
	// a Wwise game object — unlike PostEventAtLocation (anonymous emitter), this
	// supports per-game-object switches set on the same emitter that plays the
	// sound. Attaching to the socket mesh keeps the emitter co-located with the
	// weapon for the (short) duration of the event.
	UAkComponent* AkComp = NewObject<UAkComponent>(OwnerActor);
	if (!AkComp)
	{
		return 0;
	}

	AkComp->SetupAttachment(MeshComp, SocketName);
	AkComp->RegisterComponent();

	// SetWorldLocation after attachment keeps the attachment relationship but
	// computes a non-zero relative offset — used by the local player path so
	// the emitter sits FinalLocation-relative-to-socket and moves with the weapon.
	AkComp->SetWorldLocation(FinalLocation);

	// Auto-destroy once the event finishes so we do not leak components.
	AkComp->SetAutoDestroy(true);

	// SetSwitch MUST run before PostAkEvent — Wwise captures the switch state at
	// the moment the event is posted. Using the AkComponent's own SetSwitch
	// (not UAkGameplayStatics) so the switch is scoped to this emitter.
	if (SwitchGroup != NAME_None && SwitchValue != NAME_None)
	{
		AkComp->SetSwitch(nullptr, SwitchGroup.ToString(), SwitchValue.ToString());
	}

	return AkComp->PostAkEvent(Event, 0, FOnAkPostEventCallback());
}
