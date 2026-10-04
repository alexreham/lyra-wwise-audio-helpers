// Written by Alex Reham (alexreham.com) for a Lyra + Wwise integration.
// Shared for educational use under the MIT License: feel free to use and modify.

#pragma once

#include "Components/ActorComponent.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GameplayTagContainer.h"

#include "LyraCombatIntensityComponent.generated.h"

class UAkAudioEvent;
struct FLyraVerbMessage;

/**
 * ULyraCombatIntensityComponent
 *
 * Actor component that drives the Wwise "Combat_Intensity" RTPC (0-100)
 * based on player movement, weapon hits, and eliminations.
 *
 * Intensity levels:
 *   33  - Player has been moving for MoveTimeToActivate seconds
 *   66  - Player dealt damage to an enemy ("Lyra.Damage.Message")
 *   100 - Player scored an elimination ("Lyra.Elimination.Message")
 *
 * Decay:
 *   No damage dealt for ShootDecayDelay sec  -> lerp back to 33
 *   No movement for MoveDecayDelay sec       -> lerp back to 0
 *
 * Add this component to B_Hero_ShooterMannequin.
 * Requires "AkAudio" in LyraGame.Build.cs PrivateDependencyModuleNames.
 *
 * Note: "Lyra.Elimination.Message" is broadcast under WITH_SERVER_CODE only.
 * It works correctly in Standalone and Listen Server. On dedicated server clients
 * the elimination event will not fire; use the OnDeathStarted delegate as fallback.
 */
UCLASS(Blueprintable, Meta = (BlueprintSpawnableComponent))
class LYRAGAME_API ULyraCombatIntensityComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	ULyraCombatIntensityComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Returns the current combat intensity value [0..100] */
	UFUNCTION(BlueprintPure, Category = "Lyra|Audio")
	float GetCombatIntensity() const { return CombatIntensity; }

	/** Re-evaluates MusicState immediately (e.g. call from Blueprint on respawn via Event OnReset). */
	UFUNCTION(BlueprintCallable, Category = "Lyra|Audio")
	void ResetDeathState();

protected:

	// Called once from Tick when IsLocallyControlled() first becomes true.
	// Returns true if registration succeeded.
	bool TryRegisterListeners();

	void OnDamageMessage(FGameplayTag Channel, const FLyraVerbMessage& Message);
	void OnEliminationMessage(FGameplayTag Channel, const FLyraVerbMessage& Message);

	UFUNCTION()
	void OnLocalPlayerDeath(AActor* OwningActor);

	void OnEliminationForEndGame(FGameplayTag Channel, const FLyraVerbMessage& Message);

	void ApplyCombatIntensity(float NewValue);
	void SendRTPCIfChanged();
	void UpdateMusicState();

	void TickMovement(float DeltaTime, double CurrentTime);
	void TickDecay(double CurrentTime);

protected:

	/** Minimum XY speed (cm/s) the owner must exceed to count as "moving" */
	UPROPERTY(EditAnywhere, Category = "Combat Intensity", meta = (ClampMin = "0.0"))
	float MoveVelocityThreshold = 200.0f;

	/** Seconds of continuous movement before intensity rises to 33 */
	UPROPERTY(EditAnywhere, Category = "Combat Intensity", meta = (ClampMin = "0.0"))
	float MoveTimeToActivate = 2.0f;

	/** Seconds without dealing damage before intensity decays toward 33 */
	UPROPERTY(EditAnywhere, Category = "Combat Intensity", meta = (ClampMin = "0.0"))
	float ShootDecayDelay = 8.0f;

	/** Seconds without movement before intensity decays toward 0 */
	UPROPERTY(EditAnywhere, Category = "Combat Intensity", meta = (ClampMin = "0.0"))
	float MoveDecayDelay = 15.0f;

	/** Interp speed for decay from combat (66/100) back to idle (33). Higher = faster. */
	UPROPERTY(EditAnywhere, Category = "Combat Intensity", meta = (ClampMin = "0.01"))
	float DecayToCombatIdleSpeed = 3.0f;

	/** Interp speed for decay from idle (33) back to calm (0). Higher = faster. */
	UPROPERTY(EditAnywhere, Category = "Combat Intensity", meta = (ClampMin = "0.01"))
	float DecayToCalmSpeed = 1.5f;

	/** Minimum change in intensity required to re-send the RTPC to Wwise */
	UPROPERTY(EditAnywhere, Category = "Combat Intensity", meta = (ClampMin = "0.0"))
	float RTPCChangeThreshold = 1.0f;

	/** Interp speed when intensity rises toward a new level (movement/shoot/kill). Higher = snappier. */
	UPROPERTY(EditAnywhere, Category = "Combat Intensity", meta = (ClampMin = "0.01"))
	float AttackInterpSpeed = 5.0f;

	/** Combat_Intensity threshold at or above which MusicState switches to Combat */
	UPROPERTY(EditAnywhere, Category = "Music State", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float CombatStateThreshold = 33.0f;

	/** My team's score at which MusicState switches to EndGame */
	UPROPERTY(EditAnywhere, Category = "Music State", meta = (ClampMin = "1"))
	int32 EndGameScoreLimit = 10;

	// --- Wwise SFX events (set in Blueprint defaults) ---

	/** Posted when the local player takes damage */
	UPROPERTY(EditDefaultsOnly, Category = "Wwise")
	TObjectPtr<UAkAudioEvent> DamageReceivedEvent;

	/** Posted when the local player dies */
	UPROPERTY(EditDefaultsOnly, Category = "Wwise")
	TObjectPtr<UAkAudioEvent> DeathEvent;

	/** Posted when the local player scores a kill */
	UPROPERTY(EditDefaultsOnly, Category = "Wwise")
	TObjectPtr<UAkAudioEvent> KillConfirmedEvent;

	/** Posted when the local player respawns (ResetDeathState) */
	UPROPERTY(EditDefaultsOnly, Category = "Wwise")
	TObjectPtr<UAkAudioEvent> RespawnEvent;

	/** Posted at the target's world location when local player lands a hit (spatial) */
	UPROPERTY(EditDefaultsOnly, Category = "Wwise")
	TObjectPtr<UAkAudioEvent> HitEvent;

private:

	// True once listeners are registered and timers seeded.
	// Registration is deferred to the first Tick where IsLocallyControlled() is true,
	// because in Lyra BeginPlay fires before PossessedBy (Controller is null at BeginPlay).
	bool bListenersRegistered = false;

	// Set once in TryRegisterListeners after confirming local PlayerController.
	// Stays true for the component's lifetime — safe to check even when controller is detached.
	bool bIsLocalPlayer = false;

	// Where intensity is heading. Set instantly by events/movement/decay rules.
	float TargetIntensity = 0.0f;
	// Actual current value sent to Wwise. Follows TargetIntensity via FInterpTo each tick.
	float CombatIntensity = 0.0f;
	float LastSentRTPC = -1.0f;

	// Tracks the last Wwise MusicState sent to avoid redundant SetState calls.
	bool bInCombatState = false;


	double LastDamageDealtTime = -999.0;
	double LastMoveTime = -999.0;
	double MoveStartTime = -1.0;
	bool bWasMovingLastFrame = false;

	FGameplayMessageListenerHandle DamageListenerHandle;
	FGameplayMessageListenerHandle EliminationListenerHandle;
	FGameplayMessageListenerHandle EndGameListenerHandle;
};
