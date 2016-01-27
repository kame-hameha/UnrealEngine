// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.
// Copyright (C) Microsoft. All rights reserved.

/*=============================================================================
	GPUSkinCache.cpp: Performs skinning on a compute shader into a buffer to avoid vertex buffer skinning.
=============================================================================*/

#include "EnginePrivate.h"
#include "GlobalShader.h"
#include "SceneManagement.h"
#include "GPUSkinVertexFactory.h"
#include "SkeletalRenderGPUSkin.h"
#include "GPUSkinCache.h"
#include "ShaderParameterUtils.h"
#include "SceneUtils.h"

DEFINE_STAT(STAT_GPUSkinCache_TotalNumChunks);
DEFINE_STAT(STAT_GPUSkinCache_TotalNumVertices);
DEFINE_STAT(STAT_GPUSkinCache_TotalMemUsed);
DEFINE_STAT(STAT_GPUSkinCache_SkippedForMaxChunksPerLOD);
DEFINE_STAT(STAT_GPUSkinCache_SkippedForChunksPerFrame);
DEFINE_STAT(STAT_GPUSkinCache_SkippedForZeroInfluences);
DEFINE_STAT(STAT_GPUSkinCache_SkippedForCloth);
DEFINE_STAT(STAT_GPUSkinCache_SkippedForMorphs);
DEFINE_STAT(STAT_GPUSkinCache_SkippedForMemory);
DEFINE_STAT(STAT_GPUSkinCache_SkippedForAlreadyCached);
DEFINE_STAT(STAT_GPUSkinCache_SkippedForOutOfECS);

