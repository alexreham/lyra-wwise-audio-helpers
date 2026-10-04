// Written by Alex Reham (alexreham.com) for a Lyra + Wwise integration.
// Shared for educational use under the MIT License: feel free to use and modify.

#include "Audio/LyraCombatIntensityComponent.h"

#include "AkAudioEvent.h"
#include "AkGameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Character/LyraHealthComponent.h"
#include "Messages/LyraVerbMessage.h"
#include "Teams/LyraTeamSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraCombatIntensityComponent)

DEFINE_LOG_CATEGORY_STATIC(LogCombatIntensity, Log, All);

// Returns the PlayerCameraManager for a pawn-owned actor, or nullptr.
// Self-produced SFX (damage, death, respawn) are posted on the camera manager so that
// the Wwise emitter is co-located with the default listener → distance = 0 → no 3D attenuation.
static APlayerCameraManager* GetLocalCameraManager(AActor* OwnerActor)
{
	const APawn* Pawn = Cast<APawn>(OwnerActor);
	if (!Pawn) return nullptr;
	const APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	return PC ? PC->PlayerCameraManager : nullptr;
}

namespace LyraCombatIntensity
{
	static const FName RTPCName(TEXT("Combat_Intensity"));

	static const float IntensityIdle   = 0.0f;
	static const float IntensityMoving = 33.0f;
	static const float IntensityFight  = 66.0f;
	static const float IntensityKill   = 100.0f;

	// Wwise State Group and State names — must match the Wwise project exactly.
	static const FName StateGroup(TEXT("MusicState"));
	static const FName StateCombat1(TEXT("Combat1"));
	static const FName StateCombat2(TEXT("Combat2"));
	static const FName StateIdle(TEXT("Idle"));
	static const FName StateEndGame(TEXT("EndGame"));

	// Persist across pawn respawns (Lyra destroys/recreates pawn on death).
	// Safe as static because only one local player's component is ever active.
	static bool bInCombat1 = true;
	static int32 MyTeamKillCount = 0;
	// Set to true after the first death so TryRegisterListeners can detect a respawn.
	static bool bHasDied = false;
	// Set to true after the first successful TryRegisterListeners in a session,
	// so that respawn does not wipe MyTeamKillCount and bEndGameTriggered mid-match.
	static bool bSessionInitialized = false;
	// Set to true once the match-end kill threshold has been reached.
	// Persists across pawn respawns so ResetDeathState, UpdateMusicState, and
	// OnLocalPlayerDeath can all stay bailed out after EndGame has been latched.
	static bool bEndGameTriggered = false;
}

ULyraCombatIntensityComponent::ULyraCombatIntensityComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void ULyraCombatIntensityComponent::BeginPlay()
{
	Super::BeginPlay();

	// Do NOT check IsLocallyControlled() here.
	// In Lyra, BeginPlay fires before PossessedBy — Controller is null at this point,
	// so IsLocallyControlled() always returns false. Registration is deferred to Tick.

	// Register elimination listener early on ALL instances (including bots).
	// The handler gates on bIsLocalPlayer before acting.
	// This ensures EndGame detection works regardless of TryRegisterListeners timing.
	if (UGameplayMessageSubsystem::HasInstance(this))
	{
		UGameplayMessageSubsystem& MessageSystem = UGameplayMessageSubsystem::Get(this);
		EndGameListenerHandle = MessageSystem.RegisterListener<FLyraVerbMessage>(
			FGameplayTag::RequestGameplayTag(TEXT("Lyra.Elimination.Message")),
			this,
			&ThisClass::OnEliminationForEndGame);
	}
}

void ULyraCombatIntensityComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UGameplayMessageSubsystem::HasInstance(this))
	{
		UGameplayMessageSubsystem& MessageSystem = UGameplayMessageSubsystem::Get(this);
		MessageSystem.UnregisterListener(EndGameListenerHandle);

		if (bListenersRegistered)
		{
			MessageSystem.UnregisterListener(DamageListenerHandle);
			MessageSystem.UnregisterListener(EliminationListenerHandle);
		}
	}

	// Reset all session-persistent statics when the game session actually ends.
	// This covers stopping PIE (EndPlayInEditor) and traveling to a new level (LevelTransition).
	// Pawn destruction on death uses Destroyed, which is intentionally excluded so that
	// respawn inherits the accumulated kill count and end-game state.
	if (EndPlayReason == EEndPlayReason::EndPlayInEditor ||
		EndPlayReason == EEndPlayReason::LevelTransition)
	{
		LyraCombatIntensity::bSessionInitialized = false;
		LyraCombatIntensity::MyTeamKillCount     = 0;
		LyraCombatIntensity::bInCombat1          = true;
		LyraCombatIntensity::bHasDied            = false;
		LyraCombatIntensity::bEndGameTriggered   = false;
	}

	Super::EndPlay(EndPlayReason);
}

void ULyraCombatIntensityComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Lazy initialization: wait until the pawn has a controller and IsLocallyControlled() is true.
	if (!bListenersRegistered)
	{
		if (!TryRegisterListeners())
		{
			return;
		}
	}

	const double CurrentTime = GetWorld()->GetTimeSeconds();

	TickMovement(DeltaTime, CurrentTime);
	TickDecay(CurrentTime);

	// Move CombatIntensity toward TargetIntensity. Speed depends on direction:
	//   rising   → AttackInterpSpeed  (fast, responsive)
	//   falling from combat level → DecayToCombatIdleSpeed  (slow)
	//   falling from moving level → DecayToCalmSpeed        (slowest)
	float InterpSpeed;
	if (TargetIntensity > CombatIntensity)
	{
		InterpSpeed = AttackInterpSpeed;
	}
	else if (CombatIntensity > LyraCombatIntensity::IntensityMoving)
	{
		InterpSpeed = DecayToCombatIdleSpeed;
	}
	else
	{
		InterpSpeed = DecayToCalmSpeed;
	}
	CombatIntensity = FMath::FInterpTo(CombatIntensity, TargetIntensity, DeltaTime, InterpSpeed);

	// Snap to exact target values when close enough to avoid perpetual RTPC noise.
	if (FMath::IsNearlyEqual(CombatIntensity, TargetIntensity, 0.5f))
	{
		CombatIntensity = TargetIntensity;
	}

	SendRTPCIfChanged();
}

// ---------------------------------------------------------------------------
// Lazy registration
// ---------------------------------------------------------------------------

