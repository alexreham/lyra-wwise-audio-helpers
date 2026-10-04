// Written by Alex Reham (alexreham.com) for a Lyra + Wwise integration.
// Shared for educational use: feel free to use and modify.

#pragma once

#include "Animation/AnimNotifies/AnimNotify.h"

#include "AnimNotify_WwiseEvent.generated.h"

class UAkAudioEvent;

/**
 * UAnimNotify_WwiseEvent
 *
 * Posts a Wwise event on the owning actor when the notify fires.
 * Place on montage timelines (Reload, DryFire, Equip, etc.)
 * and assign the desired UAkAudioEvent in the Details panel.
 */
UCLASS(DisplayName = "Wwise Event", Meta = (ToolTip = "Posts a Wwise event on the owning actor"))
class LYRAGAME_API UAnimNotify_WwiseEvent : public UAnimNotify
{
	GENERATED_BODY()

public:

	UAnimNotify_WwiseEvent();

	virtual FString GetNotifyName_Implementation() const override;

	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;

	/** The Wwise event to post when this notify fires. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wwise")
	TObjectPtr<UAkAudioEvent> WwiseEvent;

	/** If true, post the event at the world-space position of SocketName instead of the actor origin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wwise")
	bool bUseSocketLocation = false;

	/** Socket or bone name to use when bUseSocketLocation is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wwise", Meta = (EditCondition = "bUseSocketLocation"))
	FName SocketName = NAME_None;

	/**
	 * If true, traces downward from the post location before posting the event and sets a Wwise Switch
	 * based on the physical surface hit (uses LyraContextEffectsSettings.SurfaceTypeToContextMap).
	 * Useful for footstep-like notifies on non-player meshes that lack a ContextEffectComponent.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wwise|Surface")
	bool bUseSurfaceSwitch = false;

	/** Wwise Switch Group to set when bUseSurfaceSwitch is true. Must match the Wwise project. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wwise|Surface", Meta = (EditCondition = "bUseSurfaceSwitch"))
	FName SurfaceSwitchGroup = TEXT("Surface_Type");

	/** How far downward (cm) to trace when resolving the surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wwise|Surface", Meta = (EditCondition = "bUseSurfaceSwitch"))
	float TraceDistance = 200.0f;
};
