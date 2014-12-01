// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.


#include "BlueprintGraphPrivatePCH.h"
#include "KismetCompiler.h"
#include "VariableSetHandler.h"

#define LOCTEXT_NAMESPACE "K2Node_VariableSet"

namespace K2Node_VariableSetImpl
{
	/**
	 * Shared utility method for retrieving a UK2Node_VariableSet's bare tooltip.
	 * 
	 * @param  VarName	The name of the variable that the node represents.
	 * @return A formatted text string, describing what the VariableSet node does.
	 */
	static FText GetBaseTooltip(FName VarName);

	/**
	 * Returns true if the specified variable RepNotify AND is defined in a 
	 * blueprint. Most (all?) native rep notifies are intended to be client 
	 * only. We are moving away from this paradigm in blueprints. So for now 
	 * this is somewhat of a hold over to avoid nasty bugs where a K2 set node 
	 * is calling a native function that the designer has no idea what it is 
	 * doing.
	 * 
	 * @param  VariableProperty	The variable property you wish to check.
	 * @return True if the specified variable RepNotify AND is defined in a blueprint.
	 */
	static bool PropertyHasLocalRepNotify(UProperty const* VariableProperty);
}

static FText K2Node_VariableSetImpl::GetBaseTooltip(FName VarName)
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("VarName"), FText::FromName(VarName));

	return FText::Format(LOCTEXT("SetVariableTooltip", "Set the value of variable {VarName}"), Args);

}

static bool K2Node_VariableSetImpl::PropertyHasLocalRepNotify(UProperty const* VariableProperty)
{
	if (VariableProperty != nullptr)
	{
		// We check that the variable is 'defined in a blueprint' so as to avoid 
		// natively defined RepNotifies being called unintentionally. Most(all?) 
		// native rep notifies are intended to be client only. We are moving 
		// away from this paradigm in blueprints. So for now this is somewhat of 
		// a hold over to avoid nasty bugs where a K2 set node is calling a 
		// native function that the designer has no idea what it is doing.
		UBlueprintGeneratedClass* VariableSourceClass = Cast<UBlueprintGeneratedClass>(VariableProperty->GetOwnerClass());
		bool const bIsBlueprintProperty = (VariableSourceClass != nullptr);

		if (bIsBlueprintProperty && (VariableProperty->RepNotifyFunc != NAME_None))
		{
			// Find function (ok if its defined in native class)
			UFunction* Function = VariableSourceClass->FindFunctionByName(VariableProperty->RepNotifyFunc);

			// If valid repnotify func
			if ((Function != nullptr) && (Function->NumParms == 0) && (Function->GetReturnProperty() == nullptr))
			{
				return true;
			}
		}
	}
	return false;
}

UK2Node_VariableSet::UK2Node_VariableSet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UK2Node_VariableSet::AllocateDefaultPins()
{
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	CreatePin(EGPD_Input, K2Schema->PC_Exec, TEXT(""), NULL, false, false, K2Schema->PN_Execute);
	CreatePin(EGPD_Output, K2Schema->PC_Exec, TEXT(""), NULL, false, false, K2Schema->PN_Then);

	if (GetVarName() != NAME_None)
	{
		if(CreatePinForVariable(EGPD_Input))
		{
			CreatePinForSelf();
		}

		if(CreatePinForVariable(EGPD_Output, GetVariableOutputPinName()))
		{
			CreateOutputPinTooltip();
		}
	}

	Super::AllocateDefaultPins();
}

void UK2Node_VariableSet::ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins)
{
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	CreatePin(EGPD_Input, K2Schema->PC_Exec, TEXT(""), NULL, false, false, K2Schema->PN_Execute);
	CreatePin(EGPD_Output, K2Schema->PC_Exec, TEXT(""), NULL, false, false, K2Schema->PN_Then);

	if (GetVarName() != NAME_None)
	{
		if(!CreatePinForVariable(EGPD_Input))
		{
			if(!RecreatePinForVariable(EGPD_Input, OldPins))
			{
				return;
			}
		}

		if(!CreatePinForVariable(EGPD_Output, GetVariableOutputPinName()))
		{
			if(!RecreatePinForVariable(EGPD_Output, OldPins, GetVariableOutputPinName()))
			{
				return;
			}
		}
		CreateOutputPinTooltip();
		CreatePinForSelf();
	}
}