bool ULyraCombatIntensityComponent::TryRegisterListeners()
{
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!OwnerPawn)
	{
		SetComponentTickEnabled(false);
		return false;
	}

	// Must be controlled by a real human player's controller with a ULocalPlayer.
	// Lyra's ALyraPlayerBotController extends APlayerController, so a plain
	// Cast<APlayerController> is not enough to filter bots. Bot controllers
	// never have a ULocalPlayer assigned — only the real human player does.
	APlayerController* PC = Cast<APlayerController>(OwnerPawn->GetController());
	if (!PC)
	{
		// Controller not yet assigned — retry next tick.
		// If a non-PlayerController is assigned, this is a standard AI bot.
		if (OwnerPawn->GetController() != nullptr)
		{
			SetComponentTickEnabled(false);
		}
		return false;
	}

	if (!PC->GetLocalPlayer())
	{
		// PlayerController without a LocalPlayer = LyraPlayerBotController or remote PC.
		SetComponentTickEnabled(false);
		return false;
	}

	// All checks passed — this is confirmed to be the local human player.
	bIsLocalPlayer = true;

	// Reset kill count and end-game flag exactly once per session.
	// The guard prevents a respawn (new pawn, same session) from wiping the score mid-match.
	if (!LyraCombatIntensity::bSessionInitialized)
	{
		LyraCombatIntensity::MyTeamKillCount   = 0;
		LyraCombatIntensity::bEndGameTriggered = false;
		LyraCombatIntensity::bSessionInitialized = true;
	}

	// Seed timers so decay does not fire immediately on the first frame.
	const double Now = GetWorld()->GetTimeSeconds();
	LastDamageDealtTime = Now;
	LastMoveTime = Now;

	UGameplayMessageSubsystem& MessageSystem = UGameplayMessageSubsystem::Get(this);

	// "Lyra.Damage.Message" is defined via UE_DEFINE_GAMEPLAY_TAG in LyraHealthSet.cpp.
	// Message.Instigator = GetEffectCauser() = the Character/Pawn that owns the ASC.
	DamageListenerHandle = MessageSystem.RegisterListener<FLyraVerbMessage>(
		FGameplayTag::RequestGameplayTag(TEXT("Lyra.Damage.Message")),
		this,
		&ThisClass::OnDamageMessage);

	// "Lyra.Elimination.Message" is defined in LyraHealthComponent.cpp.
	// Broadcast inside WITH_SERVER_CODE — works in Standalone and Listen Server.
	EliminationListenerHandle = MessageSystem.RegisterListener<FLyraVerbMessage>(
		FGameplayTag::RequestGameplayTag(TEXT("Lyra.Elimination.Message")),
		this,
		&ThisClass::OnEliminationMessage);

	// Listen for local player death to reset intensity and switch MusicState.
	if (ULyraHealthComponent* HealthComp = ULyraHealthComponent::FindHealthComponent(GetOwner()))
	{
		HealthComp->OnDeathStarted.AddDynamic(this, &ThisClass::OnLocalPlayerDeath);
	}

	bListenersRegistered = true;

	// Push initial value to Wwise immediately.
	LastSentRTPC = -1.0f;
	SendRTPCIfChanged();

	// If this is a respawn (not the first spawn), fire the respawn SFX now.
	// Lyra destroys the pawn on death and creates a new one — so this component
	// is brand-new each respawn. bHasDied persists across pawn recreations.
	if (LyraCombatIntensity::bHasDied)
	{
		ResetDeathState();
	}

	return true;
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

void ULyraCombatIntensityComponent::OnDamageMessage(FGameplayTag Channel, const FLyraVerbMessage& Message)
{
	// LyraHealthSet sets Message.Instigator = GetEffectCauser(), which for weapon
	// abilities is the Character/Pawn (ASC avatar actor). Compare against our owner.
	AActor* const Owner = GetOwner();

	// Primary check: direct match (character fires weapon).
	// Fallback: check if the causer is owned by our character (e.g. a projectile actor).
	AActor* const InstigatorActor = Cast<AActor>(Message.Instigator);
	const bool bWeDealtDamage = (InstigatorActor == Owner)
		|| (InstigatorActor && InstigatorActor->GetOwner() == Owner);

	// Check if local player received damage.
	// Message.Target = UAttributeSet::GetOwningActor() = ASC OwnerActor = PlayerState in Lyra,
	// NOT the pawn. Compare against our pawn's PlayerState instead.
	const APawn* OwnerPawn = Cast<APawn>(Owner);
	const bool bWeTookDamage = OwnerPawn && (Message.Target == OwnerPawn->GetPlayerState());

	if (bWeTookDamage && DamageReceivedEvent)
	{
		APlayerCameraManager* CamMgr = GetLocalCameraManager(Owner);
		AActor* PostTarget = CamMgr ? static_cast<AActor*>(CamMgr) : Owner;
		UAkGameplayStatics::PostEvent(DamageReceivedEvent, PostTarget, 0, FOnAkPostEventCallback(), false);
	}

	if (!bWeDealtDamage)
	{
		return;
	}

	LastDamageDealtTime = GetWorld()->GetTimeSeconds();

	// Damage must not downgrade a higher target (e.g. IntensityKill=100 set by an elimination).
	// Only raise the target to IntensityFight — let natural decay bring it down from 100.
	if (TargetIntensity < LyraCombatIntensity::IntensityFight)
	{
		ApplyCombatIntensity(LyraCombatIntensity::IntensityFight);
	}

	// Wwise hit SFX — post at the target's world location for spatial audio.
	// Message.Target is the PlayerState; get the target pawn's location from it.
	if (HitEvent)
	{
		FVector HitLocation = FVector::ZeroVector;
		bool bGotLocation = false;

		if (const APlayerState* TargetPS = Cast<APlayerState>(Message.Target))
		{
			if (const APawn* TargetPawn = TargetPS->GetPawn())
			{
				HitLocation = TargetPawn->GetActorLocation();
				bGotLocation = true;
			}
		}

		if (!bGotLocation)
		{
			// Fallback: post on our own actor (non-spatial but still fires).
			UAkGameplayStatics::PostEvent(HitEvent, Owner, 0, FOnAkPostEventCallback(), false);
		}
		else
		{
			UAkGameplayStatics::PostEventAtLocation(HitEvent, HitLocation, FRotator::ZeroRotator, this);
		}
	}
}

