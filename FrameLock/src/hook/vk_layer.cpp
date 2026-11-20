// FrameLock Vulkan capture - implemented as an implicit Vulkan layer inside FrameLockHook{64,32}.dll (phase opus55).
//
// Why a layer and not an inline hook of vkQueuePresentKHR: a layer is the officially supported interception point,
// it sees every device/queue/swapchain from creation (so it can add TRANSFER_SRC usage to swapchain images, which a
// late hook cannot), and it gets the loader callback needed to initialise command buffers we allocate ourselves.
//
// Activation: the launcher writes FrameLock_VkLayer{64,32}.json next to the DLL and registers it under
// HKCU\Software\Khronos\Vulkan\ImplicitLayers. The manifest has enable_environment FRAMELOCK_VK_CAPTURE=1, which
// the launcher sets ONLY for the game it starts ("Launch game .exe and attach"). Therefore:
//   * Vulkan capture requires launching the game from FrameLock; attaching to an already-running Vulkan game cannot
//     add a layer (dllmain.cpp shows a status message saying so).
//   * Child processes of the launched game inherit the variable and also load the layer (harmless: it only
//     captures when the FrameLock core has an active take in that process).
//
// Timing contract (same as D3D/GL paths): capture runs inside FL_StepPre() -> the frame is copied with
// vkCmdCopyImageToBuffer, we block on a fence, copy the pixels to RAM and queue them, and only then may the core
// advance the clock. Any failure returns false -> the same simulation step is retried (never dropped/fabricated).
//
// Build switch: compile with FL_NO_VULKAN to produce a stub (no Vulkan headers needed) - see CMake option
// FL_ENABLE_VULKAN. The stub keeps the .def export valid but refuses negotiation, so the loader skips the layer.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef CreateSemaphore   // windows.h maps it to CreateSemaphoreW; we use it as a Vulkan dispatch member name
#include <atomic>
#include "flcore.h"

static std::atomic<bool> g_vkLayerActive{ false };
bool FL_VkLayerActive() { return g_vkLayerActive.load(); }
// review3: sticky latch, set on the first vkQueuePresentKHR (with >= 1 swapchain) seen by the layer in this process.
// Lets the DXGI hooks stand down when a driver routes Vulkan presents through DXGI (e.g. NVIDIA "layered on DXGI"),
// so only one path ever owns the take/clock. review2 used a 1 s "last present" window instead, which re-enabled
// the DXGI path whenever a single Vulkan present ran longer than 1 s (encode-queue backpressure in FL_QueueFrame,
// or the 10 s capture fence wait) and during loading screens -> exactly the double step it was meant to prevent.
// Once set it is never cleared: a process that has presented through the FrameLock Vulkan layer is a Vulkan game.
static std::atomic<bool> g_vkHasPresented{ false };
bool FL_VkLayerPresenting() {
    return g_vkHasPresented.load(std::memory_order_acquire) && g_vkLayerActive.load(std::memory_order_acquire);
}

#ifdef FL_NO_VULKAN
// Stub: VkResult VK_ERROR_INITIALIZATION_FAILED = -3. Same calling convention as VKAPI_CALL on Windows.
extern "C" int __stdcall vkNegotiateLoaderLayerInterfaceVersion(void*) { return -3; }
#else

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>

// ------------------------------------------------------------------ helpers
// Non-dispatchable handles are pointers on 64-bit and uint64_t on 32-bit; normalise to uint64_t for map keys.
template <class T> static uint64_t H64(T h) { uint64_t v = 0; memcpy(&v, &h, sizeof(h) < sizeof(v) ? sizeof(h) : sizeof(v)); return v; }
// Dispatchable handles (instance/physical device/device/queue/command buffer) start with the loader's dispatch
// table pointer; a device and its queues/command buffers share it, as do an instance and its physical devices.
static void* DKey(const void* dispatchable) { return *(void* const*)dispatchable; }
// VkResult -> HRESULT for the core's logs: 0x8076xxxx where xxxx = -VkResult (facility 0x076, 'v').
static HRESULT VkHr(VkResult r) { return r == VK_SUCCESS ? S_OK : (HRESULT)(0x80760000u | ((unsigned)(-(int)r) & 0xFFFFu)); }

struct InstData {
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
};

