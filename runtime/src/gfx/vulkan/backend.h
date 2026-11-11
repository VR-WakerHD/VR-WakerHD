#pragma once
#include "vertex_format_policy.h"
#include "loader.h"
#include "barrier_state.h"
#include "per_thread.h"
#include "../../platform/quest_theater.h"
#include "mobile_gpu.h"
#ifdef WWHD_SDL_HOST
#include <SDL3/SDL.h>
#endif
#include <atomic>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <cstdint>
#include <memory>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "formats.h"
#include "api.h"
#include "buffer_cache_core.h"
#include "../target_size.h"
#include "../texture_pack_core.h"
namespace gfxvk {
class DeferredCommands;
bool begin_exact_preparation();
void drain_pending_guest_pipelines();
struct Buffer { ResourceUse use; VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; void* mapped=nullptr; VkDeviceSize size=0;
                VkMemoryPropertyFlags properties=0; /* of the memory type create_buffer chose */ };
struct UploadSlice { VkBuffer buffer=VK_NULL_HANDLE; VkDeviceSize offset=0,size=0; void* mapped=nullptr; };
struct CachedGuestLayout;
struct Surface {
 ResourceUse use;
    std::shared_ptr<Surface> mipChain; // sampled companion assembled from GPU-rendered levels
    uint64_t mipChainKey[16] = {}; // per level of mipChain: what it was built from (with_mip_chain)
    uint32_t mipChainLevels = 0;   // ... and how many of its levels are valid
 VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE;
 VkDeviceSize allocationBytes=0; // tracked image allocation, excludes retired/driver memory
 VkImageType imageType=VK_IMAGE_TYPE_2D; VkImageViewType viewType=VK_IMAGE_VIEW_TYPE_2D;
 VkExtent3D extent{}; VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED; VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT; VkImageUsageFlags usage=0; VkImageCreateFlags createFlags=0;
 uint32_t arrayLayers=1; std::vector<VkImageView> layerViews;
 // (docs/VR_MULTIVIEW.md, stage M2) the first of its image's layers that are this surface's: 0, or, for eye 1's half of
 // a two-eye render target, eye 0's layer count (its layers follow eye 0's in the same image). Every subresource range,
 // view and copy of the surface starts at it; arrayLayers counts the surface's own layers
 uint32_t baseLayer=0;
 // (stage M2b, WWHD_VK_MULTIVIEW=1) the eye whose surface map holds it; its image's layer count (eye 0's two-eye render
 // targets: twice arrayLayers); and the link between eye 0's render target (layerTwin: eye 1's surface in its second
 // layers) and eye 1's (layerOwner: the surface whose image it shares, which owns it). Before the owner's image is
 // retired, eye 1's half moves to an image of its own, its pixels copied (detach_eye1_layers)
 uint8_t eye=255;  // (255: in neither map: a scan, a private image, a temporary)
 uint32_t imageLayers=0;
 Surface* layerOwner=nullptr; Surface* layerTwin=nullptr;
// (2026-11-06) the render frame a draw last missed multiview only because this eye 0 target has one eye's image (made
// before stereo, or first as a texture); rescale() makes it two-eye at its next use in a later frame (0: never)
 uint64_t wantTwoEyeFrame=0;
 // (multiview diagnostics, 2026-11-07: counted per draw with one relaxed atomic add, formatted only every 10 s) draws of
 // eye 0 that missed multiview because of this target (mvMissWhat: 1..3 colour, 4..6 depth: no eye 1 twin, one eye's
 // image, layered); and (mobile GPU paths) the draws the per-eye hold refused for it, and what the draw that last held it
 // per eye was (perEyeWhy: its MultiviewWhy, perEyeKind: its pixel shader's gfx::ProgramKind)
 uint32_t mvMissDraws=0; uint8_t mvMissWhat=0;
 uint32_t mvHeldDraws=0; uint8_t perEyeWhy=255, perEyeKind=0;
#ifdef WWHD_MOBILE_GPU_PATHS
 // (QUEST3_IMPLEMENTATION_ROADMAP.md, mixed passes) the last render frame a draw wrote this eye 0 target's two-eye image
 // one eye at a time (not multiview); 0: never. draw.cpp multiview_draw keeps such images per eye for that frame and the
 // next, so a tiled GPU never sees one written both ways in a frame
 uint64_t perEyeFrame=0;
 // (2026-11-07) the frames it was marked so, last 8 (bit 0: perEyeFrame's): the long hold only for an image marked in
 // most of them (draw.cpp written_per_eye)
 uint8_t perEyeHistory=0;
 // (2026-11-08) on a two-eye image's eye 0 surface: the last render frames a pass wrote it for both eyes (multiview) and
 // one layer at a time; draw.cpp puts a global memory barrier before a pass that switches between the two
 uint64_t mvPassFrame=0, layerPassFrame=0;
#endif
 // a copy made into a surface of its own image (a private image made like a target): none of the two-eye link
 void own_image_only() { baseLayer=0; imageLayers=0; eye=255; layerOwner=layerTwin=nullptr; }
 std::unordered_map<uint32_t,VkImageView> sampledViews;
 uint32_t addr=0,mipAddr=0,width=0,height=0,slices=1,pitch=0,mips=1,format=0,dim=1,tileMode=0,swizzle=0;
 bool isDepth=false,gpuWritten=false,dirty=true,bcDecoded=false;
 gfx::SizeClaim sizeClaim; // a size other than the internal resolution, claimed by a pass (target_scale, gfx/target_size.h)
 bool linearFiltering=false; // device capability for the actual optimal-tiling image format
 uint64_t writeSeq=0,contentHash=0,lastCheckedFrame=~0ull,sparseHash=0;
 // (docs/VR_SCENE_THREADS.md, "the drawing threads' locks") the frame of upload_surface's last finished check
 // (lastCheckedFrame is set as it starts): with both, the check needs no surface lock
 uint64_t uploadCheckedFrame=~0ull;
 bool formatViews=false; // another surface at this address has the same texel bits in another format (adopt_newer_alias)
 uint64_t writtenBackSeq=0; // writeSeq when last written back to guest memory (linear surfaces, write_back_linear_targets)
 // CPU textures: write stamp (write_watch.h) of all levels' pages at the last full check
 uint64_t watchStamp=0; bool watched=false;
 // stereo: eye 1's held commands of this flush generation sample this eye 0 texture (stereo.h)
 uint64_t eyeHeld=0;
 uint64_t eyeHeldB=0;  // (docs/VR_SCENE_THREADS.md, stage S6) the same, by the second drawing thread's eye 1 halves
 // the overlap that last used it on the atlas's thread, and that last changed it on the render thread
 // (atlas_overlap.h, stage 2.6b)
 uint64_t atlasUse=0, sceneChange=0;
 uint64_t bPreRead=0;  // (docs/VR_SCENE_THREADS.md, "S6: B's reads deferred") the span whose start it is made readable at
 uint64_t frontUse=0, frontRenderUse=0, frontRenderChange=0;  // (docs/VR_SCENE_THREADS.md, S7) the overlap's marks
 const char* sceneChangeWhat=nullptr;  // (diagnostics) the render thread's last change of it in an overlap
 // (docs/VR_EYE_THREAD.md, stage E5) the eye 0 draw (its eye 1 draw's number) during which the render thread last
 // changed it while eye 1's thread ran: that thread must not use it for an earlier draw
 uint64_t eye0ChangeSeq=0;
 // texture budget (surfaces.cpp texture_budget_step): the frame the texture was last looked up, and whether its image
 // was released to stay within the budget (made again, and uploaded from guest memory, at its next lookup)
 uint64_t lastUsedFrame=0; bool evicted=false;
 bool artisticMips=false; // hand-drawn mip levels (gfx/artistic_mips.h): resolution-aware bias, no forced anisotropy
 // replacement texture packs (texture_pack_vk.cpp, gfx/texture_pack.h): the XXH3 of level 0's guest bytes at the last
 // upload (one of upload_surface's hash terms); the larger image made from a pack's DDS for the key it then had
 // (replacementKey), owned as mipChain is and never in the surface maps (on the replacement itself: lodOffset, the
 // levels it has in front of the guest's); the frame a draw last took it (replacement_for, for the pack's budget); and
 // whether the budget released it (the next replacement_for asks for it again: an unchanged texture is not uploaded)
 uint64_t level0Hash=0; std::shared_ptr<Surface> replacement; gfx::texture_pack::Key replacementKey{};
 uint32_t lodOffset=0; uint64_t replacementUsedFrame=0; bool replacementReleased=false;
 // a copy of a surface (a private image made like it) is no guest texture: it holds no replacement
 void forget_replacement() { replacement.reset(); replacementKey={}; replacementReleased=false; replacementUsedFrame=0; }
 uint32_t dataSize=0; FormatInfo fmt;
 mutable std::shared_ptr<CachedGuestLayout> guestLayout;
 float scale=1,sx=1,sy=1;
 float ax=1,ay=1; // aspect-ratio factors of screen-shaped targets (surfaces.cpp)
 // dynamic resolution (dynres.h): the factor of the internal resolution this image was made at (1: off), and
 // images of other recent scales kept for the next change (only their image fields are used; surfaces.cpp)
 float dyn=1;
 std::vector<std::shared_ptr<Surface>> spares;
 // (docs/VR_DLSS.md, "Background motion") the value its depth was last cleared to (NaN: never; any thread records it
 // in order with its draws, read with std::atomic_ref): the motion pass's background draws are pinned to it
 float clearedDepth=__builtin_nanf("");
};
struct SurfaceDesc { uint32_t addr=0,mipAddr=0,width=0,height=0,slices=1,pitch=0,mips=1,format=0,dim=1,tileMode=0,swizzle=0; bool isDepth=false;
                     gfx::SizeRequest sizeRequest; /* from the drawing pass (Surface::sizeClaim) */ };
struct Screen {
#ifdef WWHD_SDL_HOST
 std::atomic<SDL_Window*> window{nullptr};
#else
 void* window=nullptr; // CAMetalLayer of the AppKit view (display.mm)
#endif
 VkSurfaceKHR surface=VK_NULL_HANDLE; VkSwapchainKHR swapchain=VK_NULL_HANDLE;
 VkFormat swapFormat=VK_FORMAT_UNDEFINED; VkExtent2D swapExtent{};
 bool hdrOutput=false;
 bool hdrPq=false; // hdrOutput as HDR10 PQ: composed in FP16 scRGB, then encoded (present.cpp)
 bool hdrCompositionReady=false; // defer metadata until the first actual composition
 bool hdrScanBounded=false; // all guest image quads in the last composition use normalized formats
 // VK_EXT_hdr_metadata: a new swapchain has none; queue_present sends it, and again when the sliders change
 std::atomic<bool> hdrMetadataStale{true};
 std::array<float,4> hdrMetadataSent{};
 std::vector<VkImage> images; std::vector<VkImageLayout> layouts;
 std::vector<ResourceUse> imageUses;
 VkSemaphore acquired=VK_NULL_HANDLE,finished=VK_NULL_HANDLE;
 std::unique_ptr<Surface> scan;
 // Never overwrite the published scan while the next guest frame is being
 // built. Publish at swap; reuse the former published allocation next time.
 std::unique_ptr<Surface> buildingScan;
 // (docs/VR_DLSS.md, stage D4) the guest colour buffer the building scan was resampled from (its internal size): DLSS
 // reads it at the frame's end and writes the scan at its full size
 Surface* scanSource=nullptr;
 // dynamic resolution: scan images of other sizes, kept for the next change (copy_to_scan)
 std::vector<std::unique_ptr<Surface>> scanSpares;
 bool buildingScanDirty=false,buildingScanSrgb=false,scanSrgb=false;
 // Display shape is separate from storage: Half SBS/TAB scans keep eye-sized
 // pixels until final composition, avoiding an upsample followed by a squeeze.
 VkExtent2D scanDisplayExtent{},buildingScanDisplayExtent{};
 std::atomic<bool> visible{true},srgb{false},resize{false};
 int presentMode=-1,presentWanted=-1; // present mode of the swapchain, and the setting it was made for
 std::atomic<int> width{1280},height{720};
 uint64_t swapchainGeneration=0; // renderer-thread lifecycle diagnostics
 std::atomic<uint64_t> presented{0}; // successful queue presentations, for display diagnostics
};
inline VkExtent3D scan_display_extent(const Screen& screen) {
 if(!screen.scan)return {};
 return screen.scanDisplayExtent.width && screen.scanDisplayExtent.height
     ? VkExtent3D{screen.scanDisplayExtent.width,screen.scanDisplayExtent.height,1}
     : screen.scan->extent;
}
struct GpuScopeMetadata {
 uint32_t kind=0; // 0 render pass, 1 feedback copy.
 std::array<VkFormat,8> colors{};
 VkFormat depth=VK_FORMAT_UNDEFINED;
 VkExtent3D extent{};
 uint32_t mips=0,layers=0,aspects=0,colorCount=0;
 std::array<uint32_t,8> slices{};
 uint32_t depthSlice=0;
 bool depthOnly=false;
};
struct GpuScopeToken { uint64_t generation=0; uint32_t index=UINT32_MAX; };
inline constexpr std::array<const char*,9> secondaryStageNames{
 "acquire", "acquire_wait", "ready_fd", "record", "submit", "draw_wait", "returned_fd", "present", "primary_snapshot_record"};
// The draw path's statistics, counted per draw. Each thread that prepares draws counts into its own (draw_counters:
// the renderer's own, R.drawCounters, on the render thread), and a second thread's are added into the renderer's when
// it joins (docs/VR_ATLAS_THREAD.md, stage 2.2): counting per draw stays a plain increment, no atomic
struct DrawCounters {
 uint64_t vertexBindCalls=0,vertexBindSkips=0;
 uint64_t descriptorLookups=0,descriptorCacheHits=0,descriptorAllocations=0,descriptorFastHits=0;
 struct CpuPreparationStats {
  uint64_t packCalls=0,packBytes=0,packCapacityGrowths=0,packCapacityGrowthBytes=0;
  uint64_t uniformSnapshotCalls=0,uniformSnapshotBytes=0;
  uint64_t samplerRequests=0,samplerMemoHits=0,samplerMapLookups=0;
  uint64_t uniformReuseChecks=0,uniformReuseComparisons=0,uniformReuseHits=0,uniformReuseBytes=0;
  uint64_t descriptorBindCandidates=0,descriptorWholeMatches=0;
  uint64_t descriptorBindCalls=0,descriptorSkippedSets=0;
  uint64_t pipelineLookups=0,pipelineLastHits=0,pipelineLookasideHits=0,pipelineMapLookups=0;
  std::array<uint64_t,2> descriptorStageMatches{};
 } cpuPreparation;
 uint64_t pipelineCreates=0,pipelineCreateNs=0;
 uint64_t renderPassCount=0,drawCount=0;
 uint64_t vertexHistoryReuseChecks=0,vertexHistoryReuseHits=0,vertexHistoryReuseBytes=0;
 uint64_t vertexHistoryRequests=0,vertexHistoryMatches=0,vertexHistoryBytes=0;
 std::array<uint64_t,7> vertexHistoryDistances{};
 uint64_t vertexDeclaredBytes=0,vertexCopiedBytes=0;
 uint64_t vertexReuseChecks=0,vertexReuseHits=0,vertexReuseBytes=0,vertexReuseCompareNs=0;
 uint64_t uploadAllocations=0,uploadBytes=0;  // upload blocks created, bytes allocated from the arenas
 void add(const DrawCounters& o);  // every count of o into this one (backend.cpp)
};
// An open render pass: whether one is open, its attachments and size (docs/VR_ATLAS_THREAD.md, stage 2.6)
struct PassState {
 bool rendering=false, tracked=false;
 std::array<Surface*,8> colors{};
 std::array<uint32_t,8> slices{};
 Surface* depth=nullptr;
 uint32_t depthSlice=0, width=0, height=0;
 bool multiview=false;  // (docs/VR_MULTIVIEW.md, stage M4) view mask 0b11: the targets' both eyes' layers
};
struct Renderer {
 vk::VertexFormatPolicy vertexFormats;
 vk::VertexDiagnostics vertexDiagnostics; // issue #207 driver troubleshooting (vertex_format_policy.h)
 VkInstance instance=VK_NULL_HANDLE; VkPhysicalDevice physicalDevice=VK_NULL_HANDLE; VkDevice device=VK_NULL_HANDLE;
 std::unordered_map<VkFormat,VkFormatProperties> formatProperties;
 VkPhysicalDeviceFeatures enabledFeatures{};
 bool computeQueue=false;
 bool bcCpuDecode=false; // BC uploads without BC textures: CPU decoder instead of compute (bc_decoder_choice.h)
 bool depthClampAll=false; // depth clamp for every pipeline: panvk culls per fragment for depth clipping (draw.cpp)
 std::atomic<bool> secondaryPresentHeld{false}; // game-free blocked-present diagnostic
 std::atomic<bool> secondaryInjectAcquireLoss{false},secondaryInjectPresentLoss{false}; // authored WSI diagnostics
 std::atomic<bool> secondaryInjectQueryLoss{false};
 std::atomic<uint64_t> secondarySurfaceLosses{0};
 std::atomic<bool> secondaryAcquireWaiting{false}; // worker acknowledged the acquisition hold
 std::atomic<uint64_t> primarySurfaceLosses{0};
 std::atomic<bool> primaryInjectQueryLoss{false}; // game-free primary recovery diagnostic
 std::atomic<bool> secondaryAcquireHeld{false}; // game-free blocked-acquisition diagnostic
 uint32_t primaryRequestedQueues=1;
 std::atomic<uint32_t> secondaryDeviceSwapchains{0},secondarySharedSnapshots{0};
 std::atomic<uint64_t> secondaryLocalQueueWaits{0};
 // Opt-in diagnostic CPU wall times, including driver waits; not GPU busy times.
 std::atomic<bool> secondaryProfile{false};
 std::array<std::atomic<uint64_t>,secondaryStageNames.size()> secondaryStageNs{},secondaryStageCalls{};
 bool secondaryIsolated=false; // separate logical-device WSI worker
 bool secondaryAcquirePending=false;
 bool secondaryPresentPending=false; // renderer-thread mailbox ownership
 std::atomic<uint64_t> secondaryPresentSkipped{0};
 VkQueue secondaryQueue=VK_NULL_HANDLE;
 uint32_t secondaryQueueFamily=0;
 bool secondaryRetirementHeld=false; // game-free diagnostic loop only
 bool secondaryPresentFences=false; // Android: optional swapchain-maintenance presentation retirement
 std::atomic<uint64_t> secondaryIdleWaits{0},secondaryRetired{0},secondaryRetirementPending{0};
 bool dynamicRenderingKHR=false; // VK_KHR_dynamic_rendering (device older than Vulkan 1.3)
 // multiview (core Vulkan 1.1; docs/VR_MULTIVIEW.md): supported, and enabled (only with WWHD_VK_MULTIVIEW=1)
 bool multiviewSupported=false, multiview=false;
 VkDriverId driverID=VkDriverId(0);  // (VkPhysicalDeviceDriverProperties; 0: unknown) e.g. Mesa Turnip vs Qualcomm's
 bool multiviewIndexing=false;  // (stage M4) the arrays a multiview shader indexes by gl_ViewIndex can be
 uint32_t maxMultiviewViews=0;
 bool renderPasses=false;
 const char* renderingReason="not initialized";
 bool portabilitySubset=false,imageViewSwizzle=true,imageViewReinterpretation=true;
 PFN_vkSetHdrMetadataEXT setHdrMetadata=nullptr; // VK_EXT_hdr_metadata, when the device offers it
 bool attachmentViewUsage=false; // attachment views name their usage (issue #144: Intel; WWHD_VK_ATTACHMENT_VIEW_USAGE)
 bool imageView2DOn3DImage=true; // 2D views of volume slices (render targets); core Vulkan 1.1, optional in the portability subset
 bool samplerMipLodBias=true,separateStencilMaskRef=true,constantAlphaColorBlendFactors=true,vertexAttributeAccessBeyondStride=true,samplerMirrorClampToEdge=false;
 VkPhysicalDeviceProperties properties{}; VkQueue queue=VK_NULL_HANDLE; uint32_t queueFamily=0;
 bool gpuTimestampsEnabled=false,gpuPassTimestampsEnabled=false;
 uint32_t gpuTimestampValidBits=0;
 struct GpuTimestampStats {
  double intervalNs=0,maxIntervalNs=0;
  uint64_t submissions=0,unavailable=0,zeroIntervals=0;
 } gpuTimestampStats, gpuTimestampLifetimeStats;
 VkPipelineCache pipelineCache=VK_NULL_HANDLE;
 bool pipelineCacheDirty=false;
 uint64_t pipelineCacheChangedFrame=0;
 VkCommandPool commandPool=VK_NULL_HANDLE; VkCommandBuffer cmd=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
 VkDescriptorPool descriptorPool=VK_NULL_HANDLE; bool recording=false;
 // the second descriptor pool (docs/VR_ATLAS_THREAD.md, stage 2.5): the sun atlas's held stream allocates its sets
 // from it, so a thread of its own can while the render thread allocates from descriptorPool (a pool is used by one
 // thread at a time). The active slot's; made only while WWHD_VK_ATLAS_STREAM=1, else null
 VkDescriptorPool atlasDescriptorPool=VK_NULL_HANDLE;
 // eye 1's thread's (docs/VR_EYE_THREAD.md, stage E4): made only while WWHD_VK_EYE_THREAD=1, else null
 VkDescriptorPool eyeDescriptorPool=VK_NULL_HANDLE;
 // the second drawing thread's (docs/VR_SCENE_THREADS.md, stage S6): made only while WWHD_VK_SCENE_TWO=1, else null
 VkDescriptorPool scene2DescriptorPool=VK_NULL_HANDLE;
 // Descriptor reuse must include this epoch: pool handles repeat after reset.
 uint64_t submissionGeneration=0;
 // the draw path's statistics (DrawCounters): counted through draw_counters()
 DrawCounters drawCounters;
 // the render thread's current render pass (pass_state(): another thread's own while it has one). Stereo swaps it
 // with the other eye's and the atlas's held stream's (stereo.cpp swap_pass)
 PassState pass;
 uint32_t mainDepthAddr=0;  // eye 0's (selected_main_depth: the selected eye's)
 uint64_t frame=0; std::atomic<uint64_t> completed{0};
 struct UploadBlock { Buffer buffer; VkDeviceSize used=0; };
 std::vector<UploadBlock> uploadBlocks;
 // The second upload arena (docs/VR_ATLAS_THREAD.md, stage 2.3): the sun atlas's held stream allocates from it, so a
 // thread of its own can, while the render thread allocates from uploadBlocks. Same lifecycle: moved into the
 // submission's slot at submit, reset when its fence completes, handed back when the slot is reused
 std::vector<UploadBlock> atlasUploadBlocks;
 std::vector<UploadBlock> eyeUploadBlocks;  // eye 1's thread's (stage E4), the same lifecycle
 std::vector<UploadBlock> scene2UploadBlocks;  // the second drawing thread's (VR_SCENE_THREADS.md, S6), the same
 // pipelines the game asked for that were not made yet this run, and how many of them the recipe database knew
 uint64_t pipelineKeysNew=0,pipelineKeysKnown=0;
 // texture staging taken from the arena in the recording submission (reset when a submission is activated)
 VkDeviceSize textureStagingBytes=0;
 // Upload arena memory is HOST_CACHED|HOST_COHERENT (every block so far): CPU reads of it are as fast as
 // heap reads (Apple silicon/MoltenVK, many UMA drivers). uploadReadsDirect: the snapshot reuse caches
 // and the native index scan may read mapped upload slices instead of keeping CPU copies (auto: when
 // uploadCached; WWHD_VK_UPLOAD_READS=shadow|direct forces a mode). Set by allocate_upload.
 bool uploadCached=false,uploadReadsDirect=false;
 std::vector<std::function<void()>> completions;
 std::vector<Buffer> garbageBuffers;
 std::vector<VkFramebuffer> garbageFramebuffers;
 std::vector<bufcache::Region> garbageCacheRegions; // buffer cache regions replaced while recording
 struct RetiredImage { VkImage image;VkDeviceMemory memory;std::vector<VkImageView> views; }; std::vector<RetiredImage> garbageImages;
 // Each submission retains its pools, upload bytes and deferred objects until
 // its fence completes. The fields above alias the active recording slot.
 struct Submission {
  VkCommandPool commandPool=VK_NULL_HANDLE;
  VkCommandBuffer cmd=VK_NULL_HANDLE;
  VkFence fence=VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool=VK_NULL_HANDLE, atlasDescriptorPool=VK_NULL_HANDLE, eyeDescriptorPool=VK_NULL_HANDLE,
                   scene2DescriptorPool=VK_NULL_HANDLE;
  VkQueryPool timestampQueries=VK_NULL_HANDLE;
  bool timestampRecorded=false;
  bool dynresTiming=false; // the two timestamps were recorded for dynamic resolution (dynres.h)
  uint64_t timestampFrame=0;
  uint32_t perfQuery=UINT32_MAX;
  static constexpr uint32_t maxGpuScopes=256;
  struct GpuScope { GpuScopeMetadata metadata{}; uint64_t draws=0,generation=0; bool ended=false; };
  std::array<GpuScope,maxGpuScopes> gpuScopes{};
  uint32_t gpuScopeCount=0,activeRenderScope=UINT32_MAX;
  bool pending=false;
  bool retainedUnsubmitted=false;
  DeferredCommands* deferredOwner=nullptr;
  uint64_t serial=0; // Submission order on the single graphics queue.
  uint64_t recordedFrame=0; // R.frame when sealed: frames in flight (WWHD_VK_STATS)
  uint64_t recordWorkerSerial=0; // CPU acknowledgment before ANY fence query or pool reuse.
  std::vector<UploadBlock> uploadBlocks, atlasUploadBlocks, eyeUploadBlocks, scene2UploadBlocks;
  std::vector<std::function<void()>> completions;
  std::vector<Buffer> garbageBuffers;
  std::vector<VkFramebuffer> garbageFramebuffers;
  std::vector<RetiredImage> garbageImages;
  std::vector<bufcache::Region> garbageCacheRegions;
 };
 // The submission ring. A frame makes about 4 submissions (the sun atlas's flush, the eyes, the swap), so 4 slots
 // reached back to the previous frame: before recording the next one the render thread waited for the GPU to finish
 // it, a median 3.2 ms every frame though the GPU's own work was 3.4 ms of the 11.1 (desktop stereo at the headset's
 // 90 fps schedule, Linux, RTX 5080: every GPU wait was a slot 3 submissions back). 6 slots on desktop Linux and Windows
 // let the CPU record a frame while the GPU draws the last: no GPU waits, as with 8 (measured on Linux; Windows makes the
 // same submissions, not yet measured there). Each slot keeps a command pool, its
 // descriptor pools and its upload blocks (32 MiB each): about 190 MiB of memory a slot more, measured.
 // A Quest takes 6 too (2026-10-31, asked for after the desktop's gain; its 8 GB holds the memory: a Quest 3 log
 // showed 557 MiB resident). Phones and macOS keep 4.
 // WWHD_VK_SUBMISSION_SLOTS=2..16 sets it (docs/VR_PERFORMANCE.md, "Linux frame pacing").
 static size_t submission_slots() {
   static const size_t n = [] {
     if (const char* e = std::getenv("WWHD_VK_SUBMISSION_SLOTS")) return size_t(std::clamp(std::atoi(e), 2, 16));
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
     return size_t(6);
#elif defined(__ANDROID__)
     return size_t(quest_theater::standalone_headset() ? 6 : 4);  // (a Quest or Steam Frame - Lepton, 2026-11-04)
#else
     return size_t(4);
#endif
   }();
   return n;
 }
 std::deque<Submission> submissions=std::deque<Submission>(submission_slots());
 size_t activeSubmission=0;
 Screen tv,drc;
 // eye 0's surfaces (eye 1's: stereo.cpp; neither map moves, so a second thread can keep using eye 0's while the
 // render thread switches eyes). surfaces_of(eye), selected_surfaces() (the calling thread's selected eye's)
 std::unordered_multimap<uint32_t,std::unique_ptr<Surface>> surfaces;
 std::vector<Surface*> linearTargets; // eye 0's linear-aligned colour surfaces (never removed, like surfaces): GX2DrawDone write-back (selected_linear_targets)
};
extern Renderer R;
// The per-thread state a draw thread sets for itself (docs/VR_ATLAS_THREAD.md), read many times a draw: defined here,
// constant-initialized, so each read is a direct thread-local load with no call (a thread_local defined in another
// file is read through a wrapper call, and the accessors were calls too)
// (per_thread.h: plain globals while no draw thread is possible)
namespace thread_state {
PER_THREAD(PassState*, pass, nullptr)
PER_THREAD(DrawCounters*, counters, nullptr)
PER_THREAD(std::vector<Renderer::UploadBlock>*, uploadBlocks, nullptr)
PER_THREAD(VkDescriptorPool*, descriptorPool, nullptr)
}  // namespace thread_state
// the selected eye, per thread (stage 2.6): a draw thread keeps its own while the render thread switches eyes
PER_THREAD(int, stereoEye, 0)
// eye 1's surfaces (eye 0's: R.surfaces; stereo.cpp). Each eye keeps its own: selecting an eye swaps neither
inline std::unordered_multimap<uint32_t,std::unique_ptr<Surface>> eye1Surfaces;
// this thread's current render pass: R.pass unless set_thread_pass_state gave it its own
inline PassState& pass_state() { PassState* p = thread_state::pass(); return p ? *p : R.pass; }
inline void set_thread_pass_state(PassState* pass) { thread_state::pass() = pass; }  // null: R.pass again
// the surfaces of each eye: eye 0's are R.surfaces; selected: the calling thread's selected eye's
inline std::unordered_multimap<uint32_t,std::unique_ptr<Surface>>& surfaces_of(int eye) {
 return eye ? eye1Surfaces : R.surfaces;
}
inline std::unordered_multimap<uint32_t,std::unique_ptr<Surface>>& selected_surfaces() { return surfaces_of(stereoEye()); }
std::vector<Surface*>& selected_linear_targets();
uint32_t& selected_main_depth();
// where this thread counts its draws: R.drawCounters unless set_thread_draw_counters gave it its own
inline DrawCounters& draw_counters() { DrawCounters* c = thread_state::counters(); return c ? *c : R.drawCounters; }
inline void set_thread_draw_counters(DrawCounters* counters) { thread_state::counters() = counters; }  // null: again
// the upload arena this thread allocates from: R.uploadBlocks unless set_thread_upload_blocks gave it another
inline std::vector<Renderer::UploadBlock>& upload_blocks() {
 auto* b = thread_state::uploadBlocks();
 return b ? *b : R.uploadBlocks;
}
inline void set_thread_upload_blocks(std::vector<Renderer::UploadBlock>* blocks) { thread_state::uploadBlocks() = blocks; }
// the descriptor pool this thread allocates draw sets from: R.descriptorPool unless set_thread_descriptor_pool named
// another (by the Renderer field that holds the active slot's)
inline VkDescriptorPool descriptor_pool() {
 VkDescriptorPool* p = thread_state::descriptorPool();
 return p && *p ? *p : R.descriptorPool;
}
inline void set_thread_descriptor_pool(VkDescriptorPool* pool) { thread_state::descriptorPool() = pool; }  // null: again
void invalidate_surface_cache(decltype(Renderer::surfaces)& surfaces,
                              uint32_t flags, uint32_t addr, uint32_t size);
// Render-thread checkpoint; failures leave the cache dirty for a later retry.
void save_pipeline_cache(bool shutdown = true);
void present_completed_frame_while_pipeline_pending();
void begin_pipeline_pending_frame();
// Only guest GX2Flush uses this asynchronous path. flush() remains a drain for
// readbacks and renderer tools; final presentation also waits synchronously.
void flush_async();
void reset_pipeline_lookup_cache();
uint64_t draw_batch_submissions();
void vk_check(VkResult result,const char* operation);
// Immutable physical-device capabilities, cached on the renderer thread.
const VkFormatProperties& format_properties(VkFormat format);
uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags properties);
// preferred: extra property flags used when a memory type has them (else the required ones only)
Buffer create_buffer(VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags properties,
                     VkMemoryPropertyFlags preferred=0);