void ULyraCombatIntensityComponent::OnEliminationMessage(FGameplayTag Channel, const FLyraVerbMessage& Message)
{
	AActor* const Owner = GetOwner();

	// Message.Instigator is set in LyraHealthComponent::HandleOutOfHealth from the GAS
	// effect context's instigator — in Lyra that's the ASC OwnerActor = ALyraPlayerState,
	// NOT the killing pawn. Resolve the pawn from the PlayerState before comparing.
	APlayerState* const InstigatorPS   = Cast<APlayerState>(Message.Instigator);
	APawn*         const InstigatorPawn = InstigatorPS ? InstigatorPS->GetPawn() : nullptr;
	const bool bWeGotTheKill = (InstigatorPawn == Owner);

	if (!bWeGotTheKill)
	{
		return;
	}

	const double Now = GetWorld()->GetTimeSeconds();
	LastDamageDealtTime = Now;

	ApplyCombatIntensity(LyraCombatIntensity::IntensityKill);

	// Wwise kill confirmed SFX
	if (KillConfirmedEvent)
	{
		UAkGameplayStatics::PostEvent(KillConfirmedEvent, Owner, 0, FOnAkPostEventCallback(), false);
	}
}

// ---------------------------------------------------------------------------
// Tick helpers
// ---------------------------------------------------------------------------

void ULyraCombatIntensityComponent::TickMovement(float DeltaTime, double CurrentTime)
{
	float Speed2D = 0.0f;

	if (const ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		if (const UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement())
		{
			Speed2D = MoveComp->Velocity.Size2D();
		}
	}
	else if (const AActor* Owner = GetOwner())
	{
		Speed2D = Owner->GetVelocity().Size2D();
	}

	const bool bIsMovingNow = (Speed2D > MoveVelocityThreshold);

	if (bIsMovingNow)
	{
		LastMoveTime = CurrentTime;

		if (!bWasMovingLastFrame)
		{
			MoveStartTime = CurrentTime;
		}

		const float MoveDuration = static_cast<float>(CurrentTime - MoveStartTime);
		// Only raise the target to IntensityMoving — never downgrade a higher target
		// set by damage (66) or kills (100). Guard on TargetIntensity, not CombatIntensity:
		// mid-climb from 0→66 the smoothed CombatIntensity may still be below 33, which
		// would otherwise let movement clobber Target=66 down to 33.
		if (MoveDuration >= MoveTimeToActivate && TargetIntensity < LyraCombatIntensity::IntensityMoving)
		{
			ApplyCombatIntensity(LyraCombatIntensity::IntensityMoving);
		}
	}

	bWasMovingLastFrame = bIsMovingNow;
}

