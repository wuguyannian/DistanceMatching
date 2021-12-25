// Copyright Roman Merkushin. All Rights Reserved.

#include "Animation/AnimNode_DistanceMatching.h"
#include "Log.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimCurveCompressionCodec_UniformIndexable.h"

/**
* update by wuguyannian
* 1. Compression problem, Don't worry about compression
* 2. Non-unique boundary value
*/

#if ENABLE_ANIM_DEBUG
namespace DistanceMatchingCVars
{
	static int32 AnimNodeEnable = 1;
	FAutoConsoleVariableRef CVarAnimNodeEnable(
		TEXT("a.AnimNode.DistanceMatching.Enable"),
		AnimNodeEnable,
		TEXT("Turn on debug for DistanceMatching AnimNode."),
		ECVF_Default);
}  // namespace DistanceMatchingCVars
#endif

FAnimNode_DistanceMatching::FAnimNode_DistanceMatching()
	: CurveRef(nullptr)
	, PrevSequence(nullptr)
	, CurveBufferNumSamples(0)
	, PrevKey(0)
	, bIsEnabled(true)
	, Sequence(nullptr)
	, DistanceCurveName(FName("Distance"))
	, Distance(0.0f)
	, bEnableCacheKey(true)
	, bIsUpperBoundUnique(true)
	, bIsLowerBoundUnique(true)
	, bEnableDistanceLimit(false)
	, DistanceLimit(0.0f)
{
}

float FAnimNode_DistanceMatching::GetCurrentAssetLength()
{
	return Sequence ? Sequence->GetPlayLength() : 0.0f;
}

void FAnimNode_DistanceMatching::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_ANIMNODE(Initialize_AnyThread)
	FAnimNode_AssetPlayerBase::Initialize_AnyThread(Context);
	GetEvaluateGraphExposedInputs().Execute(Context);

	// Update CurveBuffer if sequence is changed or is nullptr
	if (!Sequence || Sequence && Sequence != PrevSequence)
	{
		PrevSequence = Sequence;
		UpdateCurveBuffer();
	}

	InternalTimeAccumulator = 0.f;
	PrevKey = 0;
}

void FAnimNode_DistanceMatching::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_ANIMNODE(CacheBones_AnyThread)
}

void FAnimNode_DistanceMatching::UpdateAssetPlayer(const FAnimationUpdateContext& Context)
{
	GetEvaluateGraphExposedInputs().Execute(Context);

#if ENABLE_ANIM_DEBUG
	bIsEnabled = DistanceMatchingCVars::AnimNodeEnable == 1;
#endif

	if (Sequence && Context.AnimInstanceProxy->IsSkeletonCompatible(Sequence->GetSkeleton()))
	{
		if (bIsEnabled)
		{
			if (!CurveRef)
			{
				UE_LOG(LogDistanceMatching, Error, TEXT("CurveRef is nullptr!"));
				return;
			}

			if (bEnableDistanceLimit && Distance >= DistanceLimit)
			{
				if (InternalTimeAccumulator == 0.f)
					InternalTimeAccumulator = FMath::Clamp(GetCurveTime(), 0.0f, Sequence->GetPlayLength());

				PlaySequence(Context);
			}
			else
			{
				float CurrentTime = InternalTimeAccumulator;
				float TargetTime = FMath::Clamp(GetCurveTime(), 0.0f, Sequence->GetPlayLength());

				if (TargetTime > CurrentTime)
					CurrentTime = TargetTime;
				else
					CurrentTime += Context.GetDeltaTime() * Sequence->RateScale;

				InternalTimeAccumulator = FMath::Clamp(CurrentTime, 0.0f, Sequence->GetPlayLength());
			}
		}
		else
		{
			PlaySequence(Context);
		}
	}

	TRACE_ANIM_NODE_VALUE(Context, TEXT("Name"), Sequence != nullptr ? Sequence->GetFName() : NAME_None);
	TRACE_ANIM_NODE_VALUE(Context, TEXT("Sequence"), Sequence);
	TRACE_ANIM_NODE_VALUE(Context, TEXT("Distance"), Distance);
	TRACE_ANIM_NODE_VALUE(Context, TEXT("Time"), InternalTimeAccumulator);
}

void FAnimNode_DistanceMatching::Evaluate_AnyThread(FPoseContext& Output)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_ANIMNODE(Evaluate_AnyThread)
	check(Output.AnimInstanceProxy != nullptr);
	if (Sequence && Output.AnimInstanceProxy->IsSkeletonCompatible(Sequence->GetSkeleton()))
	{
		FAnimationPoseData AnimationPoseData(Output);
		Sequence->GetAnimationPose(AnimationPoseData, FAnimExtractContext(InternalTimeAccumulator, Output.AnimInstanceProxy->ShouldExtractRootMotion()));
	}
	else
	{
		Output.ResetToRefPose();
	}
}