// host-visible TRANSFER_DST buffer the CPU reads back, host-cached where available
Buffer create_readback_buffer(VkDeviceSize size);
UploadSlice allocate_upload(VkDeviceSize size,VkDeviceSize alignment);
// WWHD_VK_WARM_PRUNE=1 during the setup's preparation (--warm-shaders): the driver cache is rebuilt from the recipes
bool warm_prune_enabled();
// A slice of the current submission's upload arena only if it fits in a block it already has (no new block);
// an empty slice otherwise. Texture staging (surfaces.cpp upload_surface, WWHD_VK_TEXTURE_STAGING).
UploadSlice allocate_upload_no_grow(VkDeviceSize size,VkDeviceSize alignment);
// Renderer smoke tests exercise the production snapshot helper with host data.
UploadSlice vertex_window_smoke_snapshot(uint32_t binding,uint32_t address,
    uint32_t reservation,uint32_t windowOffset,uint32_t windowLength,
    const void* data,bool poisonUnused);
// Synthetic shader metadata drives the production texture-binding path.
// the game's pipelines, those still compiling and the failed ones (draw.cpp; the memory log)
void guest_pipeline_counts(size_t& total, size_t& compiling, size_t& failed);
VkDescriptorSet texture_stage_smoke_bind(const uint32_t* registers,
    VkDescriptorSetLayout layout, uint32_t textureCount);