FText UK2Node_VariableSet::GetPropertyTooltip(UProperty const* VariableProperty)
{
	FText TextFormat;
	FFormatNamedArguments Args;

	bool const bHasLocalRepNotify = K2Node_VariableSetImpl::PropertyHasLocalRepNotify(VariableProperty);
	if (bHasLocalRepNotify)
	{
		Args.Add(TEXT("ReplicationNotifyName"), FText::FromName(VariableProperty->RepNotifyFunc));
		TextFormat = LOCTEXT("SetVariableWithRepNotify_Tooltip", "Set the value of variable {VarName} and call {ReplicationNotifyName}");
	}

	FName VarName = NAME_None;
	if (VariableProperty != nullptr)
	{
		VarName = VariableProperty->GetFName();

		UClass* SourceClass = VariableProperty->GetOwnerClass();
		// discover if the variable property is a non blueprint user variable
		bool const bIsNativeVariable = (SourceClass != nullptr) && (SourceClass->ClassGeneratedBy == nullptr);
		FName const TooltipMetaKey(TEXT("tooltip"));

		FText SubTooltip;
		if (bIsNativeVariable)
		{
			FText const PropertyTooltip = VariableProperty->GetToolTipText();
			if (!PropertyTooltip.IsEmpty())
			{
				// See if the native property has a tooltip
				SubTooltip = PropertyTooltip;
				FString TooltipName = FString::Printf(TEXT("%s.%s"), *VarName.ToString(), *TooltipMetaKey.ToString());
				FText::FindText(*VariableProperty->GetFullGroupName(true), *TooltipName, SubTooltip);
			}
		}
		else if (UBlueprint* VarBlueprint = Cast<UBlueprint>(SourceClass->ClassGeneratedBy))
		{
			FString UserTooltipData;
			if (FBlueprintEditorUtils::GetBlueprintVariableMetaData(VarBlueprint, VarName, /*InLocalVarScope =*/nullptr, TooltipMetaKey, UserTooltipData))
			{
				SubTooltip = FText::FromString(UserTooltipData);
			}
		}

		if (!SubTooltip.IsEmpty())
		{
			Args.Add(TEXT("PropertyTooltip"), SubTooltip);
			if (bHasLocalRepNotify)
			{
				TextFormat = LOCTEXT("SetVariablePropertyWithRepNotify_Tooltip", "Set the value of variable {VarName} and call {ReplicationNotifyName}\n{PropertyTooltip}");
			}
			else
			{
				TextFormat = LOCTEXT("SetVariableProperty_Tooltip", "Set the value of variable {VarName}\n{PropertyTooltip}");
			}
		}
	}

	if (TextFormat.IsEmpty())
	{
		return K2Node_VariableSetImpl::GetBaseTooltip(VarName);
	}
	else
	{
		Args.Add(TEXT("VarName"), FText::FromName(VarName));
		return FText::Format(TextFormat, Args);
	}
}

FText UK2Node_VariableSet::GetBlueprintVarTooltip(FBPVariableDescription const& VarDesc)
{
	FName const TooltipMetaKey(TEXT("tooltip"));
	int32 const MetaIndex = VarDesc.FindMetaDataEntryIndexForKey(TooltipMetaKey);
	bool const bHasTooltipData = (MetaIndex != INDEX_NONE);

	if (bHasTooltipData)
	{
		FString UserTooltipData = VarDesc.GetMetaData(TooltipMetaKey);

		FFormatNamedArguments Args;
		Args.Add(TEXT("VarName"), FText::FromName(VarDesc.VarName));
		Args.Add(TEXT("UserTooltip"), FText::FromString(UserTooltipData));

		return FText::Format(LOCTEXT("SetVariableProperty_Tooltip", "Set the value of variable {VarName}\n{UserTooltip}"), Args);
	}
	return K2Node_VariableSetImpl::GetBaseTooltip(VarDesc.VarName);
}

