// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "BlueprintEditorPrivatePCH.h"
#include "BlueprintActionMenuItem.h"
#include "BlueprintNodeSpawner.h"
#include "KismetEditorUtilities.h"	// for BringKismetToFocusAttentionOnObject()
#include "BlueprintEditorUtils.h"	// for AnalyticsTrackNewNode(), MarkBlueprintAsModified(), etc.
#include "ScopedTransaction.h"
#include "SNodePanel.h"				// for GetSnapGridSize()

#define LOCTEXT_NAMESPACE "BlueprintActionMenuItem"

/*******************************************************************************
 * Static FBlueprintMenuActionItem Helpers
 ******************************************************************************/

namespace FBlueprintMenuActionItemImpl
{
	/**
	 * Utility function for marking blueprints dirty and recompiling them after 
	 * a node has been added.
	 * 
	 * @param  SpawnedNode	The node that was just added to the blueprint.
	 */
	static void DirtyBlueprintFromNewNode(UEdGraphNode* SpawnedNode);

	/**
	 * 
	 * 
	 * @param  Action			The action you wish to invoke.
	 * @param  ParentGraph		The graph you want the action to spawn a node into.
	 * @param  Location			The position in the graph that you want the node spawned.
	 * @param  Bindings			Any bindings you want applied after the node has been spawned.
	 * @param  bSelectNewNode	Determines if the new node should be selected after it have been spawned.
	 * @return The spawned node (could be an existing one if the event was already placed).
	 */
	static UEdGraphNode* InvokeAction(const UBlueprintNodeSpawner* Action, UEdGraph* ParentGraph, FVector2D const Location, IBlueprintNodeBinder::FBindingSet const& Bindings, bool bSelectNewNode);
}

//------------------------------------------------------------------------------
static void FBlueprintMenuActionItemImpl::DirtyBlueprintFromNewNode(UEdGraphNode* SpawnedNode)
{
	UEdGraph const* const NodeGraph = SpawnedNode->GetGraph();
	check(NodeGraph != nullptr);
	
	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraphChecked(NodeGraph);
	check(Blueprint != nullptr);
	
	UK2Node* K2Node = Cast<UK2Node>(SpawnedNode);
	// see if we need to recompile skeleton after adding this node, or just mark
	// it dirty (default to rebuilding the skel, since there is no way to if
	// non-k2 nodes structurally modify the blueprint)
	if ((K2Node == nullptr) || K2Node->NodeCausesStructuralBlueprintChange())
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
}

//------------------------------------------------------------------------------
static UEdGraphNode* FBlueprintMenuActionItemImpl::InvokeAction(const UBlueprintNodeSpawner* Action, UEdGraph* ParentGraph, FVector2D const Location, IBlueprintNodeBinder::FBindingSet const& Bindings, bool bSelectNewNode)
{
	// this could return an existing node
	UEdGraphNode* SpawnedNode = Action->Invoke(ParentGraph, Bindings, Location);

	// if a returned node hasn't been added to the graph yet (it must have been freshly spawned)
	if (ParentGraph->Nodes.Find(SpawnedNode) == INDEX_NONE)
	{
		check(SpawnedNode != nullptr);

		// @TODO: Move Modify()/AddNode() into UBlueprintNodeSpawner and pull 
		//        selection functionality out to FBlueprintActionMenuItem::PerformAction()
		ParentGraph->Modify();
		ParentGraph->AddNode(SpawnedNode, /*bFromUI =*/true, bSelectNewNode);
		// @TODO: if this spawned multiple nodes, then we should be selecting all of them

		SpawnedNode->SnapToGrid(SNodePanel::GetSnapGridSize());

		FBlueprintEditorUtils::AnalyticsTrackNewNode(SpawnedNode);
		DirtyBlueprintFromNewNode(SpawnedNode);
	}
	// if this node already existed, then we just want to focus on that node...
	// some node types are only allowed one instance per blueprint (like events)
	else if (SpawnedNode != nullptr)
	{
		FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(SpawnedNode);
	}

	return SpawnedNode;
}

/*******************************************************************************
 * FBlueprintMenuActionItem
 ******************************************************************************/

//------------------------------------------------------------------------------
FBlueprintActionMenuItem::FBlueprintActionMenuItem(UBlueprintNodeSpawner const* NodeSpawner, FBlueprintActionUiSpec const& UiSpec, IBlueprintNodeBinder::FBindingSet const& Bindings)
	: Action(NodeSpawner)
	, IconTint(UiSpec.IconTint)
	, IconBrush(FEditorStyle::GetBrush(UiSpec.IconName))
	, Bindings(Bindings)
{
	check(Action != nullptr);

	MenuDescription = UiSpec.MenuName;
	Category = UiSpec.Category.ToString();
	TooltipDescription = UiSpec.Tooltip.ToString();
	Keywords = UiSpec.Keywords;
}