void defer_buffer(Buffer buffer);
// Advances whenever a buffer or image view a descriptor set may name is destroyed (any thread, before the destroy):
// descriptor sets kept across submissions (draw.cpp stage_descriptor_set) are dropped when it moved
uint64_t descriptor_targets_generation();
void note_descriptor_targets_destroyed(int site = 0);  // (site: diagnostics, WWHD_VK_DESCRIPTOR_DROPS=1)
// (docs/VR_SCENE_THREADS.md, "the helper threads' cost a draw") buffers destroyed without images: counted apart, so the
// kept descriptor sets drop only when one of theirs names such a buffer (staging buffers, mostly, never bound in one)
uint64_t destroyed_buffers_generation();
// the buffers destroyed after generation `since` into out; false when the record no longer reaches back that far
bool destroyed_buffers_since(uint64_t since, std::vector<VkBuffer>& out);
void defer_surface_image(VkImage image,VkDeviceMemory memory,std::vector<VkImageView> views);
VkCommandBuffer command_buffer();
void end_encoder();
void gpu_begin_render_scope(const std::array<Surface*,8>&,Surface*,const uint32_t*,uint32_t,uint32_t,uint32_t);
void gpu_count_render_draw();
GpuScopeToken gpu_begin_feedback_scope(const Surface&);
void gpu_end_feedback_scope(GpuScopeToken);
bool narrow_barriers();
void transition_buffer(Buffer&, VkPipelineStageFlags, VkAccessFlags);
void transition_image(Surface*,VkImageLayout,VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VkAccessFlags access=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
void forget_texture_views(int site = 1);
void service_captures();
void request_tv_dump(const std::string&, int);
void create_surface_image(Surface*,bool forRendering,VkExtent3D explicitExtent = {},
                          uint32_t sharedQueueFamily = VK_QUEUE_FAMILY_IGNORED);
void reset_ao_private_cache();
void destroy_surface_image(Surface*);
// Quest: the two-eye targets rescaled in the frame paired with eye 1 again, between frames (surfaces.cpp)
void run_pending_eye_repairs();
void note_one_eye_target(Surface* s);
// (multiview diagnostics) every target with draws counted since the last call, its counters reset (both eyes' maps,
// under the surfaces' lock): once every 10 s from draw.cpp multiview_log
struct MvTargetStat {
  uint32_t addr = 0, width = 0, height = 0, format = 0, missDraws = 0, heldDraws = 0;
  uint8_t eye = 255, missWhat = 0, heldWhy = 255, heldKind = 0;
  bool depth = false;
};
std::vector<MvTargetStat> take_multiview_target_stats();  // (surfaces.cpp: a target with one eye's image made two-eye, multiview_draw)
// the re-pairings so far, one line (the multiview summary, the crash context)
std::string multiview_repair_note();
// once per frame (swap): the texture budget's sweep (WWHD_VK_TEXTURE_BUDGET)
void texture_budget_step();
// once per frame (swap), after it: replacement texture packs' finished loads made into images, and their VRAM budget
// (texture_pack_vk.cpp)
void texture_pack_step();
// the guest texture's replacement image when one was made for the key the texture has now, else nullptr; notes the
// frame for the pack's budget (texture_pack_vk.cpp; under the surface lock)
Surface* replacement_for(Surface* guest);
// low memory (platform/memory_trim.h): releases the CPU textures not looked up within `idle` frames; bytes released
uint64_t trim_textures(uint64_t idle, uint64_t& released);
// releases one texture's image as the budget does (smoke test); its next lookup makes it again
void release_texture_image(Surface* s);
// texture staging (surfaces.cpp): -1 as WWHD_VK_TEXTURE_STAGING says, 0 own buffers, 1 the upload arena (smoke test)
extern int g_texture_staging_override;
VkImageView layer_view(Surface*,uint32_t slice);
VkImageView multiview_view(Surface* s);  // (stage M4) both eyes' layers of a two-eye render target
VkImageView make_sampled_view(Surface*,uint32_t key); // a sampled view by its key (surfaces.cpp sampled_texture_view)
void touch_image(Surface*); // dynamic resolution: clear an image made ahead once (surfaces.cpp)
VkImageView sampled_texture_view(Surface*,const uint32_t* texWords);
Surface* find_or_create_surface(const SurfaceDesc&,bool forRendering);
Surface* color_target(const uint32_t*,int,uint32_t* slice=nullptr,gfx::SizeRequest claim={});
Surface* depth_target(const uint32_t*,uint32_t* slice=nullptr);
Surface* surface_from_color_buffer(uint32_t,uint32_t* firstSlice=nullptr,uint32_t* numSlices=nullptr);
Surface* surface_from_depth_buffer(uint32_t,uint32_t* firstSlice=nullptr,uint32_t* numSlices=nullptr);
// maxLevels: the mip levels the sampler can reach; a rendered picture's mip chain is built that far
Surface* sampled_texture(const uint32_t*,bool,uint32_t maxLevels=16);
// (the sun atlas's thread, stage 3.2) a guest GX2DepthBuffer's bytes, copied when its clear was queued: its size, the
// image it names, and its depth clear
size_t depth_buffer_bytes();
uint32_t depth_buffer_image(uint32_t db);
void clear_depth_stencil_copy(const void* db,float depth,uint32_t stencil,uint32_t flags);
void upload_surface(Surface*);
void resample(Surface*,Surface*,uint32_t slices,float uMax=1,float vMax=1,uint32_t dstW=0,uint32_t dstH=0);
// scaled depth copies: blitted where the device can, else drawn (surfaces.cpp, issue #72);
// WWHD_VK_DEPTH_COPY=draw / =none override this for tests
enum class DepthCopyOverride { None, Draw, Unsupported };
extern DepthCopyOverride g_depthCopyOverride;
float res_scale();void set_res_scale(float);void latch_res_scale();
// dynamic resolution: the picture size the layout uses (the scan at the full internal resolution)
float nominal_width(const Surface*);float nominal_height(const Surface*);
uint64_t next_write_seq();
// Surface epoch (surfaces.cpp): bumped whenever the result of a texture or render-target lookup
// (find_or_create_surface, sampled views, sampler choice) or the content of a sampled CPU texture can
// change without a register write: surfaces created or their images destroyed/replaced, CPU texture
// uploads, GPU writes outside the draw's own targets (clears, copies), CPU copies, save-state resets.
// The continued-draw fast path (draw.cpp) reuses resolved state only within one epoch.
extern uint64_t g_surface_epoch;
// bumped whenever a surface cache gains, loses or swaps surfaces (texture invalidation's footprint index)
extern uint64_t g_surface_set_gen;
inline void mark_gpu_written(Surface* s){s->gpuWritten=true;s->writeSeq=next_write_seq();++g_surface_epoch;}
// A draw's own attachments: their newer writeSeq can only change lookups at their own addresses,
// which the fast path excludes (draw.cpp); a surface becoming GPU-written still ends the epoch.
inline void mark_draw_target_written(Surface* s){if(!s->gpuWritten)++g_surface_epoch;s->gpuWritten=true;s->writeSeq=next_write_seq();}
}

namespace gfxvk { void reset_feedback_images(); } // Call before device teardown, then drain retirements.

#ifdef WWHD_SDL_HOST
namespace gfxvk { SDL_Window* ensure_gamepad_window(bool create = true); bool gamepad_window_available(); }
#endif
namespace gfxvk {
// Renderer-thread/execution-mutex only; opt-in diagnostic endpoints.
void poll_present_observer();
void flush_present_observer();
}