struct DevData {
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    InstData* inst = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    VkPhysicalDeviceMemoryProperties mem{};
    std::vector<VkQueueFamilyProperties> qf;
#define FL_VK_DEV_FUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(CreateSwapchainKHR) X(DestroySwapchainKHR) \
    X(GetSwapchainImagesKHR) X(QueuePresentKHR) X(QueueSubmit) X(CreateCommandPool) X(DestroyCommandPool) \
    X(ResetCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) \
    X(CmdCopyImageToBuffer) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) \
    X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges) X(CreateFence) \
    X(DestroyFence) X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore)
#define FL_VK_DECL(n) PFN_vk##n n = nullptr;
    FL_VK_DEV_FUNCS(FL_VK_DECL)
#undef FL_VK_DECL
    // capture objects (used only under the core's step mutex, i.e. from FL_StepPre)
    bool capOk = false;            // review2: every function VkCapture/EnsureCaptureObjects calls is non-null
    uint32_t capFamily = UINT32_MAX;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory bufMem = VK_NULL_HANDLE;
    VkDeviceSize bufSize = 0;
    void* bufPtr = nullptr;
    bool bufCoherent = true;
    // Semaphores we signal and the present waits on instead of the game's. Reuse heuristic: a ring of 16 is far
    // larger than any swapchain depth; core Vulkan offers no way to know when a present's wait completed
    // (VK_EXT_swapchain_maintenance1 present fences would - see handoff).
    std::vector<VkSemaphore> semRing;
    uint32_t semNext = 0;
};

struct QueueData { DevData* d = nullptr; uint32_t family = UINT32_MAX; };
struct SwapData {
    DevData* d = nullptr; VkFormat format = VK_FORMAT_UNDEFINED; VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{}; VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; bool transferSrc = false;
    std::vector<VkImage> images;
};

static std::mutex g_vkMx;
static std::unordered_map<void*, InstData*> g_inst;      // key: DKey(instance) (== DKey(physical device))
static std::unordered_map<void*, DevData*> g_dev;        // key: DKey(device)   (== DKey(queue))
static std::unordered_map<void*, QueueData> g_queues;    // key: VkQueue
static std::unordered_map<uint64_t, SwapData> g_swaps;   // key: H64(swapchain) (unique per device; see note in CreateSwapchain)
static InstData* g_lastInst = nullptr;

static InstData* InstOf(const void* h) { std::lock_guard<std::mutex> l(g_vkMx); auto it = g_inst.find(DKey(h)); return it != g_inst.end() ? it->second : g_lastInst; }
static DevData* DevOf(const void* h) { std::lock_guard<std::mutex> l(g_vkMx); auto it = g_dev.find(DKey(h)); return it != g_dev.end() ? it->second : nullptr; }

static bool FormatToPF(VkFormat f, PF& pf) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: pf = PF::BGRA8; return true;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: pf = PF::RGBA8; return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: pf = PF::RGB10A2; return true;        // R in bits 0..9  == DXGI R10G10B10A2
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: pf = PF::D9_A2R10G10B10; return true; // B in bits 0..9  == D3D9 A2R10G10B10
    case VK_FORMAT_R16G16B16A16_SFLOAT: pf = PF::RGBA16F; return true;
    default: return false;
    }
}

// ------------------------------------------------------------------ capture objects
static void DestroyCaptureObjects(DevData* d) {
    if (d->bufPtr && d->UnmapMemory) d->UnmapMemory(d->dev, d->bufMem);
    if (d->buf) d->DestroyBuffer(d->dev, d->buf, nullptr);
    if (d->bufMem) d->FreeMemory(d->dev, d->bufMem, nullptr);
    if (d->fence) d->DestroyFence(d->dev, d->fence, nullptr);
    if (d->pool) d->DestroyCommandPool(d->dev, d->pool, nullptr);   // frees d->cmd
    for (VkSemaphore s : d->semRing) if (s) d->DestroySemaphore(d->dev, s, nullptr);
    d->semRing.clear(); d->semNext = 0;
    d->bufPtr = nullptr; d->buf = VK_NULL_HANDLE; d->bufMem = VK_NULL_HANDLE; d->bufSize = 0;
    d->fence = VK_NULL_HANDLE; d->pool = VK_NULL_HANDLE; d->cmd = VK_NULL_HANDLE; d->capFamily = UINT32_MAX;
}
// After a fence timeout the GPU may still use these objects: forget (leak) them instead of destroying.
static void RetireCaptureObjects(DevData* d) {
    FL_Log(L"[Vulkan] capture objects retired (leaked) after a GPU timeout; they will be recreated");
    d->bufPtr = nullptr; d->buf = VK_NULL_HANDLE; d->bufMem = VK_NULL_HANDLE; d->bufSize = 0;
    d->fence = VK_NULL_HANDLE; d->pool = VK_NULL_HANDLE; d->cmd = VK_NULL_HANDLE; d->capFamily = UINT32_MAX;
}