int32 GEnableGPUSkinCacheShaders = 0;
static FAutoConsoleVariableRef CVarEnableGPUSkinCacheShaders(
	TEXT("r.SkinCacheShaders"),
	GEnableGPUSkinCacheShaders,
	TEXT("Whether or not to compile the GPU compute skinning cache shaders.\n")
	TEXT("This will compile the shaders for skinning on a compute job and not skin on the vertex shader.\n")
	TEXT("GPUSkinVertexFactory.usf needs to be touched to cause a recompile if this changes.\n")
	TEXT("0 is off(default), 1 is on"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
	);

int32 GEnableGPUSkinCache = 0;
static TAutoConsoleVariable<int32> CVarEnableGPUSkinCache(
	TEXT("r.SkinCaching"),
	0,
	TEXT("Whether or not to use the GPU compute skinning cache.\n")
	TEXT("This will perform skinning on a compute job and not skin on the vertex shader.\n")
	TEXT("Requires r.SkinCacheShaders=1.\n")
	TEXT("0 is off(default), 1 is on"),
	ECVF_RenderThreadSafe
	);

static int32 GMaxGPUSkinCacheElementsPerFrame = 1000;
static FAutoConsoleVariableRef CVarMaxGPUSkinCacheElementsPerFrame(
	TEXT("r.MaxGPUSkinCacheElementsPerFrame"),
	GMaxGPUSkinCacheElementsPerFrame,
	TEXT("The maximum compute processed skin cache elements per frame. Only used with r.SkinCaching=1\n")
	TEXT(" (default is 1000)")
	);

static int32 GGPUSkinCacheBufferSize = 99 * 1024 * 1024;
static FAutoConsoleVariableRef CVarMaxGPUSkinCacheBufferSize(
	TEXT("r.SkinCache.BufferSize"),
	GMaxGPUSkinCacheElementsPerFrame,
	TEXT("The maximum memory used for writing out the vertices in bytes\n")
	TEXT("Split into GPUSKINCACHE_FRAMES chunks (eg 300 MB means 100 MB for 3 frames)\n")
	TEXT("Default is 99MB.  Only used with r.SkinCaching=1\n"),
	ECVF_ReadOnly | ECVF_RenderThreadSafe
	);

TGlobalResource<FGPUSkinCache> GGPUSkinCache;

IMPLEMENT_UNIFORM_BUFFER_STRUCT(GPUSkinCacheBonesUniformShaderParameters,TEXT("GPUSkinCacheBones"));


class FBaseGPUSkinCacheCS : public FGlobalShader
{
public:
	FBaseGPUSkinCacheCS() {} 

	FBaseGPUSkinCacheCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		SkinMeshOriginParameter.Bind(Initializer.ParameterMap, TEXT("MeshOrigin"));
		SkinMeshExtensionParameter.Bind(Initializer.ParameterMap, TEXT("MeshExtension"));

		//DebugParameter.Bind(Initializer.ParameterMap, TEXT("DebugParameter"));

		SkinInputStreamStride.Bind(Initializer.ParameterMap, TEXT("SkinCacheVertexStride"));
		SkinInputStreamVertexCount.Bind(Initializer.ParameterMap, TEXT("SkinCacheVertexCount"));
		SkinInputStreamFloatOffset.Bind(Initializer.ParameterMap, TEXT("SkinCacheInputStreamFloatOffset"));
		SkinOutputBufferFloatOffset.Bind(Initializer.ParameterMap, TEXT("SkinCacheOutputBufferFloatOffset"));
		BoneMatrices.Bind(Initializer.ParameterMap, TEXT("BoneMatrices"));
		SkinInputStream.Bind(Initializer.ParameterMap, TEXT("SkinStreamInputBuffer"));

		SkinCacheBuffer.Bind(Initializer.ParameterMap, TEXT("SkinCacheBuffer"));
	}

	void SetParameters(FRHICommandList& RHICmdList, uint32 InputStreamStride, uint32 InputStreamFloatOffset, uint32 InputStreamVertexCount, uint32 OutputBufferFloatOffset, const FVertexBufferAndSRV& BoneBuffer, FUniformBufferRHIRef UniformBuffer, FShaderResourceViewRHIRef VertexBufferSRV, FRWBuffer& SkinBuffer, const FVector& MeshOrigin, const FVector& MeshExtension)
	{
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
		SetShaderValue(RHICmdList, ShaderRHI, SkinMeshOriginParameter, MeshOrigin);
		SetShaderValue(RHICmdList, ShaderRHI, SkinMeshExtensionParameter, MeshExtension);

		SetShaderValue(RHICmdList, ShaderRHI, SkinInputStreamStride, InputStreamStride);
		SetShaderValue(RHICmdList, ShaderRHI, SkinInputStreamVertexCount, InputStreamVertexCount);
		SetShaderValue(RHICmdList, ShaderRHI, SkinInputStreamFloatOffset, InputStreamFloatOffset);
		SetShaderValue(RHICmdList, ShaderRHI, SkinOutputBufferFloatOffset, OutputBufferFloatOffset);
		//SetShaderValue(RHICmdList, ShaderRHI, DebugParameter, FUintVector4((uint32)(uint64)VertexBufferSRV.GetReference(), 0, 0, 1));

		if (UniformBuffer)
		{
			SetUniformBufferParameter(RHICmdList, ShaderRHI, GetUniformBufferParameter<GPUSkinCacheBonesUniformShaderParameters>(), UniformBuffer);
		}
		else
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BoneMatrices.GetBaseIndex(), BoneBuffer.VertexBufferSRV);
		}

		RHICmdList.SetShaderResourceViewParameter(ShaderRHI, SkinInputStream.GetBaseIndex(), VertexBufferSRV);

		RHICmdList.SetUAVParameter(ShaderRHI, SkinCacheBuffer.GetBaseIndex(), SkinBuffer.UAV);
	}

	void UnsetParameters(FRHICommandList& RHICmdList)
	{
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
		FShaderResourceViewRHIParamRef NullSRV = FShaderResourceViewRHIParamRef();
		RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BoneMatrices.GetBaseIndex(), NullSRV);

		RHICmdList.SetShaderResourceViewParameter(ShaderRHI, SkinInputStream.GetBaseIndex(), NullSRV);

		RHICmdList.SetUAVParameter(ShaderRHI, SkinCacheBuffer.GetBaseIndex(), FUnorderedAccessViewRHIParamRef() );
	}

	virtual bool Serialize(FArchive& Ar) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar  << SkinMeshOriginParameter << SkinMeshExtensionParameter << SkinInputStreamStride
			<< SkinInputStreamVertexCount << SkinInputStreamFloatOffset << SkinOutputBufferFloatOffset
			<< SkinInputStream << SkinCacheBuffer << BoneMatrices;
		//Ar << DebugParameter;
		return bShaderHasOutdatedParameters;
	}