FText UK2Node_VariableSet::GetTooltipText() const
{
	// @TODO: The variable name mutates as the user makes changes to the 
	//        underlying property, so until we can catch all those cases, we're
	//        going to leave this optimization off
	//if (CachedTooltip.IsOutOfDate())
	{
		if (UProperty* Property = GetPropertyForVariable())
		{
			CachedTooltip = GetPropertyTooltip(Property);
		}
		else if (FBPVariableDescription const* VarDesc = GetBlueprintVarDescription())
		{
			CachedTooltip = GetBlueprintVarTooltip(*VarDesc);
		}
		else
		{
			CachedTooltip = K2Node_VariableSetImpl::GetBaseTooltip(GetVarName());
		}
	}
	return CachedTooltip;

	

	FFormatNamedArguments Args;
	Args.Add( TEXT( "VarName" ), FText::FromString( GetVarNameString() ));
	Args.Add( TEXT( "ReplicationCall" ), FText::GetEmpty());
	Args.Add( TEXT( "ReplicationNotifyName" ), FText::GetEmpty());
	Args.Add( TEXT( "TextPartition" ), FText::GetEmpty());
	Args.Add( TEXT( "MetaData" ), FText::GetEmpty());
		
	if(	HasLocalRepNotify() )
	{
		Args.Add( TEXT( "ReplicationCall" ), NSLOCTEXT( "K2Node", "VariableSet_ReplicationCall", " and call " ));
		Args.Add( TEXT( "ReplicationNotifyName" ), FText::FromString( GetRepNotifyName().ToString() ));
	}
	if(  UProperty* Property = GetPropertyForVariable() )
	{
		// discover if the variable property is a non blueprint user variable
		UClass* SourceClass = Property->GetOwnerClass();
		if( SourceClass && SourceClass->ClassGeneratedBy == NULL )
		{
			const FString MetaData = Property->GetToolTipText().ToString();

			if( !MetaData.IsEmpty() )
			{
				// See if the property associated with this editor has a tooltip
				FText PropertyMetaData = FText::FromString( *MetaData );
				FString TooltipName = FString::Printf( TEXT("%s.tooltip"), *(Property->GetName()));
				FText::FindText( *(Property->GetFullGroupName(true)), *TooltipName, PropertyMetaData );
				Args.Add( TEXT( "TextPartition" ), FText::FromString( "\n" ));
				Args.Add( TEXT( "MetaData" ), PropertyMetaData );
			}
		}
	}
	// FText::Format() is slow, so we cache this to save on performance
	CachedTooltip = FText::Format(NSLOCTEXT("K2Node", "SetValueOfVariable", "Set the value of variable {VarName}{ReplicationCall}{ReplicationNotifyName}{TextPartition}{MetaData}"), Args);
	return CachedTooltip;
}

FText UK2Node_VariableSet::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	// If there is only one variable being written (one non-meta input pin), the title can be made the variable name
	FString InputPinName;
	int32 NumInputsFound = 0;

	for (int32 PinIndex = 0; PinIndex < Pins.Num(); ++PinIndex)
	{
		UEdGraphPin* Pin = Pins[PinIndex];
		if ((Pin->Direction == EGPD_Input) && (!K2Schema->IsMetaPin(*Pin)))
		{
			++NumInputsFound;
			InputPinName = Pin->PinName;
		}
	}

	if (NumInputsFound != 1)
	{
		return HasLocalRepNotify() ? NSLOCTEXT("K2Node", "SetWithNotify", "Set with Notify") : NSLOCTEXT("K2Node", "Set", "Set");
	}
	// @TODO: The variable name mutates as the user makes changes to the 
	//        underlying property, so until we can catch all those cases, we're
	//        going to leave this optimization off
	else //if (CachedNodeTitle.IsOutOfDate())
	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("PinName"), FText::FromString(InputPinName));

		// FText::Format() is slow, so we cache this to save on performance
		if (HasLocalRepNotify())
		{
			CachedNodeTitle = FText::Format(NSLOCTEXT("K2Node", "SetWithNotifyPinName", "Set with Notify {PinName}"), Args);
		}
		else
		{
			CachedNodeTitle = FText::Format(NSLOCTEXT("K2Node", "SetPinName", "Set {PinName}"), Args);
		}
	}
	return CachedNodeTitle;
}