static uint32_t FindMemType(const DevData* d, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = d->mem.memoryTypes[i].propertyFlags;
        if ((f & want) != want) continue;
        if ((f & prefer) == prefer) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    return fallback;
}

// Returns false with stage/hr set on failure.
static bool EnsureCaptureObjects(DevData* d, uint32_t family, VkDeviceSize size, const wchar_t*& stage, HRESULT& hr) {
    VkResult r;
    if (d->capFamily != family && d->pool) {        // present queue family changed: rebuild pool/cmd/fence
        if (d->bufPtr) { d->UnmapMemory(d->dev, d->bufMem); d->bufPtr = nullptr; }
        if (d->buf) d->DestroyBuffer(d->dev, d->buf, nullptr);
        if (d->bufMem) d->FreeMemory(d->dev, d->bufMem, nullptr);
        d->buf = VK_NULL_HANDLE; d->bufMem = VK_NULL_HANDLE; d->bufSize = 0;
        if (d->fence) d->DestroyFence(d->dev, d->fence, nullptr);
        d->DestroyCommandPool(d->dev, d->pool, nullptr);
        d->fence = VK_NULL_HANDLE; d->pool = VK_NULL_HANDLE; d->cmd = VK_NULL_HANDLE;
    }
    if (!d->pool) {
        stage = L"vkCreateCommandPool";
        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pci.queueFamilyIndex = family;
        if ((r = d->CreateCommandPool(d->dev, &pci, nullptr, &d->pool)) != VK_SUCCESS) { d->pool = VK_NULL_HANDLE; hr = VkHr(r); return false; }
        stage = L"vkAllocateCommandBuffers";
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = d->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        if ((r = d->AllocateCommandBuffers(d->dev, &ai, &d->cmd)) != VK_SUCCESS) { d->cmd = VK_NULL_HANDLE; hr = VkHr(r); return false; }
        // Command buffers created by a layer are dispatchable objects the loader has not seen: initialise them.
        stage = L"vkSetDeviceLoaderData(command buffer)";
        if (d->setLoaderData) { if ((r = d->setLoaderData(d->dev, d->cmd)) != VK_SUCCESS) { hr = VkHr(r); return false; } }
        else *(void**)d->cmd = *(void**)d->dev;
        stage = L"vkCreateFence";
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if ((r = d->CreateFence(d->dev, &fci, nullptr, &d->fence)) != VK_SUCCESS) { d->fence = VK_NULL_HANDLE; hr = VkHr(r); return false; }
        d->capFamily = family;
    }
    if (d->semRing.empty()) {
        stage = L"vkCreateSemaphore";
        VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        for (int i = 0; i < 16; i++) {
            VkSemaphore s = VK_NULL_HANDLE;
            if ((r = d->CreateSemaphore(d->dev, &sci, nullptr, &s)) != VK_SUCCESS) { hr = VkHr(r); return false; }
            d->semRing.push_back(s);
        }
    }
    if (!d->buf || d->bufSize < size) {
        if (d->bufPtr) { d->UnmapMemory(d->dev, d->bufMem); d->bufPtr = nullptr; }
        if (d->buf) d->DestroyBuffer(d->dev, d->buf, nullptr);
        if (d->bufMem) d->FreeMemory(d->dev, d->bufMem, nullptr);
        d->buf = VK_NULL_HANDLE; d->bufMem = VK_NULL_HANDLE; d->bufSize = 0;
        stage = L"vkCreateBuffer (readback)";
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = size; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if ((r = d->CreateBuffer(d->dev, &bci, nullptr, &d->buf)) != VK_SUCCESS) { d->buf = VK_NULL_HANDLE; hr = VkHr(r); return false; }
        VkMemoryRequirements mr{}; d->GetBufferMemoryRequirements(d->dev, d->buf, &mr);
        stage = L"no HOST_VISIBLE memory type for readback buffer";
        uint32_t mt = FindMemType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX) mt = FindMemType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (mt == UINT32_MAX) { hr = E_FAIL; return false; }
        stage = L"vkAllocateMemory (readback)";
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
        if ((r = d->AllocateMemory(d->dev, &mai, nullptr, &d->bufMem)) != VK_SUCCESS) { d->bufMem = VK_NULL_HANDLE; hr = VkHr(r); return false; }
        stage = L"vkBindBufferMemory";
        if ((r = d->BindBufferMemory(d->dev, d->buf, d->bufMem, 0)) != VK_SUCCESS) { hr = VkHr(r); return false; }
        stage = L"vkMapMemory";
        if ((r = d->MapMemory(d->dev, d->bufMem, 0, VK_WHOLE_SIZE, 0, &d->bufPtr)) != VK_SUCCESS) { d->bufPtr = nullptr; hr = VkHr(r); return false; }
        d->bufCoherent = (d->mem.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        d->bufSize = size;
    }
    return true;
}