void ULyraCombatIntensityComponent::TickDecay(double CurrentTime)
{
	const float ShootIdleSeconds = static_cast<float>(CurrentTime - LastDamageDealtTime);
	const float MoveIdleSeconds  = static_cast<float>(CurrentTime - LastMoveTime);

	// Phase 1: no damage for ShootDecayDelay sec → target drops to 33.
	// Only lowers the target; a concurrent kill/shoot event will have raised it higher.
	if (ShootIdleSeconds >= ShootDecayDelay)
	{
		TargetIntensity = FMath::Min(TargetIntensity, LyraCombatIntensity::IntensityMoving);
	}

	// Phase 2: no movement for MoveDecayDelay sec → target drops to 0.
	if (MoveIdleSeconds >= MoveDecayDelay)
	{
		TargetIntensity = LyraCombatIntensity::IntensityIdle;
	}
}

// ---------------------------------------------------------------------------
// RTPC
// ---------------------------------------------------------------------------

void ULyraCombatIntensityComponent::ApplyCombatIntensity(float NewValue)
{
	// Only set the target. CombatIntensity follows via FInterpTo in TickComponent,
	// so transitions are smooth instead of instant jumps. Wwise receives a
	// continuously changing value each tick rather than a single step.
	TargetIntensity = FMath::Clamp(NewValue, LyraCombatIntensity::IntensityIdle, LyraCombatIntensity::IntensityKill);
}

void ULyraCombatIntensityComponent::SendRTPCIfChanged()
{
	if (FMath::Abs(CombatIntensity - LastSentRTPC) < RTPCChangeThreshold)
	{
		return;
	}

	// Interpolation = 0: C++ FInterpTo already produces a smoothly changing value
	// each tick, so Wwise does not need to add its own linear interpolation on top.
	UAkGameplayStatics::SetRTPCValue(
		nullptr,
		CombatIntensity,
		0,
		nullptr,
		LyraCombatIntensity::RTPCName);

	LastSentRTPC = CombatIntensity;

	UpdateMusicState();
}

void ULyraCombatIntensityComponent::UpdateMusicState()
{
	// EndGame is a terminal state — once set, nothing else should overwrite it.
	// Tick is disabled when bEndGameTriggered flips to true, but ResetDeathState()
	// (on respawn) still calls UpdateMusicState directly, which would otherwise
	// revert the MusicState back to Idle/Combat.
	if (LyraCombatIntensity::bEndGameTriggered)
	{
		return;
	}

	const bool bShouldBeCombat = (CombatIntensity >= CombatStateThreshold);

	if (bShouldBeCombat == bInCombatState)
	{
		return;
	}

	// Guard: only the local human player's component should switch global Wwise States.
	if (!bIsLocalPlayer)
	{
		return;
	}

	bInCombatState = bShouldBeCombat;

	// Use the alternating Combat1/Combat2 state based on the static bInCombat1 flag.
	// Each death flips the flag, so entering combat after respawn uses the other state.
	const FName& TargetState = bShouldBeCombat
		? (LyraCombatIntensity::bInCombat1 ? LyraCombatIntensity::StateCombat1 : LyraCombatIntensity::StateCombat2)
		: LyraCombatIntensity::StateIdle;

	UAkGameplayStatics::SetState(nullptr, LyraCombatIntensity::StateGroup, TargetState);
}

void ULyraCombatIntensityComponent::ResetDeathState()
{
	// Wwise respawn SFX
	if (RespawnEvent && bIsLocalPlayer)
	{
		APlayerCameraManager* CamMgr = GetLocalCameraManager(GetOwner());
		AActor* PostTarget = CamMgr ? static_cast<AActor*>(CamMgr) : GetOwner();
		UAkGameplayStatics::PostEvent(RespawnEvent, PostTarget, 0, FOnAkPostEventCallback(), false);
	}

	UpdateMusicState();
}