private:

	FShaderParameter SkinMeshOriginParameter;
	FShaderParameter SkinMeshExtensionParameter;

	FShaderParameter SkinInputStreamStride;
	FShaderParameter SkinInputStreamVertexCount;
	FShaderParameter SkinInputStreamFloatOffset;
	FShaderParameter SkinOutputBufferFloatOffset;

	//FShaderParameter DebugParameter;

	FShaderUniformBufferParameter SkinUniformBuffer;

	FShaderResourceParameter BoneMatrices;
	FShaderResourceParameter SkinInputStream;
	FShaderResourceParameter SkinCacheBuffer;
};

/** Compute shader that skins a batch of vertices. */
template <bool bUseExtraBoneInfluencesT>
class TGPUSkinCacheCS : public FBaseGPUSkinCacheCS
{
	DECLARE_SHADER_TYPE(TGPUSkinCacheCS<bUseExtraBoneInfluencesT>,Global)
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return GEnableGPUSkinCacheShaders && IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform,OutEnvironment);
		const uint32 UseExtraBoneInfluences = bUseExtraBoneInfluencesT;
		OutEnvironment.SetDefine(TEXT("GPUSKIN_USE_EXTRA_INFLUENCES"), UseExtraBoneInfluences);

		OutEnvironment.SetDefine(TEXT("GPUSKIN_RWBUFFER_OFFSET_POSITION"), FGPUSkinCache::RWPositionOffsetInFloats);
		OutEnvironment.SetDefine(TEXT("GPUSKIN_RWBUFFER_OFFSET_TANGENT_X"), FGPUSkinCache::RWTangentXOffsetInFloats);
		OutEnvironment.SetDefine(TEXT("GPUSKIN_RWBUFFER_OFFSET_TANGENT_Z"), FGPUSkinCache::RWTangentZOffsetInFloats);
		OutEnvironment.SetDefine(TEXT("GPUSKIN_RWBUFFER_NUM_FLOATS"), FGPUSkinCache::RWStrideInFloats);
	}

	TGPUSkinCacheCS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FBaseGPUSkinCacheCS(Initializer)
	{
	}

	TGPUSkinCacheCS()
	{
	}
};

FGPUSkinCache::FGPUSkinCache()
	: bInitialized(false)
	, FrameCounter(0)
	, CacheMaxVectorCount(0)
	, CacheCurrentFloatOffset(0)
	, CachedChunksThisFrameCount(0)
{
}

FGPUSkinCache::~FGPUSkinCache()
{
	Cleanup();
}

void FGPUSkinCache::ReleaseRHI()
{
	Cleanup();
}

void FGPUSkinCache::Initialize(FRHICommandListImmediate& RHICmdList)
{
	check(!bInitialized);

	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.BasePassOutputsVelocity"));
	checkf(!CVar || CVar->GetValueOnRenderThread() == 0, TEXT("GPU Skin caching is not allowed with outputting velocity on base pass (r.BasePassOutputsVelocity=1)"));

	checkf(GEnableGPUSkinCacheShaders, TEXT("GPU Skin caching requires the shaders enabled (r.SkinCacheShaders=1)"));

	int32 NumFloatsPerBuffer = GGPUSkinCacheBufferSize / GPUSKINCACHE_FRAMES / sizeof(float);
	for (int32 Index = 0; Index < GPUSKINCACHE_FRAMES; ++Index)
	{
		SkinCacheBuffer[Index].Initialize(sizeof(float), NumFloatsPerBuffer, PF_R32_FLOAT, BUF_Static);

		FString Name = FString::Printf(TEXT("SkinCacheRWBuffer%d"), Index);
		RHICmdList.BindDebugLabelName(SkinCacheBuffer[Index].UAV, *Name);
	}

	TransitionAllToWriteable(RHICmdList);

	CachedElements.Reserve(MaxCachedElements);
	CachedVertexBuffers.Reserve(MaxCachedVertexBufferSRVs);

	bInitialized = true;
}

void FGPUSkinCache::Cleanup()
{
	if (bInitialized)
	{
		for (int32 Index = 0; Index < GPUSKINCACHE_FRAMES; ++Index)
		{
			SkinCacheBuffer[Index].Release();
		}

		bInitialized = false;
	}
}

