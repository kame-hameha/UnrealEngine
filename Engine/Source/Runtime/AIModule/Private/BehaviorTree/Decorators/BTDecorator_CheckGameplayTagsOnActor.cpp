// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "AIModulePrivate.h"
#include "GameplayTagAssetInterface.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Decorators/BTDecorator_CheckGameplayTagsOnActor.h"

UBTDecorator_CheckGameplayTagsOnActor::UBTDecorator_CheckGameplayTagsOnActor(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	NodeName = "Gameplay Tag Condition";

	// Accept only actors
	ActorToCheck.AddObjectFilter(this, AActor::StaticClass());

	// Default to using Self Actor
	ActorToCheck.SelectedKeyName = FBlackboard::KeySelf;

	// For now, don't allow users to select any "Abort Observers", because it's currently not supported.
	bAllowAbortNone = false;
	bAllowAbortLowerPri = false;
	bAllowAbortChildNodes = false;
}

bool UBTDecorator_CheckGameplayTagsOnActor::CalculateRawConditionValue(UBehaviorTreeComponent* OwnerComp, uint8* NodeMemory) const
{
	check(OwnerComp != NULL);
	const UBlackboardComponent* BlackboardComp = OwnerComp->GetBlackboardComponent();
	if (BlackboardComp == NULL)
	{
		return false;
	}

	IGameplayTagAssetInterface* GameplayTagAssetInterface = Cast<IGameplayTagAssetInterface>(BlackboardComp->GetValueAsObject(ActorToCheck.GetSelectedKeyID()));
	if (GameplayTagAssetInterface == NULL)
	{
		return false;
	}
	
	switch (TagsToMatch)
	{
		case EGameplayContainerMatchType::All:
			return GameplayTagAssetInterface->HasAllMatchingGameplayTags(GameplayTags);

		case EGameplayContainerMatchType::Any:
			return GameplayTagAssetInterface->HasAnyMatchingGameplayTags(GameplayTags);

		default:
		{
			UE_LOG(LogBehaviorTree, Warning, TEXT("Invalid value for TagsToMatch (EGameplayContainerMatchType) %d.  Should only be Any or All."), static_cast<int32>(TagsToMatch));
			return false;
		}
	}
}

void UBTDecorator_CheckGameplayTagsOnActor::DescribeRuntimeValues(const UBehaviorTreeComponent* OwnerComp, uint8* NodeMemory, EBTDescriptionVerbosity::Type Verbosity, TArray<FString>& Values) const
{
	Super::DescribeRuntimeValues(OwnerComp, NodeMemory, Verbosity, Values);
}

FString UBTDecorator_CheckGameplayTagsOnActor::GetStaticDescription() const
{
	return FString::Printf(TEXT("%s: %s"), *Super::GetStaticDescription(), *CachedDescription);
}

#if WITH_EDITOR

void UBTDecorator_CheckGameplayTagsOnActor::BuildDescription()
{
	CachedDescription = GameplayTags.ToMatchingText(TagsToMatch, IsInversed()).ToString();
}

void UBTDecorator_CheckGameplayTagsOnActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.Property == NULL)
	{
		return;
	}

	BuildDescription();
}

void UBTDecorator_CheckGameplayTagsOnActor::InitializeFromAsset(UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);

	UBlackboardData* BBAsset = GetBlackboardAsset();
	ActorToCheck.CacheSelectedKey(BBAsset);

	BuildDescription();
}

#endif	// WITH_EDITOR