// ---------------------------------------------------------------------------
// Death handler
// ---------------------------------------------------------------------------

void ULyraCombatIntensityComponent::OnLocalPlayerDeath(AActor* OwningActor)
{
	// Only the local human player's component should touch global Wwise state.
	// bIsLocalPlayer was set once in TryRegisterListeners after the PlayerController check,
	// so it remains reliable even when the controller is detached during death.
	if (!bIsLocalPlayer)
	{
		return;
	}

	// EndGame is terminal — do not switch the MusicState to Combat1/Combat2 on death
	// once the match has ended. Still play the death SFX? No — let the caller decide.
	// Simpler and consistent with UpdateMusicState(): bail out entirely.
	if (LyraCombatIntensity::bEndGameTriggered)
	{
		return;
	}

	LyraCombatIntensity::bHasDied = true;

	TargetIntensity = 0.0f;
	CombatIntensity = 0.0f;
	LastSentRTPC = -1.0f;

	UAkGameplayStatics::SetRTPCValue(
		nullptr,
		CombatIntensity,
		0,
		nullptr,
		LyraCombatIntensity::RTPCName);

	// Flip the combat slot so the next combat phase uses the alternate state.
	const FName& DeathState = LyraCombatIntensity::bInCombat1
		? LyraCombatIntensity::StateCombat2
		: LyraCombatIntensity::StateCombat1;
	LyraCombatIntensity::bInCombat1 = !LyraCombatIntensity::bInCombat1;

	UAkGameplayStatics::SetState(nullptr, LyraCombatIntensity::StateGroup, DeathState);
	bInCombatState = false;

	// Wwise death SFX — post on camera manager (emitter at listener position, no 3D attenuation).
	if (DeathEvent)
	{
		APlayerCameraManager* CamMgr = GetLocalCameraManager(GetOwner());
		AActor* PostTarget = CamMgr ? static_cast<AActor*>(CamMgr) : GetOwner();
		UAkGameplayStatics::PostEvent(DeathEvent, PostTarget, 0, FOnAkPostEventCallback(), false);
	}
}

// ---------------------------------------------------------------------------
// EndGame score check (fires on every elimination in the match)
// ---------------------------------------------------------------------------

void ULyraCombatIntensityComponent::OnEliminationForEndGame(FGameplayTag Channel, const FLyraVerbMessage& Message)
{
	if (!bIsLocalPlayer || LyraCombatIntensity::bEndGameTriggered)
	{
		return;
	}

	const ULyraTeamSubsystem* TeamSubsystem = GetWorld()->GetSubsystem<ULyraTeamSubsystem>();
	if (!TeamSubsystem)
	{
		return;
	}

	// Get the killer's team from the message instigator.
	// Message.Instigator is an ALyraPlayerState (see OnEliminationMessage for rationale).
	// FindTeamFromObject handles this correctly: ALyraPlayerState implements
	// ILyraTeamAgentInterface, so the team ID is returned directly without needing
	// to resolve to a pawn first.
	const int32 KillerTeamId = TeamSubsystem->FindTeamFromObject(Message.Instigator);

	// Get our team.
	const int32 MyTeamId = TeamSubsystem->FindTeamFromObject(GetOwner());

	if (MyTeamId == INDEX_NONE || KillerTeamId == INDEX_NONE)
	{
		return;
	}

	// Only count kills scored by our team.
	if (KillerTeamId != MyTeamId)
	{
		return;
	}

	++LyraCombatIntensity::MyTeamKillCount;

	if (LyraCombatIntensity::MyTeamKillCount >= EndGameScoreLimit)
	{
		LyraCombatIntensity::bEndGameTriggered = true;

		UAkGameplayStatics::SetState(nullptr, LyraCombatIntensity::StateGroup, LyraCombatIntensity::StateEndGame);
		SetComponentTickEnabled(false);
	}
}