// Kick off compute shader skinning for incoming chunk, return key for fast lookup in draw passes
int32 FGPUSkinCache::StartCacheMesh(FRHICommandListImmediate& RHICmdList, int32 Key, FGPUBaseSkinVertexFactory* VertexFactory, FGPUSkinPassthroughVertexFactory* TargetVertexFactory, const FSkelMeshChunk& BatchElement, const FSkeletalMeshObjectGPUSkin* Skin)
{
	if (CachedChunksThisFrameCount >= GMaxGPUSkinCacheElementsPerFrame && GFrameNumberRenderThread <= FrameCounter)
	{
		INC_DWORD_STAT(STAT_GPUSkinCache_SkippedForChunksPerFrame);
		return -1;
	}

	if (!bInitialized)
	{
		Initialize(RHICmdList);
	}

	if (GFrameNumberRenderThread > FrameCounter)	// Reset cache output if on a new frame
	{
		CacheCurrentFloatOffset = 0;
		CachedChunksThisFrameCount = 0;
		FrameCounter = GFrameNumberRenderThread;
	}

	FElementCacheStatusInfo::FSearchInfo SInfo;
	SInfo.BatchElement = &BatchElement;
	SInfo.Skin = Skin;

	FElementCacheStatusInfo* ECSInfo = nullptr;

	if (Key >= 0 && Key < CachedElements.Num())
	{
		ECSInfo = &(CachedElements.GetData()[Key]);

		if (ECSInfo->Skin != Skin || &BatchElement != ECSInfo->BatchElement)
		{
			ECSInfo = CachedElements.FindByKey(SInfo);
		}
	}
	else
	{
		ECSInfo = CachedElements.FindByKey(SInfo);
	}

	bool AlignStreamOffsetsFrames = false;

	auto* GPUVertexFactory = (FGPUBaseSkinVertexFactory*)VertexFactory;
	const bool bExtraBoneInfluences = GPUVertexFactory->UsesExtraBoneInfluences();
	bool bHasPreviousOffset = false;
	if (ECSInfo != nullptr)
	{
		if (ECSInfo->FrameUpdated == GFrameNumberRenderThread)	// Already cached this element this frame, return 0 key
		{
			INC_DWORD_STAT(STAT_GPUSkinCache_SkippedForAlreadyCached);
			return -1;
		}

		AlignStreamOffsetsFrames = (GFrameNumberRenderThread - ECSInfo->FrameUpdated) > 2;

		ECSInfo->FrameUpdated = GFrameNumberRenderThread;
		ECSInfo->PreviousFrameStreamOffset = ECSInfo->StreamOffset;
		bHasPreviousOffset = true;
	}
	else	// New element, init and add
	{
		if (CachedElements.Num() < MaxCachedElements)
		{
			FElementCacheStatusInfo	Info;
			Info.BatchElement = &BatchElement;
			Info.VertexFactory = VertexFactory;
			Info.TargetVertexFactory = TargetVertexFactory;
			Info.Skin = Skin;
			Info.Key = CachedElements.Num();
			Info.VertexBufferSRVIndex = -1;
			Info.FrameUpdated = GFrameNumberRenderThread;
			Info.bExtraBoneInfluences = bExtraBoneInfluences;
			CachedElements.Add(Info);

			ECSInfo = &(CachedElements.GetData()[Info.Key]);
		}
		else
		{
			FElementCacheStatusInfo*	Info = FindEvictableCacheStatusInfo();

			if (Info == nullptr)
			{
				INC_DWORD_STAT(STAT_GPUSkinCache_SkippedForOutOfECS);
				return -1;
			}

			Info->BatchElement = &BatchElement;
			Info->VertexFactory = VertexFactory;
			Info->TargetVertexFactory = TargetVertexFactory;
			Info->Skin = Skin;
			Info->VertexBufferSRVIndex = -1;
			Info->FrameUpdated = GFrameNumberRenderThread;
			Info->bExtraBoneInfluences = bExtraBoneInfluences;

			ECSInfo = Info;
		}

		AlignStreamOffsetsFrames = true;
	}

	uint32	StreamStrides[MaxVertexElementCount];
	uint32	StreamStrideCount = VertexFactory->GetStreamStrides(StreamStrides);

	uint32	NumVertices = BatchElement.GetNumVertices();

	uint32	InputStreamFloatOffset = (StreamStrides[0] * BatchElement.BaseVertexIndex) / sizeof(float);

	uint32	NumRWFloats = (RWStrideInFloats * NumVertices);

	int32 CompensatedOffset = ((int32)CacheCurrentFloatOffset * sizeof(float)) - (BatchElement.BaseVertexIndex * StreamStrides[0]);
	if (CompensatedOffset < 0)
	{
		CacheCurrentFloatOffset -= CompensatedOffset / sizeof(float);
	}

	if (CacheCurrentFloatOffset + NumRWFloats > (uint32)GGPUSkinCacheBufferSize)
	{
		// Can't fit this
		INC_DWORD_STAT(STAT_GPUSkinCache_SkippedForMemory);
		return -1;
	}

	INC_DWORD_STAT_BY(STAT_GPUSkinCache_TotalMemUsed, NumRWFloats * sizeof(float));

	ECSInfo->InputVBStride = StreamStrides[0];
	ECSInfo->StreamOffset = CacheCurrentFloatOffset;

	if (AlignStreamOffsetsFrames || !bHasPreviousOffset)
	{
		ECSInfo->PreviousFrameStreamOffset = ECSInfo->StreamOffset;
	}

	const FVertexBuffer* SkinVertexBuffer = GPUVertexFactory->GetSkinVertexBuffer();
	FVertexBufferInfo* VBInfo = nullptr;

	if (ECSInfo->VertexBufferSRVIndex >= 0)
	{
		VBInfo = &(CachedVertexBuffers.GetData()[ECSInfo->VertexBufferSRVIndex]);
	}
	else
	{
		// time complexity: O(n) 
		VBInfo = CachedVertexBuffers.FindByKey(*SkinVertexBuffer);

		if (VBInfo)
		{
			ECSInfo->VertexBufferSRVIndex = VBInfo->Index;
		}
	}

	if (VBInfo == nullptr)	// Add new cache entry for vertex buffer and SRV
	{
		FVertexBufferInfo	Info;
		Info.VertexBuffer = SkinVertexBuffer;
		Info.VertexBufferSRV = RHICreateShaderResourceView(SkinVertexBuffer->VertexBufferRHI, sizeof(float), PF_R32_FLOAT);
		int32 Index = CachedVertexBuffers.Add(Info);
		VBInfo = &(CachedVertexBuffers.GetData()[Index]);
		VBInfo->Index = Index;
		ECSInfo->VertexBufferSRVIndex = Index;
	}


	INC_DWORD_STAT(STAT_GPUSkinCache_TotalNumChunks);
	INC_DWORD_STAT_BY(STAT_GPUSkinCache_TotalNumVertices, NumVertices);

	auto& ShaderData = GPUVertexFactory->GetShaderData();
	
	DispatchSkinCacheProcess(RHICmdList, InputStreamFloatOffset, ECSInfo->StreamOffset, ShaderData.GetBoneBufferForWriting(false, GFrameNumberRenderThread), ShaderData.GetUniformBuffer(), VBInfo, StreamStrides[0], NumVertices, ShaderData.MeshOrigin, ShaderData.MeshExtension, bExtraBoneInfluences, Skin->GetFeatureLevel());

	TargetVertexFactory->UpdateVertexDeclaration(VertexFactory, &SkinCacheBuffer[FrameCounter % GPUSKINCACHE_FRAMES]);

	CacheCurrentFloatOffset += NumRWFloats;
	CachedChunksThisFrameCount++;

	return ECSInfo->Key;
}