/** Returns true if the variable we are setting has a RepNotify AND was defined in a blueprint
 *		The 'defined in a blueprint' is to avoid natively defined RepNotifies being called unintentionally.
 *		Most (all?) native rep notifies are intended to be client only. We are moving away from this paradigm in blueprints
 *		So for now this is somewhat of a hold over to avoid nasty bugs where a K2 set node is calling a native function that the
 *		designer has no idea what it is doing.
 */
bool UK2Node_VariableSet::HasLocalRepNotify() const
{
	return K2Node_VariableSetImpl::PropertyHasLocalRepNotify(GetPropertyForVariable());
}

bool UK2Node_VariableSet::ShouldFlushDormancyOnSet() const
{
	if (!GetVariableSourceClass()->IsChildOf(AActor::StaticClass()))
	{
		return false;
	}

	// Flush net dormancy before setting a replicated property
	UProperty *Property = FindField<UProperty>(GetVariableSourceClass(), GetVarName());
	return (Property != NULL && (Property->PropertyFlags & CPF_Net));
}

FName UK2Node_VariableSet::GetRepNotifyName() const
{
	UProperty * Property = GetPropertyForVariable();
	if (Property)
	{
		return Property->RepNotifyFunc;
	}
	return NAME_None;
}


FNodeHandlingFunctor* UK2Node_VariableSet::CreateNodeHandler(FKismetCompilerContext& CompilerContext) const
{
	return new FKCHandler_VariableSet(CompilerContext);
}

FString UK2Node_VariableSet::GetVariableOutputPinName() const
{
	return TEXT("Output_Get");
}

void UK2Node_VariableSet::CreateOutputPinTooltip()
{
	UEdGraphPin* Pin = FindPin(GetVariableOutputPinName());
	check(Pin);
	Pin->PinToolTip = NSLOCTEXT("K2Node", "SetPinOutputTooltip", "Retrieves the value of the variable, can use instead of a separate Get node").ToString();
}

FString UK2Node_VariableSet::GetPinNameOverride(const UEdGraphPin& Pin) const
{
	// Stop the output pin for the variable, effectively the "get" pin, from displaying a name.
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	if(Pin.Direction == EGPD_Output || Pin.PinType.PinCategory == K2Schema->PC_Exec)
	{
		return FString();
	}

	return !Pin.PinFriendlyName.IsEmpty() ? Pin.PinFriendlyName.ToString() : Pin.PinName;
}

void UK2Node_VariableSet::ExpandNode(class FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	if (CompilerContext.bIsFullCompile)
	{
		const UEdGraphSchema_K2* K2Schema = CompilerContext.GetSchema();

		UEdGraphPin* Pin = FindPin(GetVariableOutputPinName());
		if(Pin)
		{
			// If the output pin is linked, we need to spawn a separate "Get" node and hook it up.
			if(Pin->LinkedTo.Num())
			{
				UProperty* VariableProperty = GetPropertyForVariable();

				if(VariableProperty)
				{
					UK2Node_VariableGet* VariableGetNode = CompilerContext.SpawnIntermediateNode<UK2Node_VariableGet>(this, SourceGraph);
					VariableGetNode->VariableReference = VariableReference;
					VariableGetNode->AllocateDefaultPins();
					CompilerContext.MessageLog.NotifyIntermediateObjectCreation(VariableGetNode, this);
					CompilerContext.MovePinLinksToIntermediate(*Pin, *VariableGetNode->FindPin(GetVarNameString()));

					// Duplicate the connection to the self pin.
					UEdGraphPin* SetSelfPin = K2Schema->FindSelfPin(*this, EGPD_Input);
					UEdGraphPin* GetSelfPin = K2Schema->FindSelfPin(*VariableGetNode, EGPD_Input);
					if(SetSelfPin && GetSelfPin)
					{
						CompilerContext.CopyPinLinksToIntermediate(*SetSelfPin, *GetSelfPin);
					}
				}
			}
			Pins.Remove(Pin);
		}
		
	}

}

#undef LOCTEXT_NAMESPACE
