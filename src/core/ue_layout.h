// Everything the DLL knows about the game binary (IntoTheRadius2-Win64-Shipping.exe, UE 5.5.4 Meta fork).
// Values come from the PDB shipped with the exe (ITR2_DLSS_Registo_RE.md, sections 4 and 5). knownRva is for
// game 1.2.5 (TimeDateStamp 0x8DDD9ADC); the struct offsets and virtual slots are the same in 1.1.2 (0xA486E0C3),
// both checked against their PDBs.
// If the exe changes, re-check with tools/depcheck and update this file (and tools/depcheck/expect.json).
#pragma once
#include <cstdint>

namespace ue {

// Builds whose PDB was checked against everything in this file. knownRva below is for kKnownBuilds[0] only;
// the other builds are found by the AOB scan.
struct KnownBuild { uint32_t timeDateStamp, sizeOfImage; const char* game; };
inline const KnownBuild kKnownBuilds[] = {
    { 0x8DDD9ADC, 0x0A6B1000, "1.2.5 (Steam build 26083075)" },
    { 0xA486E0C3, 0x0A648000, "1.1.2 (Steam build 24024260)" },
};

// ---- Resolver items ------------------------------------------------------------------------
// kind Func: AOB starts at the function, address = match.
// kind Data: AOB starts at an instruction; address = match + EndOff + int32@(match + DispOff).
// kind CallTarget: like Data (rel32 of a call); the AOB may match many sites, but all of them
//                  must resolve to the same target (used for small ICF-shared helpers).
enum class Kind : uint8_t { Func, Data, CallTarget };

struct Item {
    const char* name;
    Kind        kind;
    uint32_t    knownRva;     // RVA of the AOB match in the known exe
    const char* aob;
    uint8_t     dispOff;      // Data only
    uint8_t     endOff;       // Data only
    uint8_t     minMode;      // 1 = needed to register the extension, 2 = needed for the upscaler
};

enum ItemId : int {
    I_GEngine = 0,
    I_RegisterExtension,
    I_GDynamicRHI,
    I_ConsoleSingleton,
    I_CreateTexture,
    I_SetupParameterPass,
    I_RDGPassCtor,
    I_ShaderParamsMetadataCtor,
    I_FlushResourceBarriers,
    I_RDGPtrArrayResizeGrow,
    I_GGPUFrameTime,          // optional (performance log): uint32, FPlatformTime cycles (QPC ticks)
    I_GGameThreadTime,
    I_GRenderThreadTime,
    I_GRHIThreadTime,
    I_HmdRefreshRateExec,     // optional: the headset's refresh rate from the OpenXR runtime (TargetHz=0)
    I_GGameThreadWaitTime,    // optional (miss guard): uint32 cycles the game thread waited for the render thread
    I_XrWaitFrame,            // optional (miss guard): OpenXRDynamicAPI::xrWaitFrame, a function pointer variable
    I_GLastGCFrame,           // optional (hitch log, debug build): uint64 GFrameCounter of the last garbage collection
    I_Count
};
constexpr uint8_t kOptional = 9;   // minMode for items that are never required

inline const Item kItems[I_Count] = {
    // RegisterExtension: 40 53 41 57 48 83 EC 38 | 48 8B 1D <GEngine> | 4C 8B F9
    { "GEngine", Kind::Data, 0x3D62E90,
      "40 53 41 57 48 83 EC 38 48 8B 1D ?? ?? ?? ?? 4C 8B F9", 0x0B, 0x0F, 1 },
    { "FSceneViewExtensions::RegisterExtension", Kind::Func, 0x3D62E90,
      "40 53 41 57 48 83 EC 38 48 8B 1D ?? ?? ?? ?? 4C 8B F9", 0, 0, 1 },
    { "GDynamicRHI", Kind::Data, 0x2C0DE66,
      "48 8B 1D ?? ?? ?? ?? 48 83 C3 08 48 3B D9 74 50 44 8B 43 0C", 0x03, 0x07, 1 },
    // IConsoleManager::SetupSingleton: 48 83 EC 28 | 48 83 3D <Singleton> 00 | ...
    { "IConsoleManager::Singleton", Kind::Data, 0x1046410,
      "48 83 EC 28 48 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 48 89 7C 24 20 B9 ?? ?? 00 00 "
      "48 89 74 24 38 E8 ?? ?? ?? ?? 33 F6 48 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ??",
      0x07, 0x0C, 1 },
    { "FRDGBuilder::CreateTexture", Kind::Func, 0x2165A90,
      "48 89 5C 24 10 55 56 57 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 0F B6 42 32", 0, 0, 2 },
    { "FRDGBuilder::SetupParameterPass", Kind::Func, 0x2C8E5D0,
      "40 56 57 48 83 EC 68 48 8B FA 48 8B F1 E8 ?? ?? ?? ??", 0, 0, 2 },
    { "FRDGPass::FRDGPass", Kind::Func, 0x2C6F770,
      "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 49 8B 00", 0, 0, 2 },
    { "FShaderParametersMetadata::FShaderParametersMetadata", Kind::Func, 0x2CBE4A0,
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 48 8D 05 ?? ?? ?? ?? 4C 89 49 08", 0, 0, 2 },
    { "FD3D12ContextCommon::FlushResourceBarriers", Kind::Func, 0x2AA8860,
      "40 53 48 83 EC 20 83 B9 90 01 00 00 00 48 8B D9 74 31 48 83 B9 98 01 00 00 00", 0, 0, 2 },
    // How FRDGBuilder::AddPass registers a pass (seen in FComputeShaderUtils::AddPass<FCapsuleShadowingCS>):
    //   Passes.Num++ ; if (Num > Max) ResizeGrow(&Passes, OldNum) ; Passes[OldNum] = Pass ; Pass->Handle = Num-1 ; SetupParameterPass
    // knownRva is the first of the 78 sites (0x214429F); target = TArray<T*, TRDGArrayAllocator<0>>::ResizeGrow (RVA 0x2184920,
    // ICF-shared with the FRDGBuffer* one).
    { "TArray<FRDGPass*,TRDGArrayAllocator>::ResizeGrow", Kind::CallTarget, 0x214429F,
      "48 63 73 08 8D 4E 01 89 4B 08 3B 4B 0C 76 0A 8B D6 48 8B CB E8 ?? ?? ?? ?? 48 8B 03 48 8B D7 48 8B CD 48 89 3C F0 8B 43 08 FF C8 89 47 2C E8",
      0x15, 0x19, 2 },
    // engine timing globals (RVA 0x9B8BC58 / 0x9B9CAAC / 0x9B9CAA0 / 0x9B9CAA8), found through instructions that write them
    { "GGPUFrameTime", Kind::Data, 0x2AF88FB,
      "89 05 ?? ?? ?? ?? 49 8B 5B 38 49 8B 6B 40 49 8B 73 48", 0x02, 0x06, kOptional },
    // FViewport::Draw: cmp/cmovbe, then mov [GGameThreadTime],eax ; mov eax,[r11+10h] ; mov [GGameThreadWaitTime],eax
    { "GGameThreadTime", Kind::Data, 0x3EF9A67,
      "45 3B 43 10 41 0F 46 C0 89 05 ?? ?? ?? ?? 41 8B 43 10 89 05 ?? ?? ?? ??", 0x0A, 0x0E, kOptional },
    { "GRenderThreadTime", Kind::Data, 0x2D88FB8,
      "89 05 ?? ?? ?? ?? 8B C5 45 2B 4A 14 41 2B C1 41 3B E9", 0x02, 0x06, kOptional },
    // xchg eax,[GRHIThreadTime]
    { "GRHIThreadTime", Kind::Data, 0x2D6D856,
      "87 05 ?? ?? ?? ?? 48 89 53 10 C7 43 18 01 00 00 00 48 83 C4 30", 0x02, 0x06, kOptional },
    // UOpenXRBlueprintFunctionLibrary::execGetHMDDisplayRefreshRate(UObject*, FFrame&, void* Result): GetOpenXRHMD(),
    // then if the runtime has XR_FB_display_refresh_rate (hmd+0x352): xrGetDisplayRefreshRateFB(session hmd+0x3A0) -> float,
    // else 0. Its only use of the FFrame is Stack.Code += (Code != 0) at +0x20, so a zeroed dummy frame is enough.
    { "UOpenXRBlueprintFunctionLibrary::execGetHMDDisplayRefreshRate", Kind::Func, 0x42B3FE0,
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 42 20 33 C9 48 85 C0 49 8B F8 0F 95 C1 48 03 C8 48 89 4A 20 E8 ?? ?? ?? ?? "
      "48 8B D8 48 85 C0 74 ?? 80 B8 52 03 00 00 00", 0, 0, kOptional },
    // the second store of GGameThreadTime's AOB: mov [GGameThreadWaitTime],eax (RVA 0x9B9CAB0)
    { "GGameThreadWaitTime", Kind::Data, 0x3EF9A67,
      "45 3B 43 10 41 0F 46 C0 89 05 ?? ?? ?? ?? 41 8B 43 10 89 05 ?? ?? ?? ??", 0x14, 0x18, kOptional },
    // FOpenXRHMD::OnBeginSimulation_GameThread+0x96: call [OpenXRDynamicAPI::xrWaitFrame] (RVA 0x9C144A8; its only
    // caller, on the game thread; InitOpenXRCore fills the variable through xrGetInstanceProcAddr at start-up)
    { "OpenXRDynamicAPI::xrWaitFrame", Kind::Data, 0x42AAF9E,
      "FF 15 ?? ?? ?? ?? 0F 10 44 24 40 8B 05 ?? ?? ?? ?? 0F 10 4C 24 50", 0x02, 0x06, kOptional },
    // UE::GC::FReachabilityAnalysisState::PerformReachabilityAnalysisAndConditionallyPurgeGarbage (two inlined copies):
    // mov rax,[GFrameCounter] ; mov [GLastGCFrame],rax ; mov byte [GIsGarbageCollecting],1 ; test cl,cl ; jne ; call
    // (RVA 0x99873C0; both sites must agree)
    { "GLastGCFrame", Kind::CallTarget, 0x12CD4F4,
      "48 8B 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 84 C9 75 0D E8", 0x0A, 0x0E, kOptional },
};

// ---- Struct offsets (PDB types of this exe) ------------------------------------------------
namespace off {
// UEngine
constexpr uint32_t Engine_ViewExtensions = 0xFA8;      // TSharedPtr<FSceneViewExtensions>
// FSceneViewFamily
constexpr uint32_t Family_ViewsData = 0x08;            // TArray<const FSceneView*>
constexpr uint32_t Family_ViewsNum  = 0x10;
constexpr uint32_t Family_bIsMainViewFamily = 0xD2;
constexpr uint32_t Family_TemporalUpscaler  = 0x190;
// FSceneView
constexpr uint32_t View_Family            = 0x008;
constexpr uint32_t View_ViewMatrices      = 0x370;
constexpr uint32_t View_StereoPass        = 0x1610;
constexpr uint32_t View_StereoViewIndex   = 0x1614;
constexpr uint32_t View_bCameraCut        = 0x1C10;
constexpr uint32_t View_bIsSceneCapture   = 0x1C28;
constexpr uint32_t View_bIsReflectionCapture = 0x1C2B;
constexpr uint32_t View_bIsPlanarReflection  = 0x1C2C;
constexpr uint32_t View_bIsInstancedStereo   = 0x1C32;
constexpr uint32_t View_AntiAliasingMethod   = 0x2690;
constexpr uint32_t View_PrimaryScreenPercentageMethod = 0x2694;
// FViewInfo (derives from FSceneView)
constexpr uint32_t ViewInfo_ViewRect       = 0x2730;
constexpr uint32_t ViewInfo_StateBits      = 0xD2B8;   // bit3 = bPrevTransformsReset
constexpr uint32_t ViewInfo_PrevViewMatrices = 0xD2D0 + 0x10;
constexpr uint32_t ViewInfo_PreExposure    = 0xE034;
// FViewMatrices (doubles)
constexpr uint32_t VM_ProjectionNoAA = 0x080;
constexpr uint32_t VM_ViewMatrix     = 0x180;
constexpr uint32_t VM_InvViewMatrix  = 0x200;
constexpr uint32_t VM_ViewOrigin     = 0x818;
constexpr uint32_t VM_TemporalAAProjectionJitter = 0x858;
// ITemporalUpscaler::FInputs
constexpr uint32_t In_OutputViewRect = 0x00;
constexpr uint32_t In_Jitter         = 0x10;   // float2, render pixels
constexpr uint32_t In_PreExposure    = 0x18;
constexpr uint32_t In_SceneColor     = 0x20;   // FScreenPassTexture {FRDGTexture*, IntRect}
constexpr uint32_t In_SceneDepth     = 0x38;
constexpr uint32_t In_SceneVelocity  = 0x50;
constexpr uint32_t In_EyeAdaptation  = 0x68;
constexpr uint32_t In_PrevHistory    = 0x70;
// ITemporalUpscaler::FOutputs
constexpr uint32_t Out_FullResTexture = 0x00;
constexpr uint32_t Out_FullResRect    = 0x08;
constexpr uint32_t Out_NewHistory     = 0x18;
constexpr uint32_t Out_Size           = 0x20;
// FRDGTexture / FRDGResource
constexpr uint32_t RDGRes_ResourceRHI = 0x10;
constexpr uint32_t RDGTex_Desc        = 0x60;   // FRHITextureDesc, 0x38 bytes
constexpr uint32_t RDGTex_WholeRange  = 0xAC;   // FRDGTextureSubresourceRange, 8 bytes
// FRHITextureDesc
constexpr uint32_t Desc_Flags      = 0x00;      // uint64 ETextureCreateFlags
constexpr uint32_t Desc_Extent     = 0x24;
constexpr uint32_t Desc_Depth      = 0x2C;
constexpr uint32_t Desc_ArraySize  = 0x2E;
constexpr uint32_t Desc_NumMips    = 0x30;
constexpr uint32_t Desc_NumSamples = 0x31;
constexpr uint32_t Desc_Dimension  = 0x32;
constexpr uint32_t Desc_Format     = 0x33;
constexpr uint32_t Desc_UAVFormat  = 0x34;
constexpr uint32_t Desc_Size       = 0x38;
// FRHICommandListBase
constexpr uint32_t CL_CommandLink    = 0x30;
constexpr uint32_t CL_ComputeContext = 0x40;
constexpr uint32_t CL_NumCommands    = 0x60;
constexpr uint32_t CL_bExecuting     = 0x64;
// FRDGBuilder
constexpr uint32_t Builder_Passes = 0x180;      // TRDGHandleRegistry<FRDGPass> = TArray<FRDGPass*> {data, num, max}
constexpr uint32_t Builder_Textures = 0x190;    // TRDGHandleRegistry<FRDGTexture> = TArray<FRDGTexture*> (PDB, FRDGBuilder size 0x680)
// FRDGPass / FShaderParametersMetadata
constexpr uint32_t RDGPass_Size  = 0x1B8;
constexpr uint32_t RDGPass_Handle = 0x2C;       // set to the index in Builder.Passes before SetupParameterPass
constexpr uint32_t Meta_Size     = 0x80;
constexpr uint32_t Meta_Layout   = 0x48;
constexpr uint32_t RHIResource_RefCount = 0x08;   // FRHIResource::AtomicFlags (lock inc/xadd)
// FD3D12CommandContext state-cache flags. UE's own native-compute path (RVA 0x2ACDC47 in 1.2.5, 0x2AC5257 in 1.1.2) does:
//   if (ctx[0x38A0] != 2) { *(u16*)(ctx+0x3610) = 0x101; ctx[0xB38] = 1; ctx[0x38A0] = 2; }
// RHIFinishExternalComputeWork only sets ctx[0xB38] = 1. The mod does the whole sequence so UE
// re-binds its PSO and root signatures after our compute pass and NGX.
constexpr uint32_t D3D12Ctx_NeedSetPSOFlags   = 0x3610;   // u16 = 0x101
constexpr uint32_t D3D12Ctx_NeedSetComputeRS  = 0x0B38;   // u8  = 1
constexpr uint32_t D3D12Ctx_LastPipelineType  = 0x38A0;   // u8  = 2
} // namespace off

// ---- Virtual slots ------------------------------------------------------------------------
namespace slot {
constexpr int Engine_Tick = 97;                      // UGameEngine::Tick(float, bool)
constexpr int DynRHI_GetName = 4;
constexpr int D3D12RHI_GetCommandQueue = 163;        // ID3D12CommandQueue* () const
constexpr int D3D12RHI_GetDevice = 164;              // ID3D12Device* (uint32 index)
constexpr int D3D12RHI_GetGraphicsCommandList = 166; // (FRHICommandListBase&, uint32)
constexpr int D3D12RHI_FinishExternalComputeWork = 180; // (FRHICommandListBase&, uint32, ID3D12GraphicsCommandList*)
constexpr int RHITexture_GetNativeResource = 4;
constexpr int ComputeContext_GetSingleDeviceContext_Off = 0x278; // byte offset, as used by RHIGetGraphicsCommandList
constexpr int ConsoleManager_FindConsoleVariable = 23;
constexpr int ConsoleVariable_Set = 19;
constexpr int ConsoleVariable_GetInt = 22;
constexpr int ConsoleVariable_GetFloat = 23;
constexpr int SceneViewExtension_Count = 30;
} // namespace slot

// ---- Enum values ----------------------------------------------------------------------------
constexpr uint32_t ECVF_SetByCode    = 0x0D000000;
constexpr uint32_t ECVF_SetByConsole = 0x0E000000;
constexpr uint64_t TexCreate_ShaderResource = 8;
constexpr uint64_t TexCreate_UAV = 0x20000;
constexpr uint8_t  PF_FloatRGBA = 10;
constexpr uint32_t ERHIAccess_SRVCompute = 16;
constexpr uint32_t ERHIAccess_CopySrc    = 128;
constexpr uint32_t ERHIAccess_UAVCompute = 1024;
constexpr uint32_t ERHIAccess_CopyDest   = 8192;
constexpr uint16_t ERDGPassFlags_Compute = 2;
constexpr uint16_t ERDGPassFlags_Copy = 8;
constexpr uint16_t ERDGPassFlags_NeverCull = 16;
constexpr uint16_t ERDGPassFlags_NeverParallel = 128;
constexpr uint8_t  UBMT_RDG_TEXTURE_ACCESS = 10;

struct IntRect { int32_t x0, y0, x1, y1; int w() const { return x1 - x0; } int h() const { return y1 - y0; } };

} // namespace ue