void FGPUSkinCache::TransitionToReadable(FRHICommandList& RHICmdList)
{
	int32 BufferIndex = FrameCounter % GPUSKINCACHE_FRAMES;
	FUnorderedAccessViewRHIParamRef OutUAVs[] ={SkinCacheBuffer[BufferIndex].UAV};
	RHICmdList.TransitionResources(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EComputeToGfx, OutUAVs, ARRAY_COUNT(OutUAVs));
}

void FGPUSkinCache::TransitionToWriteable(FRHICommandList& RHICmdList)
{
	int32 BufferIndex = (FrameCounter + GPUSKINCACHE_FRAMES - 1) % GPUSKINCACHE_FRAMES;
	FUnorderedAccessViewRHIParamRef OutUAVs[] = {SkinCacheBuffer[BufferIndex].UAV};
	RHICmdList.TransitionResources(EResourceTransitionAccess::ERWNoBarrier, EResourceTransitionPipeline::EGfxToCompute, OutUAVs, ARRAY_COUNT(OutUAVs));
}

void FGPUSkinCache::TransitionAllToWriteable(FRHICommandList& RHICmdList)
{
	if (bInitialized)
	{
		FUnorderedAccessViewRHIParamRef OutUAVs[GPUSKINCACHE_FRAMES];

		for (int32 Index = 0; Index < GPUSKINCACHE_FRAMES; ++Index)
		{
			OutUAVs[Index] = SkinCacheBuffer[Index].UAV;
		}

		RHICmdList.TransitionResources(EResourceTransitionAccess::ERWNoBarrier, EResourceTransitionPipeline::EGfxToCompute, OutUAVs, ARRAY_COUNT(OutUAVs));
	}
}

