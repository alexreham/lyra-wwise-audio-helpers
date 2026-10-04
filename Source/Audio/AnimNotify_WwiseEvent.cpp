// Written by Alex Reham (alexreham.com) for a Lyra + Wwise integration.
// Shared for educational use: feel free to use and modify.

#include "Audio/AnimNotify_WwiseEvent.h"

#include "AkAudioEvent.h"
#include "AkGameplayStatics.h"
#include "Audio/WwiseAudioHelper.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Feedback/ContextEffects/LyraContextEffectsSubsystem.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AnimNotify_WwiseEvent)

UAnimNotify_WwiseEvent::UAnimNotify_WwiseEvent()
{
#if WITH_EDITORONLY_DATA
	NotifyColor = FColor(0, 180, 255); // Wwise-blue in the montage timeline
#endif
}

FString UAnimNotify_WwiseEvent::GetNotifyName_Implementation() const
{
	if (WwiseEvent)
	{
		return FString::Printf(TEXT("Wwise: %s"), *WwiseEvent->GetName());
	}
	return TEXT("Wwise: (None)");
}

void UAnimNotify_WwiseEvent::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	if (!WwiseEvent || !MeshComp)
	{
		return;
	}

	AActor* OwnerActor = MeshComp->GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	// Resolve which skeletal mesh owns the socket.
	// AnimNotifies fire on the character mesh (CharacterMesh0), but sockets like
	// ShellEject live on the weapon mesh which is a separate attached component.
	// Search all skeletal meshes on the actor so the correct world position is used.
	USkeletalMeshComponent* SocketMesh = nullptr;
	if (bUseSocketLocation && SocketName != NAME_None)
	{
		if (MeshComp->DoesSocketExist(SocketName))
		{
			SocketMesh = MeshComp;
		}
		else
		{
			TArray<USkeletalMeshComponent*> AllSkelMeshes;
			OwnerActor->GetComponents<USkeletalMeshComponent>(AllSkelMeshes);

			for (USkeletalMeshComponent* SM : AllSkelMeshes)
			{
				if (SM != MeshComp && SM->DoesSocketExist(SocketName))
				{
					SocketMesh = SM;
					break;
				}
			}

			// Also check child actors — weapon may be spawned as a separate attached actor.
			TArray<AActor*> AttachedActors;
			OwnerActor->GetAttachedActors(AttachedActors);
			for (AActor* Child : AttachedActors)
			{
				TArray<USkeletalMeshComponent*> ChildMeshes;
				Child->GetComponents<USkeletalMeshComponent>(ChildMeshes);
				for (USkeletalMeshComponent* CM : ChildMeshes)
				{
					if (!SocketMesh && CM->DoesSocketExist(SocketName))
					{
						SocketMesh = CM;
					}
				}
			}
		}
	}

	const bool bHasSocket = (SocketMesh != nullptr);

	// PostLocation is used by the surface trace; resolved from the socket mesh if found.
	const FVector PostLocation = bHasSocket
		? SocketMesh->GetSocketLocation(SocketName)
		: OwnerActor->GetActorLocation();

	// Resolve the Wwise game-object target for switches and non-socket PostEvent.
	// Switches are per-game-object, so SetSwitch must target the same actor that
	// will receive PostEvent. For the local player that's PlayerCameraManager;
	// for bots it stays as OwnerActor.
	AActor* SwitchTarget = OwnerActor;
	if (const APawn* Pawn = Cast<APawn>(OwnerActor))
	{
		if (const APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
		{
			if (PC->GetLocalPlayer() && PC->PlayerCameraManager)
			{
				SwitchTarget = PC->PlayerCameraManager;
			}
		}
	}

	// Optional: compute a Wwise Switch value based on the physical surface beneath the
	// post location. Hoisted out of the posting branch below so both the socket path
	// (applies on the transient UAkComponent) and the actor path (applies on SwitchTarget)
	// can reference the same value.
	FName SurfaceSwitchValue(TEXT("Default"));
	if (bUseSurfaceSwitch)
	{
		UWorld* World = MeshComp->GetWorld();
		if (World)
		{
			FHitResult Hit;
			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(AnimNotify_WwiseSurface), true, OwnerActor);
			QueryParams.bReturnPhysicalMaterial = true;

			const FVector TraceEnd = PostLocation + FVector(0.f, 0.f, -TraceDistance);
			if (World->LineTraceSingleByChannel(Hit, PostLocation, TraceEnd, ECC_Visibility, QueryParams))
			{
				if (const UPhysicalMaterial* PhysMat = Hit.PhysMaterial.Get())
				{
					// Use LyraContextEffectsSettings.SurfaceTypeToContextMap so the mapping stays
					// consistent with footsteps and landing (single source of truth in project settings).
					if (const ULyraContextEffectsSettings* Settings = GetDefault<ULyraContextEffectsSettings>())
					{
						if (const FGameplayTag* TagPtr = Settings->SurfaceTypeToContextMap.Find(PhysMat->SurfaceType))
						{
							// Tag format: "SurfaceType.Concrete" → extract suffix after last dot.
							const FString TagString = TagPtr->ToString();
							int32 DotIndex = INDEX_NONE;
							if (TagString.FindLastChar(TEXT('.'), DotIndex))
							{
								SurfaceSwitchValue = FName(*TagString.RightChop(DotIndex + 1));
							}
						}
					}
				}
			}
		}
	}

	// Post the event: spatially at the socket via a transient UAkComponent, or attached to the actor.
	if (bHasSocket)
	{
		// The helper creates a UAkComponent at the socket, applies the local-player right
		// offset, sets the switch on that component's own Wwise game object (so the switch
		// applies to the posted event), and auto-destroys after the sound finishes.
		UWwiseAudioHelper::PostEventAtSocketWithLocalPlayerOffset(
			WwiseEvent,
			SocketMesh,
			SocketName,
			bUseSurfaceSwitch ? SurfaceSwitchGroup : FName(NAME_None),
			bUseSurfaceSwitch ? SurfaceSwitchValue : FName(NAME_None));
	}
	else
	{
		// Non-socket path: set the switch on SwitchTarget (same game object that
		// PostEventSmart will target — CameraManager for local player, OwnerActor for bots).
		if (bUseSurfaceSwitch)
		{
			UAkGameplayStatics::SetSwitch(nullptr, SwitchTarget, SurfaceSwitchGroup, SurfaceSwitchValue);
		}

		// PostEventSmart posts on PlayerCameraManager for the local player (no 3D offset
		// from capsule root) and on the actor for bots (proper 3D spatialization).
		UWwiseAudioHelper::PostEventSmart(WwiseEvent, OwnerActor);
	}
}
