// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.


#pragma once
#include "K2Node.h"
#include "K2Node_EnumEquality.generated.h"

UCLASS(MinimalAPI)
class UK2Node_EnumEquality : public UK2Node
{
	GENERATED_UCLASS_BODY()

	// Begin UEdGraphNode interface
	virtual void AllocateDefaultPins() override;
	virtual FString GetTooltip() const override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FString GetKeywords() const override;
	// End UEdGraphNode interface

	// Begin UK2Node interface
 	virtual void PostReconstructNode() override;
	virtual bool IsNodeSafeToIgnore() const override { return true; }
	virtual bool IsNodePure() const override { return true; }
	virtual bool ShouldDrawCompact() const override { return true; }
	virtual FText GetCompactNodeTitle() const override { return NSLOCTEXT("K2Node", "EqualEqual", "=="); }
	virtual void GetMenuEntries(FGraphContextMenuBuilder& ContextMenuBuilder) const override;
	virtual void NotifyPinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual class FNodeHandlingFunctor* CreateNodeHandler(class FKismetCompilerContext& CompilerContext) const override;
	virtual void ExpandNode(class FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void GetMenuActions(TArray<UBlueprintNodeSpawner*>& ActionListOut) const override;
	virtual FText GetMenuCategory() const override;
	virtual int32 GetNodeRefreshPriority() const override { return EBaseNodeRefreshPriority::Low_UsesDependentWildcard; }
	virtual bool IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const override;
	// End UK2Node interface

	/** Get the return value pin */
	BLUEPRINTGRAPH_API UEdGraphPin* GetReturnValuePin() const;
	/** Get the first input pin */
	BLUEPRINTGRAPH_API UEdGraphPin* GetInput1Pin() const;
	/** Get the second input pin */
	BLUEPRINTGRAPH_API UEdGraphPin* GetInput2Pin() const;

	/** Gets the name and class of the EqualEqual_ByteByte function */
	BLUEPRINTGRAPH_API void GetConditionalFunction(FName& FunctionName, UClass** FunctionClass);
};

