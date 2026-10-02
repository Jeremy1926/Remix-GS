#include "pch.h"
#include "Shared.h"

INIT_MODULE(UAbilitySystemComponent);

static void InternalServerTryActivateAbility(UAbilitySystemComponent* AbilitySystemComponent, FGameplayAbilitySpecHandle Handle, bool InputPressed, FPredictionKey* PredictionKey, void* TriggerEventData)
{
    static auto InternalTryActivateAbility = (bool (*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle, FPredictionKey, UGameplayAbility**, void*, void*))(ImageBase + 0x7AF82CC);

    FGameplayAbilitySpec* Spec = nullptr;

    for (auto& Item : AbilitySystemComponent->ActivatableAbilities.Items)
    {
        if (Item.Handle.Handle == Handle.Handle)
        {
            Spec = &Item;
            break;
        }
    }

    if (!Spec)
        return AbilitySystemComponent->ClientActivateAbilityFailed(Handle, PredictionKey->Current);

    Spec->InputPressed = true;

    UGameplayAbility* InstancedAbility = nullptr;

    if (!InternalTryActivateAbility(AbilitySystemComponent, Handle, *PredictionKey, &InstancedAbility, nullptr, TriggerEventData))
    {
        AbilitySystemComponent->ClientActivateAbilityFailed(Handle, PredictionKey->Current);
        Spec->InputPressed = false;
        AbilitySystemComponent->ActivatableAbilities.MarkItemDirty(*Spec);
    }
}

static void (*K2_AddGameplayCueWithParamsOG)(UObject*, FFrame&, void*);
static void (*K2_AddGameplayCueOG)(UObject*, FFrame&, void*);
static void (*K2_ExecuteGameplayCueOG)(UObject*, FFrame&, void*);
static void (*K2_ExecuteGameplayCueWithParamsOG)(UObject*, FFrame&, void*);

static void K2_AddGameplayCueWithParams(UObject* Context, FFrame& Stack)
{
    FGameplayTag GameplayCueTagTemp{};
    auto& GameplayCueTag = Stack.StepCompiledInRef(&GameplayCueTagTemp);
    FGameplayCueParameters GameplayCueParameterTemp{};
    auto& GameplayCueParameter = Stack.StepCompiledInRef(&GameplayCueParameterTemp);
    bool bRemoveOnAbilityEnd;

    Stack.StepCompiledIn(&bRemoveOnAbilityEnd);
    Stack.IncrementCode();

    auto Ability = (UGameplayAbility*)Context;
    callOG(Ability, Stack.CurrentNativeFunction, K2_AddGameplayCueWithParams, GameplayCueTag, GameplayCueParameter, bRemoveOnAbilityEnd);

    if (auto AbilitySystemComponent = Ability->GetAbilitySystemComponentFromActorInfo())
        AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded_WithParams(GameplayCueTag, FPredictionKey{}, GameplayCueParameter);
}

static void K2_AddGameplayCue(UObject* Context, FFrame& Stack)
{
    FGameplayTag GameplayCueTagTemp{};
    auto& GameplayCueTag = Stack.StepCompiledInRef(&GameplayCueTagTemp);
    FGameplayEffectContextHandle EffectContextTemp{};
    auto& EffectContext = Stack.StepCompiledInRef(&EffectContextTemp);
    bool bRemoveOnAbilityEnd;

    Stack.StepCompiledIn(&bRemoveOnAbilityEnd);
    Stack.IncrementCode();

    auto Ability = (UGameplayAbility*)Context;
    callOG(Ability, Stack.CurrentNativeFunction, K2_AddGameplayCue, GameplayCueTag, EffectContext, bRemoveOnAbilityEnd);

    if (auto AbilitySystemComponent = Ability->GetAbilitySystemComponentFromActorInfo())
        AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(GameplayCueTag, FPredictionKey{}, EffectContext);
}

static void K2_ExecuteGameplayCue(UObject* Context, FFrame& Stack)
{
    FGameplayTag GameplayCueTagTemp{};
    auto& GameplayCueTag = Stack.StepCompiledInRef(&GameplayCueTagTemp);
    FGameplayEffectContextHandle EffectContextTemp{};
    auto& EffectContext = Stack.StepCompiledInRef(&EffectContextTemp);

    Stack.IncrementCode();

    auto Ability = (UGameplayAbility*)Context;
    callOG(Ability, Stack.CurrentNativeFunction, K2_ExecuteGameplayCue, GameplayCueTag, EffectContext);

    if (auto AbilitySystemComponent = Ability->GetAbilitySystemComponentFromActorInfo())
        AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(GameplayCueTag, FPredictionKey{}, EffectContext);
}

static void K2_ExecuteGameplayCueWithParams(UObject* Context, FFrame& Stack)
{
    FGameplayTag GameplayCueTagTemp{};
    auto& GameplayCueTag = Stack.StepCompiledInRef(&GameplayCueTagTemp);
    FGameplayCueParameters GameplayCueParameterTemp{};
    auto& GameplayCueParameter = Stack.StepCompiledInRef(&GameplayCueParameterTemp);

    Stack.IncrementCode();

    auto Ability = (UGameplayAbility*)Context;
    callOG(Ability, Stack.CurrentNativeFunction, K2_ExecuteGameplayCueWithParams, GameplayCueTag, GameplayCueParameter);

    if (auto AbilitySystemComponent = Ability->GetAbilitySystemComponentFromActorInfo())
        AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted_WithParams(GameplayCueTag, FPredictionKey{}, GameplayCueParameter);
}

void UAbilitySystemComponent__Init()
{
    HookEvery<UAbilitySystemComponent>(0x11D, InternalServerTryActivateAbility);

    auto AbilityClass = UGameplayAbility::StaticClass();

    ExecHook(AbilityClass->FindFunction("K2_ExecuteGameplayCue"), K2_ExecuteGameplayCue, K2_ExecuteGameplayCueOG);
    ExecHook(AbilityClass->FindFunction("K2_ExecuteGameplayCueWithParams"), K2_ExecuteGameplayCueWithParams, K2_ExecuteGameplayCueWithParamsOG);
    ExecHook(AbilityClass->FindFunction("K2_AddGameplayCue"), K2_AddGameplayCue, K2_AddGameplayCueOG);
    ExecHook(AbilityClass->FindFunction("K2_AddGameplayCueWithParams"), K2_AddGameplayCueWithParams, K2_AddGameplayCueWithParamsOG);
}