bool FGPUSkinCache::InternalIsElementProcessed(int32 Key) const
{
	if (Key >= 0 && Key < CachedElements.Num())
	{
		const FElementCacheStatusInfo& CacheInfo = CachedElements.GetData()[Key];

		if (CacheInfo.FrameUpdated == GFrameNumberRenderThread)
		{
			return true;
		}
	}

	return false;
}

bool FGPUSkinCache::InternalSetVertexStreamFromCache(FRHICommandList& RHICmdList, int32 Key, FShader* Shader, const FGPUSkinPassthroughVertexFactory* VertexFactory, uint32 BaseVertexIndex, FShaderParameter PreviousStreamFloatOffset, FShaderResourceParameter PreviousStreamBuffer)
{
	FElementCacheStatusInfo& CacheInfo = CachedElements.GetData()[Key];

	// Verify that we found the correct chunk and that is was processed this frame
	if (CacheInfo.BatchElement->BaseVertexIndex == BaseVertexIndex && CacheInfo.TargetVertexFactory == VertexFactory && CacheInfo.FrameUpdated == GFrameNumberRenderThread)
	{
		// Compensated offset since the StreamOffset is relative to the RWBuffer start, while the SetStreamSource one is relative to the BaseVertexIndex
		int32 CompensatedOffset = ((int32)CacheInfo.StreamOffset * sizeof(float)) - (CacheInfo.BatchElement->BaseVertexIndex * RWStrideInFloats * sizeof(float));
		if (CompensatedOffset < 0)
		{
			return false;
		}

		int32 UAVIndex = FrameCounter % GPUSKINCACHE_FRAMES;
		RHICmdList.SetStreamSource(VertexFactory->GetStreamIndex(UAVIndex), SkinCacheBuffer[UAVIndex].Buffer, RWStrideInFloats * sizeof(float), CompensatedOffset);

		FVertexShaderRHIParamRef ShaderRHI = Shader->GetVertexShader();
		if (ShaderRHI)
		{
			int32 PreviousCompensatedOffset = ((int32)CacheInfo.PreviousFrameStreamOffset * sizeof(float)) - (CacheInfo.BatchElement->BaseVertexIndex * RWStrideInFloats * sizeof(float));
			if (PreviousCompensatedOffset < 0)
			{
				return false;
			}

			if (PreviousStreamBuffer.IsBound())
			{
				int32 PrevUAVIndex = (FrameCounter + GPUSKINCACHE_FRAMES - 1) % GPUSKINCACHE_FRAMES;
				SetShaderValue(RHICmdList, ShaderRHI, PreviousStreamFloatOffset, PreviousCompensatedOffset / sizeof(float));
				RHICmdList.SetShaderResourceViewParameter(ShaderRHI, PreviousStreamBuffer.GetBaseIndex(), SkinCacheBuffer[PrevUAVIndex].SRV);
			}
		}

		return true;
	}

	return false;
}