//------------------------------------------------------------------------------
UEdGraphNode* FBlueprintActionMenuItem::PerformAction(UEdGraph* ParentGraph, UEdGraphPin* FromPin, FVector2D const Location, bool bSelectNewNode/* = true*/)
{
	using namespace FBlueprintMenuActionItemImpl;
	FScopedTransaction Transaction(LOCTEXT("AddNodeTransaction", "Add Node"));
	
	FVector2D ModifiedLocation = Location;
	if (FromPin != nullptr)
	{
		// for input pins, a new node will generally overlap the node being
		// dragged from... work out if we want add in some spacing from the connecting node
		if (FromPin->Direction == EGPD_Input)
		{
			UEdGraphNode* FromNode = FromPin->GetOwningNode();
			check(FromNode != nullptr);
			float const FromNodeX = FromNode->NodePosX;

			static const float MinNodeDistance = 60.f; // min distance between spawned nodes (to keep them from overlapping)
			if (MinNodeDistance > FMath::Abs(FromNodeX - Location.X))
			{
				ModifiedLocation.X = FromNodeX - MinNodeDistance;
			}
		}

		// modify before the call to AutowireNewNode() below
		FromPin->Modify();
	}

	UEdGraphNode* LastSpawnedNode = nullptr;
	auto BoundObjIt = Bindings.CreateConstIterator();
	do
	{
		IBlueprintNodeBinder::FBindingSet BindingsSubset;
		for (; BoundObjIt && (Action->CanBindMultipleObjects() || (BindingsSubset.Num() == 0)); ++BoundObjIt)
		{
			if (BoundObjIt->IsValid())
			{
				BindingsSubset.Add(BoundObjIt->Get());
			}
		}

		LastSpawnedNode = InvokeAction(Action, ParentGraph, ModifiedLocation, BindingsSubset, bSelectNewNode);
		if (FromPin != nullptr)
		{
			// make sure to auto-wire after we position the new node (in case
			// the auto-wire creates a conversion node to put between them)
			LastSpawnedNode->AutowireNewNode(FromPin);
		}

		// Increase the node location a safe distance so follow-up nodes are not stacked
		ModifiedLocation.Y += UEdGraphSchema_K2::EstimateNodeHeight(LastSpawnedNode);

	} while (BoundObjIt);
	// @TODO: select ALL spawned nodes
	
	return LastSpawnedNode;
}

//------------------------------------------------------------------------------
UEdGraphNode* FBlueprintActionMenuItem::PerformAction(UEdGraph* ParentGraph, TArray<UEdGraphPin*>& FromPins, FVector2D const Location, bool bSelectNewNode/* = true*/)
{
	UEdGraphPin* FromPin = nullptr;
	if (FromPins.Num() > 0)
	{
		FromPin = FromPins[0];
	}
	
	UEdGraphNode* SpawnedNode = PerformAction(ParentGraph, FromPin, Location, bSelectNewNode);
	// try auto-wiring the rest of the pins (if there are any)
	for (int32 PinIndex = 1; PinIndex < FromPins.Num(); ++PinIndex)
	{
		SpawnedNode->AutowireNewNode(FromPins[PinIndex]);
	}
	
	return SpawnedNode;
}

//------------------------------------------------------------------------------
void FBlueprintActionMenuItem::AddReferencedObjects(FReferenceCollector& Collector)
{
	FEdGraphSchemaAction::AddReferencedObjects(Collector);
	
	// these don't get saved to disk, but we want to make sure the objects don't
	// get GC'd while the action array is around
	Collector.AddReferencedObject(Action);
}

//------------------------------------------------------------------------------
void FBlueprintActionMenuItem::AppendBindings(const FBlueprintActionContext& Context, IBlueprintNodeBinder::FBindingSet const& BindingSet)
{
	Bindings.Append(BindingSet);

	FBlueprintActionUiSpec UiSpec = Action->GetUiSpec(Context, Bindings);
	// ui signature could be dynamic, and change as bindings change
	MenuDescription = UiSpec.MenuName;
	// @TODO: would invalidate any category pre-pending that was done at the 
	//        MenuBuilder level
	//Category  = UiSpec.Category.ToString();
	TooltipDescription = UiSpec.Tooltip.ToString();
	Keywords  = UiSpec.Keywords;	
	IconBrush = FEditorStyle::GetBrush(UiSpec.IconName);
	IconTint  = UiSpec.IconTint;
}

//------------------------------------------------------------------------------
FSlateBrush const* FBlueprintActionMenuItem::GetMenuIcon(FSlateColor& ColorOut)
{
	ColorOut = IconTint;
	return IconBrush;
}

//------------------------------------------------------------------------------
UBlueprintNodeSpawner const* FBlueprintActionMenuItem::GetRawAction() const
{
	return Action;
}

#undef LOCTEXT_NAMESPACE