void FAnimNode_DistanceMatching::OverrideAsset(UAnimationAsset* NewAsset)
{
	if (UAnimSequenceBase* AnimSequence = Cast<UAnimSequenceBase>(NewAsset))
	{
		Sequence = AnimSequence;
	}
}

void FAnimNode_DistanceMatching::GatherDebugData(FNodeDebugData& DebugData)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_ANIMNODE(GatherDebugData)
	FString DebugLine = DebugData.GetNodeName(this);

	DebugLine += FString::Printf(TEXT("('%s' Distance: %.3f, Time: %.3f)"), *GetNameSafe(Sequence), Distance, InternalTimeAccumulator);
	DebugData.AddDebugItem(DebugLine, true);
}

void FAnimNode_DistanceMatching::UpdateCurveBuffer()
{
	if (!Sequence)
	{
		UE_LOG(LogDistanceMatching, Error, TEXT("Sequence is null with name: %s."), *DistanceCurveName.ToString());
		return;
	}

	for (auto CurveIt = Sequence->GetCurveData().FloatCurves.CreateConstIterator(); CurveIt; CurveIt++)
	{
		if (CurveIt->Name.DisplayName == DistanceCurveName)
		{
			CurveRef = &*CurveIt;
			CurveBufferNumSamples = CurveRef->FloatCurve.GetNumKeys();
			return;
		}
	}
	UE_LOG(LogDistanceMatching, Error, TEXT("Can't find the curve by name: %s."), *DistanceCurveName.ToString());
}

float FAnimNode_DistanceMatching::GetCurveTime()
{
	if (CurveBufferNumSamples == 0)
	{
		// If no keys in curve, return 0
		return 0.0f;
	}

	if (CurveBufferNumSamples < 2)
	{
		return CurveRef->FloatCurve.Keys[0].Time;
	}

	if (!bIsLowerBoundUnique && Distance == CurveRef->FloatCurve.Keys[0].Value)
	{
		return CurveRef->FloatCurve.Keys[0].Time;
	}

	if (Distance >= CurveRef->FloatCurve.Keys[CurveBufferNumSamples - 1].Value)
	{
		if (bIsUpperBoundUnique)
		{
			return CurveRef->FloatCurve.Keys[CurveBufferNumSamples - 1].Time;
		}
		else if (bEnableCacheKey && Distance == CurveRef->FloatCurve.Keys[PrevKey].Value)
		{
			return CurveRef->FloatCurve.Keys[PrevKey].Time;
		}
		else
		{
			int32 First = 0;
			int32 Last = CurveBufferNumSamples - 1;

			while (First <= Last)
			{
				int32 Middle = First + (Last - First) / 2;
				if ((Middle == 0 || CurveRef->FloatCurve.Keys[Middle - 1].Value < Distance) && CurveRef->FloatCurve.Keys[Middle].Value == Distance)
				{
					PrevKey = Middle;
					return CurveRef->FloatCurve.Keys[Middle].Time;
				}
				else if (Distance > CurveRef->FloatCurve.Keys[Middle].Value)
				{
					First = Middle + 1;
				}
				else
				{
					Last = Middle - 1;
				}
			}
			PrevKey = CurveBufferNumSamples - 1;
			return CurveRef->FloatCurve.Keys[CurveBufferNumSamples - 1].Time;
		}
	}
	else
	{
		// Perform a lower bound to get the second of the interpolation nodes
		int32 First = bEnableCacheKey ? PrevKey + 1 : 1;
		const int32 Last = CurveBufferNumSamples - 1;
		int32 Count = Last - First;

		while (Count > 0)
		{
			const int32 Step = Count / 2;
			const int32 Middle = First + Step;

			if (Distance >= CurveRef->FloatCurve.Keys[Middle].Value)
			{
				First = Middle + 1;
				Count -= Step + 1;
			}
			else
			{
				Count = Step;
			}
		}

		const float Diff = CurveRef->FloatCurve.Keys[First].Value - CurveRef->FloatCurve.Keys[First - 1].Value;
		PrevKey = First - 1;

		if (Diff > 0.0f)
		{
			const float Alpha = (Distance - CurveRef->FloatCurve.Keys[First - 1].Value) / Diff;
			const float P0 = CurveRef->FloatCurve.Keys[First - 1].Time;
			const float P3 = CurveRef->FloatCurve.Keys[First].Time;

			// Find time by two nearest known points on the curve
			return FMath::Lerp(P0, P3, Alpha);
		}

		return CurveRef->FloatCurve.Keys[First - 1].Time;
	}
}

void FAnimNode_DistanceMatching::PlaySequence(const FAnimationUpdateContext& Context)
{
	InternalTimeAccumulator = FMath::Clamp(InternalTimeAccumulator, 0.f, Sequence->GetPlayLength());
	CreateTickRecordForNode(Context, Sequence, false, 1.0f);
}