// ------------------------------------------------------------------ capture callback (runs under the core step mutex)
struct PresentCtx {
    DevData* d; VkQueue queue; const VkPresentInfoKHR* pi;
    VkSemaphore replaceWait;     // set once our submit consumed the game's wait semaphores
};

static bool VkCapture(void* c, int64_t idx, bool v, const wchar_t** pStage, HRESULT* pHr) {
    PresentCtx& ctx = *(PresentCtx*)c;
    DevData* d = ctx.d;
    const wchar_t*& stage = *pStage; HRESULT& hr = *pHr;
    int64_t t0 = FL_RealQPC();
    hr = E_FAIL;

    stage = L"Vulkan swapchain not tracked (created before the layer?)";
    SwapData sw; QueueData qd;
    {
        std::lock_guard<std::mutex> l(g_vkMx);
        auto it = g_swaps.find(H64(ctx.pi->pSwapchains[0]));
        if (it == g_swaps.end() || it->second.d != d) return false;
        sw = it->second;   // copy; images vector is small
        auto qi = g_queues.find((void*)ctx.queue);
        if (qi != g_queues.end()) qd = qi->second;
    }
    stage = L"Vulkan capture functions missing on this device (see '[Vulkan] layer: device' log line)";
    if (!d->capOk) { hr = E_NOTIMPL; return false; }   // review2: never call a null next-in-chain pointer
    stage = L"Vulkan present queue family unknown";
    if (qd.family == UINT32_MAX || qd.family >= d->qf.size()) return false;
    stage = L"Vulkan present queue cannot run transfer commands";
    if (!(d->qf[qd.family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) return false;
    stage = L"Vulkan swapchain images lack TRANSFER_SRC usage (surface does not support it)";
    if (!sw.transferSrc) { hr = E_NOTIMPL; return false; }
    stage = L"Vulkan shared-present-mode swapchain (unsupported layout)";
    if (sw.presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR || sw.presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR) { hr = E_NOTIMPL; return false; }
    PF pf;
    stage = L"Vulkan swapchain format not supported by the encoder";
    if (!FormatToPF(sw.format, pf)) { FL_Log(L"[Vulkan] unsupported swapchain format %d", (int)sw.format); hr = E_NOTIMPL; return false; }
    uint32_t imgIdx = ctx.pi->pImageIndices[0];
    stage = L"Vulkan image index out of range";
    if (imgIdx >= sw.images.size()) return false;
    VkImage img = sw.images[imgIdx];
    const uint32_t w = sw.extent.width, h = sw.extent.height;
    const size_t bpp = FL_BytesPerPixel(pf), pitch = (size_t)w * bpp;
    const VkDeviceSize size = (VkDeviceSize)pitch * h;
    stage = L"Vulkan zero-sized swapchain";
    if (!w || !h) return false;
    if (idx == 0 && sw.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && !(pf == PF::RGBA16F && sw.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT))
        FL_Log(L"[Vulkan] WARNING: swapchain color space %d is not SDR sRGB; frames are encoded as if it were (colors may be wrong)", (int)sw.colorSpace);

    if (!EnsureCaptureObjects(d, qd.family, size, stage, hr)) return false;

    VkResult r;
    stage = L"vkResetCommandPool";
    if ((r = d->ResetCommandPool(d->dev, d->pool, 0)) != VK_SUCCESS) { hr = VkHr(r); return false; }
    stage = L"vkBeginCommandBuffer";
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if ((r = d->BeginCommandBuffer(d->cmd, &bi)) != VK_SUCCESS) { hr = VkHr(r); return false; }

    VkImageMemoryBarrier ib{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    ib.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img; ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    d->CmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);

    VkBufferImageCopy rg{};
    rg.bufferOffset = 0; rg.bufferRowLength = 0; rg.bufferImageHeight = 0;   // tightly packed: pitch = w * bpp
    rg.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    rg.imageOffset = { 0, 0, 0 }; rg.imageExtent = { w, h, 1 };
    d->CmdCopyImageToBuffer(d->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->buf, 1, &rg);

    VkImageMemoryBarrier back = ib;
    back.srcAccessMask = 0; back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier bb{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = d->buf; bb.offset = 0; bb.size = VK_WHOLE_SIZE;
    d->CmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                          0, nullptr, 1, &bb, 1, &back);
    stage = L"vkEndCommandBuffer";
    if ((r = d->EndCommandBuffer(d->cmd)) != VK_SUCCESS) { hr = VkHr(r); return false; }

    // Our submit waits on the game's present-wait semaphores (thereby consuming them) and signals one of ours,
    // which the present then waits on instead (see FL_QueuePresentKHR).
    VkSemaphore sig = d->semRing[d->semNext % d->semRing.size()];
    std::vector<VkPipelineStageFlags> waitStages(ctx.pi->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = ctx.pi->waitSemaphoreCount; si.pWaitSemaphores = ctx.pi->pWaitSemaphores;
    si.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
    si.commandBufferCount = 1; si.pCommandBuffers = &d->cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sig;
    stage = L"vkQueueSubmit (capture copy)";
    // Spec: a failed vkQueueSubmit leaves the semaphores untouched, so the original present stays valid.
    if ((r = d->QueueSubmit(ctx.queue, 1, &si, d->fence)) != VK_SUCCESS) { hr = VkHr(r); return false; }
    d->semNext++;
    ctx.replaceWait = sig;   // from here on the present MUST wait on `sig`, whatever happens below

    stage = L"vkWaitForFences (capture copy, 10 s)";
    r = d->WaitForFences(d->dev, 1, &d->fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
    if (r != VK_SUCCESS) { hr = r == VK_TIMEOUT ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : VkHr(r); RetireCaptureObjects(d); return false; }
    d->ResetFences(d->dev, 1, &d->fence);
    if (!d->bufCoherent) {
        stage = L"vkInvalidateMappedMemoryRanges";
        VkMappedMemoryRange mr{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE }; mr.memory = d->bufMem; mr.offset = 0; mr.size = VK_WHOLE_SIZE;
        if ((r = d->InvalidateMappedMemoryRanges(d->dev, 1, &mr)) != VK_SUCCESS) { hr = VkHr(r); return false; }
    }
    std::vector<uint8_t> raw((size_t)size);
    memcpy(raw.data(), d->bufPtr, (size_t)size);
    if (v) FL_Log(L"[Vulkan] frame %lld: %ux%u fmt %d copied + read back in %.2f ms", (long long)idx, w, h, (int)sw.format, FL_RealMs(FL_RealQPC() - t0));
    FL_SetApi(L"Vulkan (FrameLock layer)");
    stage = L"encode queue";
    FL_QueueFrame(idx, (int)w, (int)h, pf, pitch, std::move(raw));
    hr = S_OK;
    return true;
}

// ------------------------------------------------------------------ intercepted entry points
static VKAPI_ATTR VkResult VKAPI_CALL FL_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pi) {
    DevData* d = DevOf(queue);
    if (!d || !d->QueuePresentKHR) return VK_ERROR_INITIALIZATION_FAILED;   // cannot happen with a valid queue
    // review3: latch BEFORE calling down, so a DXGI present issued by the driver on another thread for this very
    // Vulkan present already sees the stand-down (see FL_VkLayerPresenting).
    if (pi && pi->swapchainCount && !g_vkHasPresented.load(std::memory_order_relaxed)) {
        g_vkHasPresented.store(true, std::memory_order_release);
        FL_Log(L"[Vulkan] layer: first vkQueuePresentKHR seen; DXGI Present hooks now stand down for this process");
    }
    if (!pi || pi->swapchainCount == 0 || !FL_BeginPresent()) return d->QueuePresentKHR(queue, pi);
    // Only pSwapchains[0] is captured/used as the take target (multi-swapchain presents are rare). On 32-bit the
    // 64-bit handle is truncated to form the target id; collisions are theoretically possible.
    PresentCtx ctx{ d, queue, pi, VK_NULL_HANDLE };
    int rc = FL_StepPre((void*)(uintptr_t)H64(pi->pSwapchains[0]), &VkCapture, &ctx);
    VkPresentInfoKHR mod;
    const VkPresentInfoKHR* use = pi;
    if (ctx.replaceWait) { mod = *pi; mod.waitSemaphoreCount = 1; mod.pWaitSemaphores = &ctx.replaceWait; use = &mod; }
    VkResult res = d->QueuePresentKHR(queue, use);
    FL_StepPost(rc);
    FL_EndPresent();
    return res;
}

static void TrackQueue(VkDevice device, uint32_t family, VkQueue q) {
    if (!q) return;
    DevData* d = DevOf(device);
    std::lock_guard<std::mutex> l(g_vkMx);
    g_queues[(void*)q] = QueueData{ d, family };
}
static VKAPI_ATTR void VKAPI_CALL FL_vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* pQueue) {
    DevData* d = DevOf(device);
    if (!d || !d->GetDeviceQueue) return;
    d->GetDeviceQueue(device, family, index, pQueue);
    if (pQueue) TrackQueue(device, family, *pQueue);
}
static VKAPI_ATTR void VKAPI_CALL FL_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* pQueue) {
    DevData* d = DevOf(device);
    if (!d || !d->GetDeviceQueue2) return;
    d->GetDeviceQueue2(device, info, pQueue);
    if (pQueue && info) TrackQueue(device, info->queueFamilyIndex, *pQueue);
}

static VKAPI_ATTR VkResult VKAPI_CALL FL_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    DevData* d = DevOf(device);
    if (!d || !d->CreateSwapchainKHR || !ci || !out) return VK_ERROR_INITIALIZATION_FAILED;   // cannot happen via our GDPA
    VkSwapchainCreateInfoKHR mod = *ci;
    bool canSrc = (ci->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (!canSrc && d->inst && d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR) {
        VkSurfaceCapabilitiesKHR caps{};
        if (d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, ci->surface, &caps) == VK_SUCCESS &&
            (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            mod.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; canSrc = true;
        }
    }
    VkResult r = d->CreateSwapchainKHR(device, &mod, a, out);
    if (r != VK_SUCCESS && mod.imageUsage != ci->imageUsage) {   // be conservative: never break the game
        FL_Log(L"[Vulkan] vkCreateSwapchainKHR with added TRANSFER_SRC failed (%d); retrying unmodified", (int)r);
        canSrc = false; r = d->CreateSwapchainKHR(device, ci, a, out);
    }
    if (r != VK_SUCCESS || !out) return r;
    SwapData s; s.d = d; s.format = ci->imageFormat; s.colorSpace = ci->imageColorSpace; s.extent = ci->imageExtent;
    s.presentMode = ci->presentMode; s.transferSrc = canSrc;
    uint32_t n = 0;
    if (d->GetSwapchainImagesKHR(device, *out, &n, nullptr) == VK_SUCCESS && n) {
        s.images.resize(n);
        if (d->GetSwapchainImagesKHR(device, *out, &n, s.images.data()) != VK_SUCCESS) s.images.clear(); else s.images.resize(n);
    }
    FL_Log(L"[Vulkan] swapchain created: %ux%u format %d colorspace %d images %u transferSrc %d",
           s.extent.width, s.extent.height, (int)s.format, (int)s.colorSpace, (unsigned)s.images.size(), (int)canSrc);
    // Non-dispatchable handles are unique per device for swapchains in practice; we additionally store the owning
    // device and check it on lookup.
    std::lock_guard<std::mutex> l(g_vkMx);
    g_swaps[H64(*out)] = std::move(s);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL FL_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    DevData* d = DevOf(device);
    { std::lock_guard<std::mutex> l(g_vkMx); g_swaps.erase(H64(sc)); }
    if (d && d->DestroySwapchainKHR) d->DestroySwapchainKHR(device, sc, a);
}

static VkLayerDeviceCreateInfo* FindDeviceChain(const VkDeviceCreateInfo* ci, VkLayerFunction fn) {
    auto p = (VkLayerDeviceCreateInfo*)ci->pNext;
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && p->function == fn)) p = (VkLayerDeviceCreateInfo*)p->pNext;
    return p;
}
static VkLayerInstanceCreateInfo* FindInstanceChain(const VkInstanceCreateInfo* ci, VkLayerFunction fn) {
    auto p = (VkLayerInstanceCreateInfo*)ci->pNext;
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && p->function == fn)) p = (VkLayerInstanceCreateInfo*)p->pNext;
    return p;
}