FGPUSkinCache::FElementCacheStatusInfo* FGPUSkinCache::FindEvictableCacheStatusInfo()
{
	if (!CachedElements.Num())
	{
		return nullptr;
	}

	uint32	BestFrameCounter = 0x7fffffff;
	FElementCacheStatusInfo* BestEntry = nullptr;

	uint32	TargetFrameCounter = GFrameNumberRenderThread >= 10 ? GFrameNumberRenderThread - 10 : 0;

	FElementCacheStatusInfo* CSInfo = CachedElements.GetData();

	for (int32 i = 0; i < CachedElements.Num(); i++)
	{
		if (CSInfo->FrameUpdated < BestFrameCounter)
		{
			BestEntry = CSInfo;
			BestFrameCounter = CSInfo->FrameUpdated;

			if (BestFrameCounter < TargetFrameCounter)
			{
				break;
			}
		}

		CSInfo++;
	}

	if (BestEntry && BestFrameCounter < GFrameNumberRenderThread)
	{
		return BestEntry;
	}
	else
	{
		return nullptr;
	}
}

void FGPUSkinCache::DispatchSkinCacheProcess(FRHICommandListImmediate& RHICmdList, uint32 InputStreamFloatOffset, uint32 OutputBufferFloatOffset, const FVertexBufferAndSRV& BoneBuffer, FUniformBufferRHIRef UniformBuffer, const FVertexBufferInfo* VBInfo, uint32 VertexStride, uint32 VertexCount, const FVector& MeshOrigin, const FVector& MeshExtension, bool bUseExtraBoneInfluences, ERHIFeatureLevel::Type FeatureLevel)
{
	check(FeatureLevel >= ERHIFeatureLevel::SM5);

	uint32 VertexCountAlign64 = FMath::DivideAndRoundUp(VertexCount, (uint32)64);

	SCOPED_DRAW_EVENT(RHICmdList, SkinCacheDispatch);

	int32 UAVIndex = FrameCounter % GPUSKINCACHE_FRAMES;

	TShaderMapRef<TGPUSkinCacheCS<true> > SkinCacheCS1(GetGlobalShaderMap(FeatureLevel));
	TShaderMapRef<TGPUSkinCacheCS<false> > SkinCacheCS0(GetGlobalShaderMap(FeatureLevel));
	FBaseGPUSkinCacheCS* Shader = bUseExtraBoneInfluences ? (FBaseGPUSkinCacheCS*)*SkinCacheCS1 : (FBaseGPUSkinCacheCS*)*SkinCacheCS0;

	RHICmdList.SetComputeShader(Shader->GetComputeShader());
	Shader->SetParameters(RHICmdList, VertexStride, InputStreamFloatOffset, VertexCount, OutputBufferFloatOffset, BoneBuffer, UniformBuffer, VBInfo->VertexBufferSRV, SkinCacheBuffer[UAVIndex], MeshOrigin, MeshExtension);

	RHICmdList.DispatchComputeShader(VertexCountAlign64, 1, 1);
	Shader->UnsetParameters(RHICmdList);
}

void FGPUSkinCache::CVarSinkFunction()
{
	int32 NewGPUSkinCacheValue = CVarEnableGPUSkinCache.GetValueOnAnyThread();
	if (!GEnableGPUSkinCacheShaders)
	{
		NewGPUSkinCacheValue = 0;
	}

	if (NewGPUSkinCacheValue != GEnableGPUSkinCache)
	{
		ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(DoEnableSkinCaching, 
			int32, SkinValue, NewGPUSkinCacheValue,
		{
			GEnableGPUSkinCache = SkinValue;
			if (SkinValue)
			{
				GGPUSkinCache.TransitionAllToWriteable(RHICmdList);
			}
		});
	}
}

FAutoConsoleVariableSink FGPUSkinCache::CVarSink(FConsoleCommandDelegate::CreateStatic(&CVarSinkFunction));

IMPLEMENT_SHADER_TYPE(template<>,TGPUSkinCacheCS<false>,TEXT("GpuSkinCacheComputeShader"),TEXT("SkinCacheUpdateBatchCS"),SF_Compute);
IMPLEMENT_SHADER_TYPE(template<>,TGPUSkinCacheCS<true>,TEXT("GpuSkinCacheComputeShader"),TEXT("SkinCacheUpdateBatchCS"),SF_Compute);