static VKAPI_ATTR VkResult VKAPI_CALL FL_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    VkLayerDeviceCreateInfo* link = FindDeviceChain(ci, VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerDeviceCreateInfo* cb = FindDeviceChain(ci, VK_LOADER_DATA_CALLBACK);
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;      // advance the chain for the next layer
    InstData* inst = InstOf(phys);
    auto createDevice = (PFN_vkCreateDevice)gipa(inst ? inst->inst : VK_NULL_HANDLE, "vkCreateDevice");
    if (!createDevice) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = createDevice(phys, ci, a, out);
    if (r != VK_SUCCESS) return r;

    auto d = new DevData();
    d->dev = *out; d->phys = phys; d->inst = inst; d->gdpa = gdpa;
    d->setLoaderData = cb ? cb->u.pfnSetDeviceLoaderData : nullptr;
#define FL_VK_GET(n) d->n = (PFN_vk##n)gdpa(*out, "vk" #n);
    FL_VK_DEV_FUNCS(FL_VK_GET)
#undef FL_VK_GET
    if (inst && inst->GetPhysicalDeviceMemoryProperties) inst->GetPhysicalDeviceMemoryProperties(phys, &d->mem);
    if (inst && inst->GetPhysicalDeviceQueueFamilyProperties) {
        uint32_t n = 0; inst->GetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
        d->qf.resize(n); if (n) inst->GetPhysicalDeviceQueueFamilyProperties(phys, &n, d->qf.data());
    }
    // review2: check EVERY function the capture path calls (was a subset -> possible null call in VkCapture).
    bool capOk = d->GetSwapchainImagesKHR && d->QueueSubmit && d->CreateCommandPool && d->DestroyCommandPool &&
                 d->ResetCommandPool && d->AllocateCommandBuffers && d->BeginCommandBuffer && d->EndCommandBuffer &&
                 d->CmdPipelineBarrier && d->CmdCopyImageToBuffer && d->CreateBuffer && d->DestroyBuffer &&
                 d->GetBufferMemoryRequirements && d->AllocateMemory && d->FreeMemory && d->BindBufferMemory &&
                 d->MapMemory && d->UnmapMemory && d->InvalidateMappedMemoryRanges && d->CreateFence && d->DestroyFence &&
                 d->WaitForFences && d->ResetFences && d->CreateSemaphore && d->DestroySemaphore &&
                 d->mem.memoryTypeCount > 0 && !d->qf.empty();
    d->capOk = capOk;
    FL_Log(L"[Vulkan] layer: device %p created (%u queue families, swapchain funcs %s, capture funcs %s)",
           (void*)*out, (unsigned)d->qf.size(), d->QueuePresentKHR ? L"yes" : L"no", capOk ? L"complete" : L"MISSING");
    std::lock_guard<std::mutex> l(g_vkMx);
    g_dev[DKey(*out)] = d;
    return r;
}

static VKAPI_ATTR void VKAPI_CALL FL_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* a) {
    if (!device) return;
    DevData* d = nullptr;
    {
        std::lock_guard<std::mutex> l(g_vkMx);
        auto it = g_dev.find(DKey(device)); if (it == g_dev.end()) return;
        d = it->second; g_dev.erase(it);
        for (auto q = g_queues.begin(); q != g_queues.end();) { if (q->second.d == d) q = g_queues.erase(q); else ++q; }
        for (auto s = g_swaps.begin(); s != g_swaps.end();) { if (s->second.d == d) s = g_swaps.erase(s); else ++s; }
    }
    if (d->capOk) DestroyCaptureObjects(d);   // app must have made the device idle before vkDestroyDevice (spec requirement)
    PFN_vkDestroyDevice next = d->DestroyDevice;
    delete d;
    if (next) next(device, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL FL_vkCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    VkLayerInstanceCreateInfo* link = FindInstanceChain(ci, VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto createInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!createInstance) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = createInstance(ci, a, out);
    if (r != VK_SUCCESS) return r;
    auto id = new InstData();
    id->inst = *out; id->gipa = gipa;
    id->DestroyInstance = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
    id->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(*out, "vkGetPhysicalDeviceMemoryProperties");
    id->GetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(*out, "vkGetPhysicalDeviceQueueFamilyProperties");
    id->GetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(*out, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    {
        std::lock_guard<std::mutex> l(g_vkMx);
        g_inst[DKey(*out)] = id; g_lastInst = id;
    }
    g_vkLayerActive = true;
    FL_Log(L"[Vulkan] layer: instance %p created (FrameLock Vulkan layer active)", (void*)*out);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL FL_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* a) {
    if (!instance) return;
    InstData* id = nullptr;
    {
        std::lock_guard<std::mutex> l(g_vkMx);
        auto it = g_inst.find(DKey(instance)); if (it == g_inst.end()) return;
        id = it->second; g_inst.erase(it);
        if (g_lastInst == id) g_lastInst = g_inst.empty() ? nullptr : g_inst.begin()->second;
        for (auto& dv : g_dev) if (dv.second->inst == id) dv.second->inst = nullptr;   // devices must already be gone (spec)
    }
    PFN_vkDestroyInstance next = id->DestroyInstance;
    delete id;
    if (next) next(instance, a);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FL_vkGetDeviceProcAddr(VkDevice device, const char* name);

static PFN_vkVoidFunction DeviceIntercept(const char* n) {
    if (!strcmp(n, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)FL_vkGetDeviceProcAddr;
    if (!strcmp(n, "vkDestroyDevice")) return (PFN_vkVoidFunction)FL_vkDestroyDevice;
    if (!strcmp(n, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)FL_vkGetDeviceQueue;
    if (!strcmp(n, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction)FL_vkGetDeviceQueue2;
    if (!strcmp(n, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)FL_vkCreateSwapchainKHR;
    if (!strcmp(n, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)FL_vkDestroySwapchainKHR;
    if (!strcmp(n, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)FL_vkQueuePresentKHR;
    return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FL_vkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    DevData* d = device ? DevOf(device) : nullptr;
    if (!d) return nullptr;
    // vkGetDeviceQueue2 only exists when the next layer/driver provides it (Vulkan 1.1+).
    if (!strcmp(name, "vkGetDeviceQueue2") && !d->GetDeviceQueue2) return nullptr;
    // Swapchain entry points only exist when VK_KHR_swapchain is enabled on this device: never hand out an
    // intercept whose next-in-chain pointer is null (the app would crash calling it).
    if ((!strcmp(name, "vkCreateSwapchainKHR") && !d->CreateSwapchainKHR) ||
        (!strcmp(name, "vkDestroySwapchainKHR") && !d->DestroySwapchainKHR) ||
        (!strcmp(name, "vkQueuePresentKHR") && !d->QueuePresentKHR)) return nullptr;
    if (PFN_vkVoidFunction f = DeviceIntercept(name)) return f;
    return d->gdpa(device, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FL_vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)FL_vkGetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)FL_vkCreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)FL_vkDestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)FL_vkCreateDevice;
    if (PFN_vkVoidFunction f = DeviceIntercept(name)) return f;
    if (!instance) return nullptr;
    InstData* id = nullptr;
    { std::lock_guard<std::mutex> l(g_vkMx); auto it = g_inst.find(DKey(instance)); if (it != g_inst.end()) id = it->second; }
    return id ? id->gipa(instance, name) : nullptr;
}

// Exported via FrameLockHook.def (undecorated on x86 too). Loader-layer interface version 2.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* p) {
    if (!p || p->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (p->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    p->loaderLayerInterfaceVersion = 2;
    p->pfnGetInstanceProcAddr = FL_vkGetInstanceProcAddr;
    p->pfnGetDeviceProcAddr = FL_vkGetDeviceProcAddr;
    p->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

#endif // FL_NO_VULKAN
