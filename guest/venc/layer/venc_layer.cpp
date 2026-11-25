// VK_LAYER_STEAMAC_video_encode: Vulkan Video encode (H.264, H.265) for drivers that have none.
//
// SteamVR's Steam Frame streaming driver (vrlink) encodes with Vulkan Video. Inside the steamac VM the Vulkan
// driver is Venus, whose host driver on macOS has no video queues. This implicit layer adds what vrlink needs:
//
//   - a video-encode queue family (appended after the driver's families), VK_KHR_video_queue,
//     VK_KHR_video_encode_queue, VK_KHR_video_encode_h264/h265 and VK_KHR_video_maintenance1;
//   - video sessions, session parameters, encode feedback queries and the video coding commands, recorded into
//     layer-owned command buffers;
//   - a worker per video queue that, for every submitted batch, copies the source pictures to host memory on a
//     real queue (after the batch's wait semaphores), encodes them with a backend (venc_backend.h: VideoToolbox
//     on the Mac, or x264/x265), copies the bitstream into the application's buffers and signals the batch's
//     semaphores and fence on a real queue.
//
// Images and buffers with video usages become ordinary transfer images/buffers in the driver: video usages,
// layouts, stages, accesses and the video queue family are rewritten wherever the driver would see them.
// The backend owns the bitstream syntax: parameter sets come from the backend and are reported as overrides,
// which the Vulkan Video spec allows and applications (FFmpeg, vrlink) parse back.

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "venc_backend.h"
#include "h264_rewrite.h"
#include "h265_rewrite.h"

#define LAYER_NAME "VK_LAYER_STEAMAC_video_encode"
#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {

// ------------------------------------------------------------------------------------------------- logging
int g_log_level = -1;
void logf(int lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void logf(int lvl, const char *fmt, ...)
{
    if (g_log_level < 0) {
        const char *e = getenv("VENC_LOG");
        g_log_level = e ? atoi(e) : 1;
    }
    if (lvl > g_log_level)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[venc] %s\n", buf);
    // and /tmp/steamac-venc/<process>.<pid>.log, info and errors only, for problem reports (SteamVR's processes
    // run in containers whose stderr nobody sees); /tmp is shared with the Steam Runtime containers
    if (lvl > 1)
        return;
    static FILE *f = [] {
        mkdir("/tmp/steamac-venc", 01777);
        chmod("/tmp/steamac-venc", 01777);
        char comm[32] = "unknown";
        if (FILE *c = fopen("/proc/self/comm", "r")) {
            if (fgets(comm, sizeof(comm), c))
                comm[strcspn(comm, "\n")] = 0;
            fclose(c);
        }
        char path[128];
        snprintf(path, sizeof(path), "/tmp/steamac-venc/%s.%d.log", comm, (int)getpid());
        FILE *o = fopen(path, "a");
        if (o)
            setvbuf(o, nullptr, _IOLBF, 0);
        return o;
    }();
    if (f) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        fprintf(f, "%lld.%03ld %s\n", (long long)ts.tv_sec, ts.tv_nsec / 1000000, buf);
    }
}
#define LOGE(...) logf(0, __VA_ARGS__)
#define LOGI(...) logf(1, __VA_ARGS__)
#define LOGD(...) logf(2, __VA_ARGS__)
#define LOGT(...) logf(3, __VA_ARGS__)

// ------------------------------------------------------------------------------------------------- who gets it
// An implicit layer sees every Vulkan process in the VM. Only the processes that encode get the video queue:
// SteamVR's server (vrlink, the Steam Frame streaming driver, runs inside vrserver), or any process started with
// STEAMAC_VENC=1. Everything else (games) sees the driver unchanged; STEAMAC_VENC=0 turns it off everywhere.
bool process_wanted()
{
    static int wanted = -1;
    if (wanted >= 0)
        return wanted;
    const char *e = getenv("STEAMAC_VENC");
    if (e && *e) {
        wanted = *e != '0';
        return wanted;
    }
    char comm[64] = {0};
    if (FILE *f = fopen("/proc/self/comm", "r")) {
        if (fgets(comm, sizeof(comm), f))
            comm[strcspn(comm, "\n")] = 0;
        fclose(f);
    }
    // SteamVR's processes: the known names, any "vr…" process, and anything run from the SteamVR install
    // (the Steam Frame streaming driver may encode in a helper of its own)
    static const char *const names[] = {"vrserver", "vrcompositor", "vrstartup", "vrmonitor"};
    wanted = 0;
    for (const char *n : names)
        if (!strcmp(comm, n))
            wanted = 1;
    if (!strncmp(comm, "vr", 2))
        wanted = 1;
    char exe[512] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0 && (strstr(exe, "/SteamVR/") || strstr(exe, "/steamvr/")))
        wanted = 1;
    if (wanted || getenv("VENC_LOG"))
        LOGI("process %s (%s): %s", comm, n > 0 ? exe : "?", wanted ? "encoding" : "pass-through");
    return wanted;
}

// ------------------------------------------------------------------------------------------------- backend
const venc_backend_ops *g_backend;
std::once_flag g_backend_once;

void load_backend()
{
    std::call_once(g_backend_once, [] {
        Dl_info di{};
        std::string dir;
        if (dladdr((void *)&load_backend, &di) && di.dli_fname) {
            dir = di.dli_fname;
            size_t s = dir.rfind('/');
            dir = s == std::string::npos ? std::string() : dir.substr(0, s + 1);
        }
        std::vector<std::string> names;
        if (const char *e = getenv("VENC_BACKEND"))
            names.push_back(e);
        else
            names = {"vt", "x26x"};
        for (auto &n : names) {
            std::string path = n.find('/') != std::string::npos ? n : dir + "libvenc_backend_" + n + ".so";
            void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h) {
                LOGI("backend %s: %s", path.c_str(), dlerror());
                continue;
            }
            auto get = (venc_backend_get_fn)dlsym(h, "venc_backend_get");
            const venc_backend_ops *ops = get ? get() : nullptr;
            if (!ops || ops->abi != VENC_ABI) {
                LOGE("backend %s: wrong ABI", path.c_str());
                dlclose(h);
                continue;
            }
            venc_limits l;
            int q264 = ops->query(VENC_H264, &l), q265 = ops->query(VENC_H265, &l);
            LOGI("backend %s: H.264 %d, H.265 %d (0 = available, else -errno)", ops->name, q264, q265);
            if (q264 != 0 && q265 != 0) {
                LOGI("backend %s: no codec available", ops->name);
                continue;  // keep it loaded: a backend may hold process-wide state
            }
            g_backend = ops;
            LOGI("encoder backend: %s (%s)", ops->name, path.c_str());
            return;
        }
        LOGI("no encoder backend: the layer stays inactive");
    });
}

// ------------------------------------------------------------------------------------------------- pNext helpers
const void *find_struct(const void *chain, VkStructureType t)
{
    for (auto *p = (const VkBaseInStructure *)chain; p; p = p->pNext)
        if (p->sType == t)
            return p;
    return nullptr;
}
void *find_struct_out(void *chain, VkStructureType t)
{
    for (auto *p = (VkBaseOutStructure *)chain; p; p = p->pNext)
        if (p->sType == t)
            return p;
    return nullptr;
}

// Temporarily removes structures from an application's pNext chain (restored by the destructor, in reverse).
struct Unlinker {
    struct Cut { VkBaseOutStructure *prev, *node; };
    std::vector<Cut> cuts;
    void cut(const void *head, std::initializer_list<VkStructureType> types)
    {
        auto *p = (VkBaseOutStructure *)head;
        while (p && p->pNext) {
            bool hit = false;
            for (auto t : types)
                hit |= p->pNext->sType == t;
            if (hit) {
                cuts.push_back({p, p->pNext});
                p->pNext = p->pNext->pNext;
            } else {
                p = p->pNext;
            }
        }
    }
    ~Unlinker()
    {
        for (auto it = cuts.rbegin(); it != cuts.rend(); ++it)
            it->prev->pNext = it->node;
    }
};

const std::initializer_list<VkStructureType> kVideoInfoTypes = {
    VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
    VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
    VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR,
    VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR,
    VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR,
};

// ------------------------------------------------------------------------------------------------- usage / sync rewriting
constexpr VkImageUsageFlags kImgVideo = VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                                        VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                                        VK_IMAGE_USAGE_VIDEO_DECODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
constexpr VkBufferUsageFlags kBufVideo = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_BUFFER_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                                         VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR | VK_BUFFER_USAGE_VIDEO_DECODE_DST_BIT_KHR;

VkImageUsageFlags map_image_usage(VkImageUsageFlags u)
{
    if (!(u & kImgVideo))
        return u;
    VkImageUsageFlags r = u & ~kImgVideo;
    if (u & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR)
        r |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // the layer copies source pictures out
    if (!r)
        r = VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // DPB images: never touched, any valid usage
    return r;
}
VkBufferUsageFlags map_buffer_usage(VkBufferUsageFlags u)
{
    if (!(u & kBufVideo))
        return u;
    VkBufferUsageFlags r = u & ~kBufVideo;
    if (u & VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR)
        r |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;  // the layer writes the bitstream with a copy
    if (!r)
        r = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    return r;
}
VkImageLayout map_layout(VkImageLayout l)
{
    switch (l) {
    case VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR:
    case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DST_KHR:
    case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR:
    case VK_IMAGE_LAYOUT_VIDEO_DECODE_SRC_KHR:
    case VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR:
    case VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR:
        return VK_IMAGE_LAYOUT_GENERAL;
    default:
        return l;
    }
}
constexpr VkPipelineStageFlags2 kStageVideo = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR | VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
constexpr VkAccessFlags2 kAccessVideo = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR |
                                        VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
VkPipelineStageFlags2 map_stage2(VkPipelineStageFlags2 s)
{
    return (s & kStageVideo) ? ((s & ~kStageVideo) | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) : s;
}
VkAccessFlags2 map_access2(VkAccessFlags2 a)
{
    if (!(a & kAccessVideo))
        return a;
    VkAccessFlags2 r = a & ~kAccessVideo;
    if (a & (VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR))
        r |= VK_ACCESS_2_MEMORY_READ_BIT;
    if (a & (VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR))
        r |= VK_ACCESS_2_MEMORY_WRITE_BIT;
    return r;
}
// sync2 -> sync1 (for the layer's own command buffers and for drivers without synchronization2)
VkPipelineStageFlags stage1(VkPipelineStageFlags2 s)
{
    s = map_stage2(s);
    VkPipelineStageFlags r = (VkPipelineStageFlags)(s & 0x7fffffffull);
    if (s >> 31)
        r |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (s == VK_PIPELINE_STAGE_2_NONE)
        r = 0;
    return r;
}
VkAccessFlags access1(VkAccessFlags2 a)
{
    a = map_access2(a);
    VkAccessFlags r = (VkAccessFlags)(a & 0x7fffffffull);
    if (a >> 31)
        r |= VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    return r;
}

// ------------------------------------------------------------------------------------------------- dispatch
inline void *dkey(const void *h) { return *(void **)h; }

struct Instance {
    VkInstance handle{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkDestroyInstance DestroyInstance{};
    PFN_vkCreateDevice CreateDevice{};
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 GetPhysicalDeviceQueueFamilyProperties2{};
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2{};
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties{};
    PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2{};
    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2{};
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties{};
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties{};
};

struct Phys {
    Instance *inst{};
    bool active{};
    uint32_t nfam{};          // driver queue families; the video-encode family is index nfam
    bool h264{}, h265{};
    venc_limits l264{}, l265{};
    VkPhysicalDeviceMemoryProperties mem{};
};

// Process-lifetime globals, never destroyed: applications may exit without vkDestroyDevice, and the worker
// threads of a live device must not see their state destructed by exit handlers.
std::shared_mutex &g_lock = *new std::shared_mutex;
std::unordered_map<void *, std::unique_ptr<Instance>> &g_instances = *new std::unordered_map<void *, std::unique_ptr<Instance>>;
std::unordered_map<VkPhysicalDevice, Phys> &g_phys = *new std::unordered_map<VkPhysicalDevice, Phys>;

Instance *inst_of(const void *h)
{
    std::shared_lock l(g_lock);
    auto it = g_instances.find(dkey(h));
    return it == g_instances.end() ? nullptr : it->second.get();
}

const VkExtensionProperties kExts[] = {
    {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_QUEUE_SPEC_VERSION},
    {VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_QUEUE_SPEC_VERSION},
    {VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_H264_SPEC_VERSION},
    {VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_H265_SPEC_VERSION},
    {VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME, VK_KHR_VIDEO_MAINTENANCE_1_SPEC_VERSION},
};
bool is_our_ext(const char *n)
{
    for (auto &e : kExts)
        if (!strcmp(e.extensionName, n))
            return true;
    return false;
}

Phys &phys_of(VkPhysicalDevice pd)
{
    {
        std::shared_lock l(g_lock);
        auto it = g_phys.find(pd);
        if (it != g_phys.end())
            return it->second;
    }
    Phys p;
    p.inst = inst_of(pd);
    if (process_wanted())
        load_backend();
    if (p.inst) {
        p.inst->GetPhysicalDeviceQueueFamilyProperties(pd, &p.nfam, nullptr);
        p.inst->GetPhysicalDeviceMemoryProperties(pd, &p.mem);
        uint32_t n = 0;
        p.inst->EnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> ex(n);
        p.inst->EnumerateDeviceExtensionProperties(pd, nullptr, &n, ex.data());
        bool driver_video = false;
        for (auto &e : ex)
            driver_video |= !strcmp(e.extensionName, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
        if (g_backend && !driver_video && process_wanted()) {
            p.h264 = g_backend->query(VENC_H264, &p.l264) == 0;
            p.h265 = g_backend->query(VENC_H265, &p.l265) == 0;
            p.active = p.h264 || p.h265;
        }
        if (process_wanted())
            LOGI("physical device: %s (H.264 %d, H.265 %d, %u queue families, backend %s)",
                 p.active ? "video encode offered" : "no video encode", p.h264, p.h265, p.nfam,
                 g_backend ? g_backend->name : "none");
        if (driver_video)
            LOGI("driver has its own video queues: layer inactive");
    }
    std::unique_lock l(g_lock);
    return g_phys.emplace(pd, p).first->second;
}

// ------------------------------------------------------------------------------------------------- device objects
struct Device;

struct ImageInfo {
    VkFormat format;
    VkExtent3D extent;
    uint32_t layers;
};
struct ViewInfo {
    VkImage image;
    VkFormat format;
    uint32_t base_layer;
};

struct QuerySlot {
    bool avail = false;
    int32_t status = 0;      // VkQueryResultStatusKHR
    uint64_t offset = 0, bytes = 0;
    uint64_t overrides = 0;
};
struct QueryPool {
    VkQueryType type;
    VkVideoEncodeFeedbackFlagsKHR feedback = 0;
    std::vector<QuerySlot> slots;
};

struct ParamSummary {   // what the layer needs from the application's parameter sets
    uint32_t width = 0, height = 0, profile_idc = 0, bit_depth = 8, cabac = 1, fps_num = 0, fps_den = 0;
};
// Deep copies of the application's H.264 parameter sets (the Std structures point at arrays).
struct H264SpsStore {
    StdVideoH264SequenceParameterSet sps{};
    StdVideoH264SequenceParameterSetVui vui{};
    StdVideoH264HrdParameters hrd{};
    StdVideoH264ScalingLists sl{};
    std::vector<int32_t> offsets;
    explicit H264SpsStore(const StdVideoH264SequenceParameterSet &s) : sps(s)
    {
        if (s.pSequenceParameterSetVui) {
            vui = *s.pSequenceParameterSetVui;
            if (vui.pHrdParameters)
                hrd = *vui.pHrdParameters;
            vui.pHrdParameters = s.pSequenceParameterSetVui->pHrdParameters ? &hrd : nullptr;
            sps.pSequenceParameterSetVui = &vui;
        }
        if (s.pScalingLists) {
            sl = *s.pScalingLists;
            sps.pScalingLists = &sl;
        }
        if (s.pOffsetForRefFrame && s.num_ref_frames_in_pic_order_cnt_cycle) {
            offsets.assign(s.pOffsetForRefFrame, s.pOffsetForRefFrame + s.num_ref_frames_in_pic_order_cnt_cycle);
            sps.pOffsetForRefFrame = offsets.data();
        }
    }
    H264SpsStore(const H264SpsStore &) = delete;
};
struct H264PpsStore {
    StdVideoH264PictureParameterSet pps{};
    StdVideoH264ScalingLists sl{};
    explicit H264PpsStore(const StdVideoH264PictureParameterSet &p) : pps(p)
    {
        if (p.pScalingLists) {
            sl = *p.pScalingLists;
            pps.pScalingLists = &sl;
        }
    }
    H264PpsStore(const H264PpsStore &) = delete;
};

// Deep copies of the application's H.265 parameter sets.
struct H265VpsStore {
    StdVideoH265VideoParameterSet vps{};
    StdVideoH265DecPicBufMgr dpb{};
    StdVideoH265ProfileTierLevel ptl{};
    explicit H265VpsStore(const StdVideoH265VideoParameterSet &v) : vps(v)
    {
        if (v.pDecPicBufMgr) { dpb = *v.pDecPicBufMgr; vps.pDecPicBufMgr = &dpb; }
        if (v.pProfileTierLevel) { ptl = *v.pProfileTierLevel; vps.pProfileTierLevel = &ptl; }
        vps.pHrdParameters = nullptr;
    }
    H265VpsStore(const H265VpsStore &) = delete;
};
struct H265SpsStore {
    StdVideoH265SequenceParameterSet sps{};
    StdVideoH265ProfileTierLevel ptl{};
    StdVideoH265DecPicBufMgr dpb{};
    StdVideoH265SequenceParameterSetVui vui{};
    StdVideoH265LongTermRefPicsSps lt{};
    std::vector<StdVideoH265ShortTermRefPicSet> rps;
    explicit H265SpsStore(const StdVideoH265SequenceParameterSet &s) : sps(s)
    {
        if (s.pProfileTierLevel) { ptl = *s.pProfileTierLevel; sps.pProfileTierLevel = &ptl; }
        if (s.pDecPicBufMgr) { dpb = *s.pDecPicBufMgr; sps.pDecPicBufMgr = &dpb; }
        if (s.pSequenceParameterSetVui) { vui = *s.pSequenceParameterSetVui; vui.pHrdParameters = nullptr; sps.pSequenceParameterSetVui = &vui; }
        if (s.pLongTermRefPicsSps) { lt = *s.pLongTermRefPicsSps; sps.pLongTermRefPicsSps = &lt; }
        if (s.pShortTermRefPicSet && s.num_short_term_ref_pic_sets) {
            rps.assign(s.pShortTermRefPicSet, s.pShortTermRefPicSet + s.num_short_term_ref_pic_sets);
            sps.pShortTermRefPicSet = rps.data();
        } else {
            sps.pShortTermRefPicSet = nullptr;
        }
        sps.pScalingLists = nullptr;
        sps.pPredictorPaletteEntries = nullptr;
    }
    H265SpsStore(const H265SpsStore &) = delete;
};
struct H265PpsStore {
    StdVideoH265PictureParameterSet pps{};
    explicit H265PpsStore(const StdVideoH265PictureParameterSet &p) : pps(p)
    {
        pps.pScalingLists = nullptr;
        pps.pPredictorPaletteEntries = nullptr;
    }
    H265PpsStore(const H265PpsStore &) = delete;
};

struct SessionParams {
    uint32_t codec;
    std::map<uint32_t, ParamSummary> sps;   // H.264/H.265 SPS id -> summary
    std::map<uint32_t, uint32_t> pps_cabac; // H.264 PPS id -> entropy_coding_mode_flag
    std::map<uint32_t, std::shared_ptr<H264SpsStore>> h264_sps;
    std::map<uint32_t, std::shared_ptr<H264PpsStore>> h264_pps;
    std::map<uint32_t, std::shared_ptr<H265VpsStore>> h265_vps;
    std::map<uint32_t, std::shared_ptr<H265SpsStore>> h265_sps;
    std::map<uint32_t, std::shared_ptr<H265PpsStore>> h265_pps;
    void *probe = nullptr;                  // backend encoder used only for parameter sets
    venc_config probe_cfg{};
};
struct Session {
    uint32_t codec;
    VkExtent2D max_extent;
    VkFormat picture_format;
    uint32_t profile_idc, bit_depth;
    bool inline_queries;
    // state at execution time (worker thread only)
    void *enc = nullptr;
    venc_config cfg{};
    bool have_rc = false;
    venc_config rc{};       // rate control fields only
    bool reset_pending = true;
    // the backend's own H.264 parameter sets, for rewriting its slices
    venc::Sps enc_sps;
    venc::Pps enc_pps;
    venc::h265::Sps enc_sps5;
    venc::h265::Pps enc_pps5;
    venc::h265::PocMap pocs;
    bool enc_parsed = false;
    std::string warned;
};

struct Op {
    enum Kind { Barrier, ResetQuery, BeginQuery, EndQuery, BeginCoding, Control, Encode, EndCoding } kind;
    // barriers (sync2 form, pNext dropped)
    std::vector<VkMemoryBarrier2> mem;
    std::vector<VkBufferMemoryBarrier2> buf;
    std::vector<VkImageMemoryBarrier2> img;
    // queries
    QueryPool *pool = nullptr;
    uint32_t first = 0, count = 0;
    // coding
    Session *session = nullptr;
    SessionParams *params = nullptr;
    VkVideoCodingControlFlagsKHR ctrl = 0;
    bool has_rc = false;
    venc_config rc{};
    // encode
    VkImage src = VK_NULL_HANDLE;
    VkFormat src_format = VK_FORMAT_UNDEFINED;
    uint32_t src_layer = 0;
    VkOffset2D off{};
    VkExtent2D ext{};
    VkBuffer dst = VK_NULL_HANDLE;
    VkDeviceSize dst_offset = 0, dst_range = 0;
    bool force_idr = false;
    uint32_t sps_id = 0, pps_id = 0;
    venc::PicInfo pic;
    QueryPool *inline_pool = nullptr;
    uint32_t inline_query = 0;
};

struct CmdPool;
struct FakeCmd {
    void *loader_data;   // dispatchable object: the loader writes its dispatch pointer here
    CmdPool *pool;
    std::vector<Op> ops;
    bool recording = false;
};
struct CmdPool {
    VkCommandPoolCreateFlags flags;
    std::set<FakeCmd *> cmds;
};

struct Job {
    struct Batch {
        std::vector<VkSemaphoreSubmitInfo> waits, signals;
        std::vector<FakeCmd *> cmds;
        std::vector<std::vector<Op>> snapshot;   // ops copied at submit time
    };
    std::vector<Batch> batches;
    VkFence fence = VK_NULL_HANDLE;
};

struct FakeQueue {
    void *loader_data;
    Device *dev;
    uint32_t index;
    std::thread worker;
    std::mutex m;
    std::condition_variable cv, idle_cv;
    std::deque<Job> jobs;
    bool busy = false, quit = false;
    // worker-owned
    VkCommandBuffer cb1 = VK_NULL_HANDLE, cb2 = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    struct Staging {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        uint8_t *ptr = nullptr;
        bool coherent = true;
        uint64_t handle = 0;   // the backend's handle of this memory (0: pointers only)
    } in, out;
};

#define DEVICE_FNS(X)                                                                                                    \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(QueueSubmit) X(QueueSubmit2) X(QueueWaitIdle)               \
    X(DeviceWaitIdle) X(QueuePresentKHR) X(QueueBindSparse) X(CreateCommandPool) X(DestroyCommandPool)                    \
    X(ResetCommandPool) X(TrimCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer)          \
    X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdPipelineBarrier) X(CmdPipelineBarrier2) X(CmdWaitEvents)               \
    X(CmdWaitEvents2) X(CmdSetEvent2) X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) X(CmdWriteTimestamp)            \
    X(CmdWriteTimestamp2) X(CmdCopyQueryPoolResults) X(CreateImage) X(CreateImageView) X(CreateBuffer) X(DestroyImage)     \
    X(DestroyImageView) X(DestroyBuffer) X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) X(ResetQueryPool)   \
    X(GetDeviceImageMemoryRequirements) X(GetDeviceBufferMemoryRequirements) X(CreateFence) X(DestroyFence)              \
    X(WaitForFences) X(ResetFences) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(InvalidateMappedMemoryRanges)           \
    X(FlushMappedMemoryRanges) X(BindBufferMemory) X(GetBufferMemoryRequirements) X(CmdCopyImageToBuffer)                 \
    X(CmdCopyBuffer) X(WaitSemaphores) X(BindImageMemory2) X(GetMemoryFdKHR) X(SetDebugUtilsObjectNameEXT) X(SetDebugUtilsObjectTagEXT) X(CmdBeginDebugUtilsLabelEXT)             \
    X(CmdEndDebugUtilsLabelEXT) X(CmdInsertDebugUtilsLabelEXT) X(QueueBeginDebugUtilsLabelEXT)                           \
    X(QueueEndDebugUtilsLabelEXT) X(QueueInsertDebugUtilsLabelEXT)

struct Device {
    VkDevice handle{};
    VkPhysicalDevice pd{};
    Phys *phys{};
    PFN_vkGetDeviceProcAddr gdpa{};
#define DECL(n) PFN_vk##n n{};
    DEVICE_FNS(DECL)
#undef DECL
    bool active = false;
    uint32_t fake_family = 0;
    int export_type = 0;   // 0 none, 1 dma-buf, 2 opaque fd: staging memory shared with the backend
    // the real queue the workers use
    uint32_t work_family = 0, work_index = 0;
    VkQueue work_queue = VK_NULL_HANDLE;
    bool work_shared = false;      // also used by the application: serialize submissions
    VkCommandPool work_pool = VK_NULL_HANDLE;
    std::mutex work_pool_lock;
    std::vector<std::unique_ptr<FakeQueue>> queues;
    // per-queue locks for shared real queues
    std::mutex queue_lock;
    // objects
    std::shared_mutex objs;
    std::unordered_map<VkImage, ImageInfo> images;
    std::unordered_map<VkImageView, ViewInfo> views;
    std::unordered_set<VkCommandPool> fake_pools;
    std::unordered_set<void *> fake_cmds;
    std::unordered_set<VkQueryPool> fake_query_pools;
    std::unordered_set<uint64_t> fake_sessions;      // sessions and session parameters
    std::mutex query_m;
    std::condition_variable query_cv;
};

std::unordered_map<void *, std::unique_ptr<Device>> &g_devices = *new std::unordered_map<void *, std::unique_ptr<Device>>;  // by dispatch key

Device *dev_of(const void *h)
{
    std::shared_lock l(g_lock);
    auto it = g_devices.find(dkey(h));
    return it == g_devices.end() ? nullptr : it->second.get();
}

bool is_fake_cmd(Device *d, VkCommandBuffer cb)
{
    if (!d->active)
        return false;
    std::shared_lock l(d->objs);
    return d->fake_cmds.count((void *)cb) != 0;
}
FakeQueue *fake_queue(Device *d, VkQueue q)
{
    if (!d->active)
        return nullptr;
    for (auto &f : d->queues)
        if ((void *)f.get() == (void *)q)
            return f.get();
    return nullptr;
}
QueryPool *fake_qpool(Device *d, VkQueryPool p)
{
    if (!d->active || p == VK_NULL_HANDLE)
        return nullptr;
    std::shared_lock l(d->objs);
    return d->fake_query_pools.count(p) ? (QueryPool *)(uintptr_t)p : nullptr;
}

// ------------------------------------------------------------------------------------------------- codec helpers
uint32_t codec_of(VkVideoCodecOperationFlagBitsKHR op)
{
    return op == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR ? VENC_H264
         : op == VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR ? VENC_H265 : 0;
}

StdVideoH264LevelIdc h264_level(uint32_t idc)
{
    static const struct { uint32_t idc; StdVideoH264LevelIdc l; } t[] = {
        {10, STD_VIDEO_H264_LEVEL_IDC_1_0}, {11, STD_VIDEO_H264_LEVEL_IDC_1_1}, {12, STD_VIDEO_H264_LEVEL_IDC_1_2},
        {13, STD_VIDEO_H264_LEVEL_IDC_1_3}, {20, STD_VIDEO_H264_LEVEL_IDC_2_0}, {21, STD_VIDEO_H264_LEVEL_IDC_2_1},
        {22, STD_VIDEO_H264_LEVEL_IDC_2_2}, {30, STD_VIDEO_H264_LEVEL_IDC_3_0}, {31, STD_VIDEO_H264_LEVEL_IDC_3_1},
        {32, STD_VIDEO_H264_LEVEL_IDC_3_2}, {40, STD_VIDEO_H264_LEVEL_IDC_4_0}, {41, STD_VIDEO_H264_LEVEL_IDC_4_1},
        {42, STD_VIDEO_H264_LEVEL_IDC_4_2}, {50, STD_VIDEO_H264_LEVEL_IDC_5_0}, {51, STD_VIDEO_H264_LEVEL_IDC_5_1},
        {52, STD_VIDEO_H264_LEVEL_IDC_5_2}, {60, STD_VIDEO_H264_LEVEL_IDC_6_0}, {61, STD_VIDEO_H264_LEVEL_IDC_6_1},
        {62, STD_VIDEO_H264_LEVEL_IDC_6_2},
    };
    StdVideoH264LevelIdc r = STD_VIDEO_H264_LEVEL_IDC_4_1;
    for (auto &e : t)
        if (e.idc <= idc)
            r = e.l;
    return r;
}
StdVideoH265LevelIdc h265_level(uint32_t idc)
{
    static const struct { uint32_t idc; StdVideoH265LevelIdc l; } t[] = {
        {30, STD_VIDEO_H265_LEVEL_IDC_1_0}, {60, STD_VIDEO_H265_LEVEL_IDC_2_0}, {63, STD_VIDEO_H265_LEVEL_IDC_2_1},
        {90, STD_VIDEO_H265_LEVEL_IDC_3_0}, {93, STD_VIDEO_H265_LEVEL_IDC_3_1}, {120, STD_VIDEO_H265_LEVEL_IDC_4_0},
        {123, STD_VIDEO_H265_LEVEL_IDC_4_1}, {150, STD_VIDEO_H265_LEVEL_IDC_5_0}, {153, STD_VIDEO_H265_LEVEL_IDC_5_1},
        {156, STD_VIDEO_H265_LEVEL_IDC_5_2}, {180, STD_VIDEO_H265_LEVEL_IDC_6_0}, {183, STD_VIDEO_H265_LEVEL_IDC_6_1},
        {186, STD_VIDEO_H265_LEVEL_IDC_6_2},
    };
    StdVideoH265LevelIdc r = STD_VIDEO_H265_LEVEL_IDC_4_1;
    for (auto &e : t)
        if (e.idc <= idc)
            r = e.l;
    return r;
}

// Profile checks shared by the capability queries. Returns the codec, 0 if the profile is not ours.
uint32_t check_profile(Phys &p, const VkVideoProfileInfoKHR *prof, uint32_t *bit_depth)
{
    if (!prof)
        return 0;
    uint32_t codec = codec_of(prof->videoCodecOperation);
    if (!codec || (codec == VENC_H264 && !p.h264) || (codec == VENC_H265 && !p.h265))
        return 0;
    if (prof->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)
        return 0;
    if (prof->lumaBitDepth != prof->chromaBitDepth)
        return 0;
    uint32_t bd = prof->lumaBitDepth == VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR ? 8
                : prof->lumaBitDepth == VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR ? 10 : 0;
    if (bd == 0 || (bd == 10 && (codec != VENC_H265 || !p.l265.ten_bit)))
        return 0;
    if (codec == VENC_H264) {
        auto *h = (const VkVideoEncodeH264ProfileInfoKHR *)find_struct(prof->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR);
        if (h && h->stdProfileIdc != STD_VIDEO_H264_PROFILE_IDC_BASELINE && h->stdProfileIdc != STD_VIDEO_H264_PROFILE_IDC_MAIN &&
            h->stdProfileIdc != STD_VIDEO_H264_PROFILE_IDC_HIGH)
            return 0;
    } else {
        auto *h = (const VkVideoEncodeH265ProfileInfoKHR *)find_struct(prof->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR);
        if (h && h->stdProfileIdc != STD_VIDEO_H265_PROFILE_IDC_MAIN && !(bd == 10 && h->stdProfileIdc == STD_VIDEO_H265_PROFILE_IDC_MAIN_10))
            return 0;
    }
    if (bit_depth)
        *bit_depth = bd;
    return codec;
}

VkFormat picture_format(uint32_t bit_depth)
{
    return bit_depth == 10 ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
}

// ================================================================================================= instance hooks
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out)
{
    auto *li = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
        li = (VkLayerInstanceCreateInfo *)li->pNext;
    if (!li)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    li->u.pLayerInfo = li->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = create(ci, a, out);
    if (r != VK_SUCCESS)
        return r;
    auto in = std::make_unique<Instance>();
    in->handle = *out;
    in->gipa = gipa;
#define L(n) in->n = (PFN_vk##n)gipa(*out, "vk" #n);
    L(DestroyInstance) L(CreateDevice) L(EnumerateDeviceExtensionProperties) L(GetPhysicalDeviceQueueFamilyProperties)
    L(GetPhysicalDeviceQueueFamilyProperties2) L(GetPhysicalDeviceFeatures2) L(GetPhysicalDeviceFormatProperties)
    L(GetPhysicalDeviceFormatProperties2) L(GetPhysicalDeviceImageFormatProperties2) L(GetPhysicalDeviceMemoryProperties)
    L(GetPhysicalDeviceProperties)
#undef L
    if (!in->GetPhysicalDeviceQueueFamilyProperties2)
        in->GetPhysicalDeviceQueueFamilyProperties2 = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)gipa(*out, "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
    if (!in->GetPhysicalDeviceFeatures2)
        in->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2KHR");
    if (!in->GetPhysicalDeviceFormatProperties2)
        in->GetPhysicalDeviceFormatProperties2 = (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(*out, "vkGetPhysicalDeviceFormatProperties2KHR");
    if (!in->GetPhysicalDeviceImageFormatProperties2)
        in->GetPhysicalDeviceImageFormatProperties2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(*out, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    std::unique_lock l(g_lock);
    g_instances[dkey(*out)] = std::move(in);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance inst, const VkAllocationCallbacks *a)
{
    Instance *in = inst_of(inst);
    if (!in)
        return;
    auto destroy = in->DestroyInstance;
    {
        std::unique_lock l(g_lock);
        for (auto it = g_phys.begin(); it != g_phys.end();)
            it = it->second.inst == in ? g_phys.erase(it) : std::next(it);
        g_instances.erase(dkey(inst));
    }
    destroy(inst, a);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count,
                                                                  VkExtensionProperties *props)
{
    Phys &p = phys_of(pd);
    const uint32_t nours = sizeof(kExts) / sizeof(kExts[0]);
    if (layer && !strcmp(layer, LAYER_NAME)) {
        uint32_t n = p.active ? nours : 0;
        if (!props) {
            *count = n;
            return VK_SUCCESS;
        }
        uint32_t w = std::min(*count, n);
        for (uint32_t i = 0; i < w; i++)
            props[i] = kExts[i];
        *count = w;
        return w < n ? VK_INCOMPLETE : VK_SUCCESS;
    }
    if (layer || !p.active)
        return p.inst->EnumerateDeviceExtensionProperties(pd, layer, count, props);
    uint32_t n = 0;
    VkResult r = p.inst->EnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    if (r != VK_SUCCESS)
        return r;
    std::vector<VkExtensionProperties> all(n);
    r = p.inst->EnumerateDeviceExtensionProperties(pd, nullptr, &n, all.data());
    if (r < 0)
        return r;
    all.resize(n);
    if (const char *hide = getenv("VENC_HIDE_EXT"))   // tests: hide a driver extension
        all.erase(std::remove_if(all.begin(), all.end(), [&](const VkExtensionProperties &e) { return strstr(hide, e.extensionName); }), all.end());
    for (auto &e : kExts) {
        bool have = false;
        for (auto &x : all)
            have |= !strcmp(x.extensionName, e.extensionName);
        if (!have && (strcmp(e.extensionName, VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME) || p.h264) &&
            (strcmp(e.extensionName, VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME) || p.h265))
            all.push_back(e);
    }
    if (!props) {
        *count = (uint32_t)all.size();
        return VK_SUCCESS;
    }
    uint32_t w = std::min<uint32_t>(*count, (uint32_t)all.size());
    memcpy(props, all.data(), w * sizeof(VkExtensionProperties));
    *count = w;
    return w < all.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

void fill_fake_family(Phys &p, VkQueueFamilyProperties &q)
{
    q = {};
    q.queueFlags = VK_QUEUE_VIDEO_ENCODE_BIT_KHR;
    q.queueCount = 1;
    q.timestampValidBits = 0;
    q.minImageTransferGranularity = {1, 1, 1};
    (void)p;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties *props)
{
    Phys &p = phys_of(pd);
    if (!p.active)
        return p.inst->GetPhysicalDeviceQueueFamilyProperties(pd, count, props);
    if (!props) {
        *count = p.nfam + 1;
        return;
    }
    uint32_t n = std::min(*count, p.nfam);
    p.inst->GetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    if (*count > p.nfam) {
        fill_fake_family(p, props[p.nfam]);
        *count = p.nfam + 1;
    } else {
        *count = n;
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties2 *props)
{
    Phys &p = phys_of(pd);
    if (!p.active)
        return p.inst->GetPhysicalDeviceQueueFamilyProperties2(pd, count, props);
    if (!props) {
        *count = p.nfam + 1;
        return;
    }
    uint32_t n = std::min(*count, p.nfam);
    p.inst->GetPhysicalDeviceQueueFamilyProperties2(pd, &n, props);
    uint32_t total = *count > p.nfam ? p.nfam + 1 : n;
    for (uint32_t i = 0; i < total; i++) {
        bool fake = i == p.nfam;
        if (fake)
            fill_fake_family(p, props[i].queueFamilyProperties);
        if (auto *v = (VkQueueFamilyVideoPropertiesKHR *)find_struct_out(props[i].pNext, VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR))
            v->videoCodecOperations = fake ? ((p.h264 ? VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR : 0) |
                                              (p.h265 ? VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR : 0))
                                           : 0;
        if (auto *s = (VkQueueFamilyQueryResultStatusPropertiesKHR *)find_struct_out(props[i].pNext, VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR))
            s->queryResultStatusSupport = fake ? VK_TRUE : VK_FALSE;
    }
    *count = total;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    Phys &p = phys_of(pd);
    p.inst->GetPhysicalDeviceFeatures2(pd, f);
    if (!p.active)
        return;
    if (auto *m = (VkPhysicalDeviceVideoMaintenance1FeaturesKHR *)find_struct_out(f->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR))
        m->videoMaintenance1 = VK_TRUE;
}

constexpr VkFormatFeatureFlags kFmtVideo1 = VK_FORMAT_FEATURE_VIDEO_ENCODE_INPUT_BIT_KHR | VK_FORMAT_FEATURE_VIDEO_ENCODE_DPB_BIT_KHR;
constexpr VkFormatFeatureFlags2 kFmtVideo2 = VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT_BIT_KHR | VK_FORMAT_FEATURE_2_VIDEO_ENCODE_DPB_BIT_KHR;

bool format_is_picture(Phys &p, VkFormat f)
{
    return f == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM || (p.l265.ten_bit && f == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat f, VkFormatProperties *out)
{
    Phys &p = phys_of(pd);
    p.inst->GetPhysicalDeviceFormatProperties(pd, f, out);
    if (p.active && format_is_picture(p, f) && (out->optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        out->optimalTilingFeatures |= kFmtVideo1;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2 *out)
{
    Phys &p = phys_of(pd);
    p.inst->GetPhysicalDeviceFormatProperties2(pd, f, out);
    if (!p.active || !format_is_picture(p, f) || !(out->formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        return;
    out->formatProperties.optimalTilingFeatures |= kFmtVideo1;
    if (auto *f3 = (VkFormatProperties3 *)find_struct_out(out->pNext, VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3))
        f3->optimalTilingFeatures |= kFmtVideo2;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                       VkImageFormatProperties2 *out)
{
    Phys &p = phys_of(pd);
    if (!p.active || !(info->usage & kImgVideo))
        return p.inst->GetPhysicalDeviceImageFormatProperties2(pd, info, out);
    if (!format_is_picture(p, info->format))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkPhysicalDeviceImageFormatInfo2 copy = *info;
    copy.usage = map_image_usage(info->usage);
    copy.flags &= ~VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
    Unlinker u;
    u.cut(&copy, kVideoInfoTypes);
    return p.inst->GetPhysicalDeviceImageFormatProperties2(pd, &copy, out);
}

// ------------------------------------------------------------------------------------------------- video capability queries
VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceVideoCapabilitiesKHR(VkPhysicalDevice pd, const VkVideoProfileInfoKHR *prof,
                                                                     VkVideoCapabilitiesKHR *caps)
{
    Phys &p = phys_of(pd);
    uint32_t bd = 8;
    uint32_t codec = p.active ? check_profile(p, prof, &bd) : 0;
    if (!codec)
        return prof && codec_of(prof->videoCodecOperation) ? VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR
                                                           : VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
    const venc_limits &l = codec == VENC_H264 ? p.l264 : p.l265;
    caps->flags = VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR;
    caps->minBitstreamBufferOffsetAlignment = 1;
    caps->minBitstreamBufferSizeAlignment = 1;
    caps->pictureAccessGranularity = {16, 16};
    caps->minCodedExtent = {std::max(16u, l.min_width), std::max(16u, l.min_height)};
    caps->maxCodedExtent = {l.max_width, l.max_height};
    caps->maxDpbSlots = 16;
    caps->maxActiveReferencePictures = 1;
    if (codec == VENC_H264)
        caps->stdHeaderVersion = {VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION};
    else
        caps->stdHeaderVersion = {VK_STD_VULKAN_VIDEO_CODEC_H265_ENCODE_EXTENSION_NAME, VK_STD_VULKAN_VIDEO_CODEC_H265_ENCODE_SPEC_VERSION};
    if (auto *e = (VkVideoEncodeCapabilitiesKHR *)find_struct_out(caps->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR)) {
        e->flags = VK_VIDEO_ENCODE_CAPABILITY_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_DETECTION_BIT_KHR;
        e->rateControlModes = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR | VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR |
                              VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR | VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
        e->maxRateControlLayers = 1;
        e->maxBitrate = l.max_bitrate;
        e->maxQualityLevels = 1;
        e->encodeInputPictureGranularity = {16, 16};
        e->supportedEncodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
                                          VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR |
                                          VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_HAS_OVERRIDES_BIT_KHR;
    }
    if (auto *h = (VkVideoEncodeH264CapabilitiesKHR *)find_struct_out(caps->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR)) {
        h->flags = 0;
        h->maxLevelIdc = h264_level(l.max_level_idc);
        h->maxSliceCount = 64;      // accepted, encoded as one slice (see the H.265 note below)
        h->maxPPictureL0ReferenceCount = 1;
        h->maxBPictureL0ReferenceCount = 0;
        h->maxL1ReferenceCount = 0;
        h->maxTemporalLayerCount = 1;
        h->expectDyadicTemporalLayerPattern = VK_FALSE;
        h->minQp = 0;
        h->maxQp = 51;
        h->prefersGopRemainingFrames = VK_FALSE;
        h->requiresGopRemainingFrames = VK_FALSE;
        h->stdSyntaxFlags = 0;
    }
    if (auto *h = (VkVideoEncodeH265CapabilitiesKHR *)find_struct_out(caps->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR)) {
        h->flags = 0;
        h->maxLevelIdc = h265_level(l.max_level_idc);
        // SteamVR's Steam Frame streaming (vrlink) asks for one slice segment per atlas view (4); VideoToolbox makes
        // one slice per picture. Any count is accepted and the picture comes out as a single slice, which every
        // decoder takes (reported as overridden, VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_HAS_OVERRIDES_BIT_KHR).
        h->maxSliceSegmentCount = 64;
        h->maxTiles = {1, 1};
        h->ctbSizes = VK_VIDEO_ENCODE_H265_CTB_SIZE_16_BIT_KHR | VK_VIDEO_ENCODE_H265_CTB_SIZE_32_BIT_KHR |
                      VK_VIDEO_ENCODE_H265_CTB_SIZE_64_BIT_KHR;
        h->transformBlockSizes = VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_4_BIT_KHR | VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_8_BIT_KHR |
                                 VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_16_BIT_KHR | VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_32_BIT_KHR;
        h->maxPPictureL0ReferenceCount = 1;
        h->maxBPictureL0ReferenceCount = 0;
        h->maxL1ReferenceCount = 0;
        h->maxSubLayerCount = 1;
        h->expectDyadicTemporalSubLayerPattern = VK_FALSE;
        h->minQp = 0;
        h->maxQp = 51;
        h->prefersGopRemainingFrames = VK_FALSE;
        h->requiresGopRemainingFrames = VK_FALSE;
        h->stdSyntaxFlags = 0;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceVideoFormatPropertiesKHR(VkPhysicalDevice pd, const VkPhysicalDeviceVideoFormatInfoKHR *info,
                                                                         uint32_t *count, VkVideoFormatPropertiesKHR *props)
{
    Phys &p = phys_of(pd);
    auto *list = (const VkVideoProfileListInfoKHR *)find_struct(info->pNext, VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR);
    uint32_t bd = 8;
    if (!p.active || !list || !list->profileCount)
        return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
    for (uint32_t i = 0; i < list->profileCount; i++)
        if (!check_profile(p, &list->pProfiles[i], &bd))
            return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
    if (info->imageUsage & ~(VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR |
                             VK_IMAGE_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR;
    if (!(info->imageUsage & (VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR)))
        return VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR;
    VkFormat fmt = picture_format(bd);
    VkFormatProperties fp{};
    p.inst->GetPhysicalDeviceFormatProperties(pd, fmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkImageUsageFlags usage = info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (!props) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count < 1)
        return VK_INCOMPLETE;
    props[0].format = fmt;
    props[0].componentMapping = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                 VK_COMPONENT_SWIZZLE_IDENTITY};
    props[0].imageCreateFlags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT |
                                VK_IMAGE_CREATE_DISJOINT_BIT | VK_IMAGE_CREATE_ALIAS_BIT;
    props[0].imageType = VK_IMAGE_TYPE_2D;
    props[0].imageTiling = VK_IMAGE_TILING_OPTIMAL;
    props[0].imageUsageFlags = usage;
    *count = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR(VkPhysicalDevice pd,
                                                                                     const VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR *info,
                                                                                     VkVideoEncodeQualityLevelPropertiesKHR *out)
{
    Phys &p = phys_of(pd);
    uint32_t codec = p.active ? check_profile(p, info->pVideoProfile, nullptr) : 0;
    if (!codec)
        return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
    if (info->qualityLevel != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    out->preferredRateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
    out->preferredRateControlLayerCount = 1;
    if (auto *h = (VkVideoEncodeH264QualityLevelPropertiesKHR *)find_struct_out(out->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUALITY_LEVEL_PROPERTIES_KHR)) {
        h->preferredRateControlFlags = VK_VIDEO_ENCODE_H264_RATE_CONTROL_REGULAR_GOP_BIT_KHR;
        h->preferredGopFrameCount = 0;   // infinite: IDR on request
        h->preferredIdrPeriod = 0;
        h->preferredConsecutiveBFrameCount = 0;
        h->preferredTemporalLayerCount = 1;
        h->preferredConstantQp = {26, 26, 26};
        h->preferredMaxL0ReferenceCount = 1;
        h->preferredMaxL1ReferenceCount = 0;
        h->preferredStdEntropyCodingModeFlag = VK_TRUE;
    }
    if (auto *h = (VkVideoEncodeH265QualityLevelPropertiesKHR *)find_struct_out(out->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUALITY_LEVEL_PROPERTIES_KHR)) {
        h->preferredRateControlFlags = VK_VIDEO_ENCODE_H265_RATE_CONTROL_REGULAR_GOP_BIT_KHR;
        h->preferredGopFrameCount = 0;
        h->preferredIdrPeriod = 0;
        h->preferredConsecutiveBFrameCount = 0;
        h->preferredSubLayerCount = 1;
        h->preferredConstantQp = {28, 28, 28};
        h->preferredMaxL0ReferenceCount = 1;
        h->preferredMaxL1ReferenceCount = 0;
    }
    return VK_SUCCESS;
}

// ================================================================================================= device creation
void worker_main(FakeQueue *q);

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out)
{
    Phys &p = phys_of(pd);
    auto *li = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
        li = (VkLayerDeviceCreateInfo *)li->pNext;
    if (!li)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = li->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    li->u.pLayerInfo = li->u.pLayerInfo->pNext;
    auto create = (PFN_vkCreateDevice)gipa(p.inst->handle, "vkCreateDevice");

    // Did the application ask for video at all?
    bool want = false;
    uint32_t fake_count = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        want |= p.active && is_our_ext(ci->ppEnabledExtensionNames[i]);
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
        if (p.active && ci->pQueueCreateInfos[i].queueFamilyIndex == p.nfam)
            fake_count += ci->pQueueCreateInfos[i].queueCount;

    auto d = std::make_unique<Device>();
    d->pd = pd;
    d->phys = &p;
    d->active = want || fake_count;
    d->fake_family = p.nfam;

    VkDeviceCreateInfo c = *ci;
    std::vector<const char *> exts;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    std::vector<float> prio(64, 1.0f);
    Unlinker u;
    if (d->active) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
            if (!is_our_ext(ci->ppEnabledExtensionNames[i]))
                exts.push_back(ci->ppEnabledExtensionNames[i]);
        // memory shared with the backend (the Mac backend maps the staging memory): external memory fds
        if (g_backend->import_memory) {
            uint32_t n = 0;
            p.inst->EnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> ex(n);
            p.inst->EnumerateDeviceExtensionProperties(pd, nullptr, &n, ex.data());
            auto has = [&](const char *name) {
                for (auto &e : ex)
                    if (!strcmp(e.extensionName, name))
                        return true;
                return false;
            };
            auto enable = [&](const char *name) {
                for (auto *e : exts)
                    if (!strcmp(e, name))
                        return;
                exts.push_back(name);
            };
            if (has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
                enable(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
                if (has(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME))
                    enable(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
                if (has(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME))
                    enable(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
                if (has(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
                    enable(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
                d->export_type = 2;
                if (has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) && !getenv("VENC_OPAQUE_FD")) {
                    enable(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
                    d->export_type = 1;
                }
            }
        }
        c.enabledExtensionCount = (uint32_t)exts.size();
        c.ppEnabledExtensionNames = exts.data();
        // the worker queue: the first family with graphics or compute (both can copy images)
        std::vector<VkQueueFamilyProperties> fams(p.nfam);
        uint32_t nf = p.nfam;
        p.inst->GetPhysicalDeviceQueueFamilyProperties(pd, &nf, fams.data());
        d->work_family = 0;
        for (uint32_t i = 0; i < nf; i++)
            if (fams[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                d->work_family = i;
                break;
            }
        bool found = false;
        for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
            VkDeviceQueueCreateInfo q = ci->pQueueCreateInfos[i];
            if (q.queueFamilyIndex == p.nfam)
                continue;
            if (q.queueFamilyIndex == d->work_family) {
                found = true;
                if (q.queueCount < fams[d->work_family].queueCount && q.queueCount < prio.size()) {
                    // a queue of our own: no sharing with the application
                    std::copy(q.pQueuePriorities, q.pQueuePriorities + q.queueCount, prio.begin());
                    d->work_index = q.queueCount;
                    q.queueCount += 1;
                    q.pQueuePriorities = prio.data();
                } else {
                    d->work_index = 0;
                    d->work_shared = true;
                }
            }
            qcis.push_back(q);
        }
        if (!found) {
            VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            q.queueFamilyIndex = d->work_family;
            q.queueCount = 1;
            q.pQueuePriorities = prio.data();
            qcis.push_back(q);
            d->work_index = 0;
        }
        c.queueCreateInfoCount = (uint32_t)qcis.size();
        c.pQueueCreateInfos = qcis.data();
        u.cut(&c, {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR});
    }
    VkResult r = create(pd, &c, a, out);
    if (r != VK_SUCCESS)
        return r;

    d->handle = *out;
    d->gdpa = gdpa;
#define LOADFN(n) d->n = (PFN_vk##n)gdpa(*out, "vk" #n);
    DEVICE_FNS(LOADFN)
#undef LOADFN
    if (!d->QueueSubmit2)
        d->QueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(*out, "vkQueueSubmit2KHR");
    if (!d->CmdPipelineBarrier2)
        d->CmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)gdpa(*out, "vkCmdPipelineBarrier2KHR");
    if (!d->CmdWaitEvents2)
        d->CmdWaitEvents2 = (PFN_vkCmdWaitEvents2)gdpa(*out, "vkCmdWaitEvents2KHR");
    if (!d->CmdSetEvent2)
        d->CmdSetEvent2 = (PFN_vkCmdSetEvent2)gdpa(*out, "vkCmdSetEvent2KHR");
    if (!d->CmdWriteTimestamp2)
        d->CmdWriteTimestamp2 = (PFN_vkCmdWriteTimestamp2)gdpa(*out, "vkCmdWriteTimestamp2KHR");
    if (!d->GetDeviceImageMemoryRequirements)
        d->GetDeviceImageMemoryRequirements = (PFN_vkGetDeviceImageMemoryRequirements)gdpa(*out, "vkGetDeviceImageMemoryRequirementsKHR");
    if (!d->GetDeviceBufferMemoryRequirements)
        d->GetDeviceBufferMemoryRequirements = (PFN_vkGetDeviceBufferMemoryRequirements)gdpa(*out, "vkGetDeviceBufferMemoryRequirementsKHR");
    if (!d->GetMemoryFdKHR)
        d->export_type = 0;

    Device *dp = d.get();
    {
        std::unique_lock l(g_lock);
        g_devices[dkey(*out)] = std::move(d);
    }
    if (dp->active) {
        dp->GetDeviceQueue(*out, dp->work_family, dp->work_index, &dp->work_queue);
        // the work queue is a dispatchable object created by the driver: give it the loader's dispatch table
        *(void **)dp->work_queue = dkey(*out);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = dp->work_family;
        dp->CreateCommandPool(*out, &pci, nullptr, &dp->work_pool);
        for (uint32_t i = 0; i < std::max(fake_count, 1u); i++) {
            auto q = std::make_unique<FakeQueue>();
            q->loader_data = dkey(*out);
            q->dev = dp;
            q->index = i;
            FakeQueue *qp = q.get();
            dp->queues.push_back(std::move(q));
            qp->worker = std::thread(worker_main, qp);
        }
        LOGI("device: video encode family %u (%u queue%s), worker queue family %u index %u%s", dp->fake_family,
             std::max(fake_count, 1u), fake_count > 1 ? "s" : "", dp->work_family, dp->work_index,
             dp->work_shared ? " (shared with the application)" : "");
    }
    return VK_SUCCESS;
}

void free_staging(Device *d, FakeQueue *q);

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice dev, const VkAllocationCallbacks *a)
{
    Device *d = dev_of(dev);
    if (!d)
        return;
    for (auto &q : d->queues) {
        {
            std::lock_guard l(q->m);
            q->quit = true;
        }
        q->cv.notify_all();
        q->worker.join();
        free_staging(d, q.get());
    }
    if (d->work_pool)
        d->DestroyCommandPool(dev, d->work_pool, nullptr);
    auto destroy = d->DestroyDevice;
    {
        std::unique_lock l(g_lock);
        g_devices.erase(dkey(dev));
    }
    destroy(dev, a);
}

// ------------------------------------------------------------------------------------------------- queues
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue *q)
{
    Device *d = dev_of(dev);
    if (d->active && fam == d->fake_family) {
        *q = idx < d->queues.size() ? (VkQueue)d->queues[idx].get() : VK_NULL_HANDLE;
        return;
    }
    d->GetDeviceQueue(dev, fam, idx, q);
}
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2 *info, VkQueue *q)
{
    Device *d = dev_of(dev);
    if (d->active && info->queueFamilyIndex == d->fake_family) {
        *q = info->queueIndex < d->queues.size() ? (VkQueue)d->queues[info->queueIndex].get() : VK_NULL_HANDLE;
        return;
    }
    d->GetDeviceQueue2(dev, info, q);
}

bool is_work_queue(Device *d, VkQueue q) { return d->active && d->work_shared && q == d->work_queue; }

std::vector<VkSemaphoreSubmitInfo> sems_from_v1(uint32_t n, const VkSemaphore *s, const VkPipelineStageFlags *stages,
                                                const uint64_t *values)
{
    std::vector<VkSemaphoreSubmitInfo> v(n);
    for (uint32_t i = 0; i < n; i++) {
        v[i] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        v[i].semaphore = s[i];
        v[i].value = values ? values[i] : 0;
        v[i].stageMask = stages ? (VkPipelineStageFlags2)stages[i] : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    return v;
}

void enqueue(FakeQueue *q, Job &&j)
{
    {
        std::lock_guard l(q->m);
        q->jobs.push_back(std::move(j));
    }
    q->cv.notify_one();
}

Job::Batch make_batch(Device *d, std::vector<VkSemaphoreSubmitInfo> w, std::vector<VkSemaphoreSubmitInfo> s,
                      uint32_t ncb, const VkCommandBuffer *cbs)
{
    Job::Batch b;
    b.waits = std::move(w);
    b.signals = std::move(s);
    for (uint32_t i = 0; i < ncb; i++) {
        auto *fc = (FakeCmd *)cbs[i];
        if (!is_fake_cmd(d, cbs[i])) {
            LOGE("submit: command buffer %p on the video queue was not allocated from a video command pool", (void *)cbs[i]);
            continue;
        }
        b.cmds.push_back(fc);
        b.snapshot.push_back(fc->ops);
    }
    return b;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t n, const VkSubmitInfo *s, VkFence fence)
{
    Device *d = dev_of(queue);
    if (FakeQueue *fq = fake_queue(d, queue)) {
        Job j;
        j.fence = fence;
        for (uint32_t i = 0; i < n; i++) {
            auto *tl = (const VkTimelineSemaphoreSubmitInfo *)find_struct(s[i].pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
            j.batches.push_back(make_batch(
                d, sems_from_v1(s[i].waitSemaphoreCount, s[i].pWaitSemaphores, s[i].pWaitDstStageMask, tl ? tl->pWaitSemaphoreValues : nullptr),
                sems_from_v1(s[i].signalSemaphoreCount, s[i].pSignalSemaphores, nullptr, tl ? tl->pSignalSemaphoreValues : nullptr),
                s[i].commandBufferCount, s[i].pCommandBuffers));
        }
        enqueue(fq, std::move(j));
        return VK_SUCCESS;
    }
    if (is_work_queue(d, queue)) {
        std::lock_guard l(d->queue_lock);
        return d->QueueSubmit(queue, n, s, fence);
    }
    return d->QueueSubmit(queue, n, s, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t n, const VkSubmitInfo2 *s, VkFence fence)
{
    Device *d = dev_of(queue);
    if (FakeQueue *fq = fake_queue(d, queue)) {
        Job j;
        j.fence = fence;
        for (uint32_t i = 0; i < n; i++) {
            std::vector<VkSemaphoreSubmitInfo> w(s[i].pWaitSemaphoreInfos, s[i].pWaitSemaphoreInfos + s[i].waitSemaphoreInfoCount);
            std::vector<VkSemaphoreSubmitInfo> sg(s[i].pSignalSemaphoreInfos, s[i].pSignalSemaphoreInfos + s[i].signalSemaphoreInfoCount);
            std::vector<VkCommandBuffer> cbs;
            for (uint32_t k = 0; k < s[i].commandBufferInfoCount; k++)
                cbs.push_back(s[i].pCommandBufferInfos[k].commandBuffer);
            j.batches.push_back(make_batch(d, std::move(w), std::move(sg), (uint32_t)cbs.size(), cbs.data()));
        }
        enqueue(fq, std::move(j));
        return VK_SUCCESS;
    }
    if (is_work_queue(d, queue)) {
        std::lock_guard l(d->queue_lock);
        return d->QueueSubmit2(queue, n, s, fence);
    }
    return d->QueueSubmit2(queue, n, s, fence);
}

void wait_queue_idle(FakeQueue *q)
{
    std::unique_lock l(q->m);
    q->idle_cv.wait(l, [&] { return q->jobs.empty() && !q->busy; });
}

VkResult real_queue_wait_idle(Device *d, VkQueue queue)
{
    if (!is_work_queue(d, queue))
        return d->QueueWaitIdle(queue);
    // Do not hold the queue lock while waiting: a worker may need the queue to make the awaited work finish.
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    VkResult r = d->CreateFence(d->handle, &fci, nullptr, &f);
    if (r != VK_SUCCESS)
        return r;
    {
        std::lock_guard l(d->queue_lock);
        r = d->QueueSubmit(queue, 0, nullptr, f);
    }
    if (r == VK_SUCCESS)
        r = d->WaitForFences(d->handle, 1, &f, VK_TRUE, UINT64_MAX);
    d->DestroyFence(d->handle, f, nullptr);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue)
{
    Device *d = dev_of(queue);
    if (FakeQueue *fq = fake_queue(d, queue)) {
        wait_queue_idle(fq);
        return VK_SUCCESS;
    }
    return real_queue_wait_idle(d, queue);
}

VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice dev)
{
    Device *d = dev_of(dev);
    for (auto &q : d->queues)
        wait_queue_idle(q.get());
    return d->DeviceWaitIdle(dev);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    Device *d = dev_of(queue);
    if (is_work_queue(d, queue)) {
        std::lock_guard l(d->queue_lock);
        return d->QueuePresentKHR(queue, pi);
    }
    return d->QueuePresentKHR(queue, pi);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(VkQueue queue, uint32_t n, const VkBindSparseInfo *b, VkFence f)
{
    Device *d = dev_of(queue);
    if (fake_queue(d, queue))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (is_work_queue(d, queue)) {
        std::lock_guard l(d->queue_lock);
        return d->QueueBindSparse(queue, n, b, f);
    }
    return d->QueueBindSparse(queue, n, b, f);
}

VKAPI_ATTR void VKAPI_CALL QueueBeginDebugUtilsLabelEXT(VkQueue q, const VkDebugUtilsLabelEXT *l)
{
    Device *d = dev_of(q);
    if (!fake_queue(d, q) && d->QueueBeginDebugUtilsLabelEXT)
        d->QueueBeginDebugUtilsLabelEXT(q, l);
}
VKAPI_ATTR void VKAPI_CALL QueueEndDebugUtilsLabelEXT(VkQueue q)
{
    Device *d = dev_of(q);
    if (!fake_queue(d, q) && d->QueueEndDebugUtilsLabelEXT)
        d->QueueEndDebugUtilsLabelEXT(q);
}
VKAPI_ATTR void VKAPI_CALL QueueInsertDebugUtilsLabelEXT(VkQueue q, const VkDebugUtilsLabelEXT *l)
{
    Device *d = dev_of(q);
    if (!fake_queue(d, q) && d->QueueInsertDebugUtilsLabelEXT)
        d->QueueInsertDebugUtilsLabelEXT(q, l);
}

// ------------------------------------------------------------------------------------------------- command pools / buffers
VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(VkDevice dev, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out)
{
    Device *d = dev_of(dev);
    if (d->active && ci->queueFamilyIndex == d->fake_family) {
        auto *p = new CmdPool{ci->flags, {}};
        *out = (VkCommandPool)(uintptr_t)p;
        std::unique_lock l(d->objs);
        d->fake_pools.insert(*out);
        return VK_SUCCESS;
    }
    return d->CreateCommandPool(dev, ci, a, out);
}
bool is_fake_pool(Device *d, VkCommandPool p)
{
    if (!d->active || !p)
        return false;
    std::shared_lock l(d->objs);
    return d->fake_pools.count(p) != 0;
}
VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice dev, VkCommandPool pool, const VkAllocationCallbacks *a)
{
    Device *d = dev_of(dev);
    if (is_fake_pool(d, pool)) {
        auto *p = (CmdPool *)(uintptr_t)pool;
        std::unique_lock l(d->objs);
        for (FakeCmd *c : p->cmds) {
            d->fake_cmds.erase(c);
            delete c;
        }
        d->fake_pools.erase(pool);
        delete p;
        return;
    }
    d->DestroyCommandPool(dev, pool, a);
}
VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags f)
{
    Device *d = dev_of(dev);
    if (is_fake_pool(d, pool)) {
        for (FakeCmd *c : ((CmdPool *)(uintptr_t)pool)->cmds) {
            c->ops.clear();
            c->recording = false;
        }
        return VK_SUCCESS;
    }
    return d->ResetCommandPool(dev, pool, f);
}
VKAPI_ATTR void VKAPI_CALL TrimCommandPool(VkDevice dev, VkCommandPool pool, VkCommandPoolTrimFlags f)
{
    Device *d = dev_of(dev);
    if (!is_fake_pool(d, pool))
        d->TrimCommandPool(dev, pool, f);
}
VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out)
{
    Device *d = dev_of(dev);
    if (is_fake_pool(d, ai->commandPool)) {
        auto *p = (CmdPool *)(uintptr_t)ai->commandPool;
        std::unique_lock l(d->objs);
        for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
            auto *c = new FakeCmd{dkey(dev), p, {}, false};
            p->cmds.insert(c);
            d->fake_cmds.insert(c);
            out[i] = (VkCommandBuffer)c;
        }
        return VK_SUCCESS;
    }
    return d->AllocateCommandBuffers(dev, ai, out);
}
VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice dev, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs)
{
    Device *d = dev_of(dev);
    if (is_fake_pool(d, pool)) {
        auto *p = (CmdPool *)(uintptr_t)pool;
        std::unique_lock l(d->objs);
        for (uint32_t i = 0; i < n; i++) {
            if (!cbs[i])
                continue;
            auto *c = (FakeCmd *)cbs[i];
            p->cmds.erase(c);
            d->fake_cmds.erase(c);
            delete c;
        }
        return;
    }
    d->FreeCommandBuffers(dev, pool, n, cbs);
}
VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb)) {
        auto *c = (FakeCmd *)cb;
        c->ops.clear();
        c->recording = true;
        return VK_SUCCESS;
    }
    return d->BeginCommandBuffer(cb, bi);
}
VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer cb)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb)) {
        ((FakeCmd *)cb)->recording = false;
        return VK_SUCCESS;
    }
    return d->EndCommandBuffer(cb);
}
VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb)) {
        ((FakeCmd *)cb)->ops.clear();
        ((FakeCmd *)cb)->recording = false;
        return VK_SUCCESS;
    }
    return d->ResetCommandBuffer(cb, f);
}

// ------------------------------------------------------------------------------------------------- barriers
uint32_t map_family(Device *d, uint32_t f) { return d->active && f == d->fake_family ? VK_QUEUE_FAMILY_IGNORED : f; }

void fix_families(Device *d, uint32_t &src, uint32_t &dst)
{
    src = map_family(d, src);
    dst = map_family(d, dst);
    if ((src == VK_QUEUE_FAMILY_IGNORED) != (dst == VK_QUEUE_FAMILY_IGNORED))
        src = dst = VK_QUEUE_FAMILY_IGNORED;  // a transfer to/from the video family: no transfer for the driver
}

struct Dep2 {   // a rewritten VkDependencyInfo
    VkDependencyInfo info;
    std::vector<VkMemoryBarrier2> mem;
    std::vector<VkBufferMemoryBarrier2> buf;
    std::vector<VkImageMemoryBarrier2> img;
};
void rewrite_dep(Device *d, const VkDependencyInfo *in, Dep2 &o)
{
    o.info = *in;
    o.mem.assign(in->pMemoryBarriers, in->pMemoryBarriers + in->memoryBarrierCount);
    o.buf.assign(in->pBufferMemoryBarriers, in->pBufferMemoryBarriers + in->bufferMemoryBarrierCount);
    o.img.assign(in->pImageMemoryBarriers, in->pImageMemoryBarriers + in->imageMemoryBarrierCount);
    for (auto &m : o.mem) {
        m.srcStageMask = map_stage2(m.srcStageMask);
        m.dstStageMask = map_stage2(m.dstStageMask);
        m.srcAccessMask = map_access2(m.srcAccessMask);
        m.dstAccessMask = map_access2(m.dstAccessMask);
    }
    for (auto &b : o.buf) {
        b.srcStageMask = map_stage2(b.srcStageMask);
        b.dstStageMask = map_stage2(b.dstStageMask);
        b.srcAccessMask = map_access2(b.srcAccessMask);
        b.dstAccessMask = map_access2(b.dstAccessMask);
        fix_families(d, b.srcQueueFamilyIndex, b.dstQueueFamilyIndex);
    }
    for (auto &b : o.img) {
        b.srcStageMask = map_stage2(b.srcStageMask);
        b.dstStageMask = map_stage2(b.dstStageMask);
        b.srcAccessMask = map_access2(b.srcAccessMask);
        b.dstAccessMask = map_access2(b.dstAccessMask);
        b.oldLayout = map_layout(b.oldLayout);
        b.newLayout = map_layout(b.newLayout);
        fix_families(d, b.srcQueueFamilyIndex, b.dstQueueFamilyIndex);
    }
    o.info.pMemoryBarriers = o.mem.data();
    o.info.pBufferMemoryBarriers = o.buf.data();
    o.info.pImageMemoryBarriers = o.img.data();
}

void record_dep(FakeCmd *c, const VkDependencyInfo *in)
{
    Op op{Op::Barrier};
    op.mem.assign(in->pMemoryBarriers, in->pMemoryBarriers + in->memoryBarrierCount);
    op.buf.assign(in->pBufferMemoryBarriers, in->pBufferMemoryBarriers + in->bufferMemoryBarrierCount);
    op.img.assign(in->pImageMemoryBarriers, in->pImageMemoryBarriers + in->imageMemoryBarrierCount);
    for (auto &m : op.mem) m.pNext = nullptr;
    for (auto &b : op.buf) b.pNext = nullptr;
    for (auto &b : op.img) b.pNext = nullptr;
    c->ops.push_back(std::move(op));
}

void record_dep1(FakeCmd *c, VkPipelineStageFlags ss, VkPipelineStageFlags ds, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb,
                 const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im)
{
    Op op{Op::Barrier};
    for (uint32_t i = 0; i < nm; i++)
        op.mem.push_back({VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, ss, m[i].srcAccessMask, ds, m[i].dstAccessMask});
    for (uint32_t i = 0; i < nb; i++)
        op.buf.push_back({VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, nullptr, ss, b[i].srcAccessMask, ds, b[i].dstAccessMask,
                          b[i].srcQueueFamilyIndex, b[i].dstQueueFamilyIndex, b[i].buffer, b[i].offset, b[i].size});
    for (uint32_t i = 0; i < ni; i++)
        op.img.push_back({VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr, ss, im[i].srcAccessMask, ds, im[i].dstAccessMask,
                          im[i].oldLayout, im[i].newLayout, im[i].srcQueueFamilyIndex, im[i].dstQueueFamilyIndex, im[i].image,
                          im[i].subresourceRange});
    c->ops.push_back(std::move(op));
}

// sync1 barriers on real command buffers
struct Dep1 {
    std::vector<VkMemoryBarrier> mem;
    std::vector<VkBufferMemoryBarrier> buf;
    std::vector<VkImageMemoryBarrier> img;
};
void rewrite_dep1(Device *d, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im, Dep1 &o)
{
    o.buf.assign(b, b + nb);
    o.img.assign(im, im + ni);
    for (auto &x : o.buf) {
        x.srcAccessMask = access1(x.srcAccessMask);
        x.dstAccessMask = access1(x.dstAccessMask);
        fix_families(d, x.srcQueueFamilyIndex, x.dstQueueFamilyIndex);
    }
    for (auto &x : o.img) {
        x.srcAccessMask = access1(x.srcAccessMask);
        x.dstAccessMask = access1(x.dstAccessMask);
        x.oldLayout = map_layout(x.oldLayout);
        x.newLayout = map_layout(x.newLayout);
        fix_families(d, x.srcQueueFamilyIndex, x.dstQueueFamilyIndex);
    }
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags f,
                                              uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
                                              uint32_t ni, const VkImageMemoryBarrier *im)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb))
        return record_dep1((FakeCmd *)cb, ss, ds, nm, m, nb, b, ni, im);
    if (!d->active)
        return d->CmdPipelineBarrier(cb, ss, ds, f, nm, m, nb, b, ni, im);
    Dep1 o;
    rewrite_dep1(d, nb, b, ni, im, o);
    std::vector<VkMemoryBarrier> mm(m, m + nm);
    for (auto &x : mm) {
        x.srcAccessMask = access1(x.srcAccessMask);
        x.dstAccessMask = access1(x.dstAccessMask);
    }
    d->CmdPipelineBarrier(cb, stage1(ss), stage1(ds), f, nm, mm.data(), nb, o.buf.data(), ni, o.img.data());
}
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb))
        return record_dep((FakeCmd *)cb, di);
    if (!d->active)
        return d->CmdPipelineBarrier2(cb, di);
    Dep2 o;
    rewrite_dep(d, di, o);
    d->CmdPipelineBarrier2(cb, &o.info);
}
VKAPI_ATTR void VKAPI_CALL CmdWaitEvents(VkCommandBuffer cb, uint32_t ne, const VkEvent *ev, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                                         uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni,
                                         const VkImageMemoryBarrier *im)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb))
        return record_dep1((FakeCmd *)cb, ss, ds, nm, m, nb, b, ni, im);  // events: treated as barriers
    if (!d->active)
        return d->CmdWaitEvents(cb, ne, ev, ss, ds, nm, m, nb, b, ni, im);
    Dep1 o;
    rewrite_dep1(d, nb, b, ni, im, o);
    d->CmdWaitEvents(cb, ne, ev, stage1(ss), stage1(ds), nm, m, nb, o.buf.data(), ni, o.img.data());
}
VKAPI_ATTR void VKAPI_CALL CmdWaitEvents2(VkCommandBuffer cb, uint32_t ne, const VkEvent *ev, const VkDependencyInfo *di)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb)) {
        for (uint32_t i = 0; i < ne; i++)
            record_dep((FakeCmd *)cb, &di[i]);
        return;
    }
    if (!d->active)
        return d->CmdWaitEvents2(cb, ne, ev, di);
    std::vector<Dep2> o(ne);
    std::vector<VkDependencyInfo> infos(ne);
    for (uint32_t i = 0; i < ne; i++) {
        rewrite_dep(d, &di[i], o[i]);
        infos[i] = o[i].info;
    }
    d->CmdWaitEvents2(cb, ne, ev, infos.data());
}
VKAPI_ATTR void VKAPI_CALL CmdSetEvent2(VkCommandBuffer cb, VkEvent ev, const VkDependencyInfo *di)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb))
        return record_dep((FakeCmd *)cb, di);
    if (!d->active)
        return d->CmdSetEvent2(cb, ev, di);
    Dep2 o;
    rewrite_dep(d, di, o);
    d->CmdSetEvent2(cb, ev, &o.info);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginDebugUtilsLabelEXT(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l)
{
    Device *d = dev_of(cb);
    if (!is_fake_cmd(d, cb) && d->CmdBeginDebugUtilsLabelEXT)
        d->CmdBeginDebugUtilsLabelEXT(cb, l);
}
VKAPI_ATTR void VKAPI_CALL CmdEndDebugUtilsLabelEXT(VkCommandBuffer cb)
{
    Device *d = dev_of(cb);
    if (!is_fake_cmd(d, cb) && d->CmdEndDebugUtilsLabelEXT)
        d->CmdEndDebugUtilsLabelEXT(cb);
}
VKAPI_ATTR void VKAPI_CALL CmdInsertDebugUtilsLabelEXT(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l)
{
    Device *d = dev_of(cb);
    if (!is_fake_cmd(d, cb) && d->CmdInsertDebugUtilsLabelEXT)
        d->CmdInsertDebugUtilsLabelEXT(cb, l);
}

// ------------------------------------------------------------------------------------------------- images, views, buffers
VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice dev, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    Device *d = dev_of(dev);
    if (!d->active || !(ci->usage & kImgVideo))
        return d->CreateImage(dev, ci, a, out);
    VkImageCreateInfo c = *ci;
    c.usage = map_image_usage(ci->usage);
    c.flags &= ~VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
    std::vector<uint32_t> fams;
    if (c.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        for (uint32_t i = 0; i < ci->queueFamilyIndexCount; i++)
            if (ci->pQueueFamilyIndices[i] != d->fake_family)
                fams.push_back(ci->pQueueFamilyIndices[i]);
        if (std::find(fams.begin(), fams.end(), d->work_family) == fams.end())
            fams.push_back(d->work_family);
        if (fams.size() < 2) {
            c.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            c.queueFamilyIndexCount = 0;
            c.pQueueFamilyIndices = nullptr;
        } else {
            c.queueFamilyIndexCount = (uint32_t)fams.size();
            c.pQueueFamilyIndices = fams.data();
        }
    }
    Unlinker u;
    u.cut(&c, kVideoInfoTypes);
    VkResult r = d->CreateImage(dev, &c, a, out);
    if (r == VK_SUCCESS && (ci->usage & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR)) {
        std::unique_lock l(d->objs);
        d->images[*out] = {ci->format, ci->extent, ci->arrayLayers};
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice dev, VkImage img, const VkAllocationCallbacks *a)
{
    Device *d = dev_of(dev);
    if (d->active && img) {
        std::unique_lock l(d->objs);
        d->images.erase(img);
    }
    d->DestroyImage(dev, img, a);
}
VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2(VkDevice dev, uint32_t n, const VkBindImageMemoryInfo *bi)
{
    Device *d = dev_of(dev);
    if (g_log_level >= 3)
        for (uint32_t i = 0; i < n; i++) {
            auto *pl = (const VkBindImagePlaneMemoryInfo *)find_struct(bi[i].pNext, VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO);
            LOGT("bind image %#llx memory %#llx offset %llu plane %d", (unsigned long long)(uintptr_t)bi[i].image,
                 (unsigned long long)(uintptr_t)bi[i].memory, (unsigned long long)bi[i].memoryOffset, pl ? (int)pl->planeAspect : -1);
        }
    return d->BindImageMemory2(dev, n, bi);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice dev, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out)
{
    Device *d = dev_of(dev);
    if (!d->active)
        return d->CreateImageView(dev, ci, a, out);
    Unlinker u;
    u.cut(ci, kVideoInfoTypes);
    VkImageViewUsageCreateInfo *vu = (VkImageViewUsageCreateInfo *)find_struct_out((void *)ci->pNext, VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO);
    VkImageUsageFlags saved = vu ? vu->usage : 0;
    if (vu && (vu->usage & kImgVideo)) {
        vu->usage &= ~kImgVideo;
        if (!vu->usage)
            u.cut(ci, {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO});
    }
    VkResult r = d->CreateImageView(dev, ci, a, out);
    if (vu)
        vu->usage = saved;
    if (r == VK_SUCCESS) {
        std::unique_lock l(d->objs);
        if (d->images.count(ci->image))
            d->views[*out] = {ci->image, ci->format, ci->subresourceRange.baseArrayLayer};
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks *a)
{
    Device *d = dev_of(dev);
    if (d->active && v) {
        std::unique_lock l(d->objs);
        d->views.erase(v);
    }
    d->DestroyImageView(dev, v, a);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice dev, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out)
{
    Device *d = dev_of(dev);
    if (!d->active)
        return d->CreateBuffer(dev, ci, a, out);
    auto *u2 = (VkBufferUsageFlags2CreateInfoKHR *)find_struct_out((void *)ci->pNext, VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR);
    bool video = (ci->usage & kBufVideo) || (u2 && (u2->usage & kBufVideo));
    if (!video)
        return d->CreateBuffer(dev, ci, a, out);
    VkBufferCreateInfo c = *ci;
    c.usage = map_buffer_usage(ci->usage);
    std::vector<uint32_t> fams;
    if (c.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        for (uint32_t i = 0; i < ci->queueFamilyIndexCount; i++)
            if (ci->pQueueFamilyIndices[i] != d->fake_family)
                fams.push_back(ci->pQueueFamilyIndices[i]);
        if (std::find(fams.begin(), fams.end(), d->work_family) == fams.end())
            fams.push_back(d->work_family);
        if (fams.size() < 2) {
            c.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            c.queueFamilyIndexCount = 0;
            c.pQueueFamilyIndices = nullptr;
        } else {
            c.queueFamilyIndexCount = (uint32_t)fams.size();
            c.pQueueFamilyIndices = fams.data();
        }
    }
    VkBufferUsageFlags2KHR saved = u2 ? u2->usage : 0;
    if (u2)
        u2->usage = map_buffer_usage((VkBufferUsageFlags)(u2->usage & 0xffffffffu)) | (u2->usage & ~0xffffffffull);
    Unlinker u;
    u.cut(&c, kVideoInfoTypes);
    VkResult r = d->CreateBuffer(dev, &c, a, out);
    if (u2)
        u2->usage = saved;
    return r;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceImageMemoryRequirements(VkDevice dev, const VkDeviceImageMemoryRequirements *info, VkMemoryRequirements2 *out)
{
    Device *d = dev_of(dev);
    if (!d->active || !(info->pCreateInfo->usage & kImgVideo))
        return d->GetDeviceImageMemoryRequirements(dev, info, out);
    VkImageCreateInfo c = *info->pCreateInfo;
    c.usage = map_image_usage(c.usage);
    c.flags &= ~VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
    if (c.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        c.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        c.queueFamilyIndexCount = 0;
        c.pQueueFamilyIndices = nullptr;
    }
    Unlinker u;
    u.cut(&c, kVideoInfoTypes);
    VkDeviceImageMemoryRequirements i2 = *info;
    i2.pCreateInfo = &c;
    d->GetDeviceImageMemoryRequirements(dev, &i2, out);
}
VKAPI_ATTR void VKAPI_CALL GetDeviceBufferMemoryRequirements(VkDevice dev, const VkDeviceBufferMemoryRequirements *info, VkMemoryRequirements2 *out)
{
    Device *d = dev_of(dev);
    if (!d->active || !(info->pCreateInfo->usage & kBufVideo))
        return d->GetDeviceBufferMemoryRequirements(dev, info, out);
    VkBufferCreateInfo c = *info->pCreateInfo;
    c.usage = map_buffer_usage(c.usage);
    if (c.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        c.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        c.queueFamilyIndexCount = 0;
        c.pQueueFamilyIndices = nullptr;
    }
    Unlinker u;
    u.cut(&c, kVideoInfoTypes);
    VkDeviceBufferMemoryRequirements i2 = *info;
    i2.pCreateInfo = &c;
    d->GetDeviceBufferMemoryRequirements(dev, &i2, out);
}

// ------------------------------------------------------------------------------------------------- query pools
VKAPI_ATTR VkResult VKAPI_CALL CreateQueryPool(VkDevice dev, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out)
{
    Device *d = dev_of(dev);
    if (d->active && (ci->queryType == VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR || ci->queryType == VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR)) {
        auto *p = new QueryPool{ci->queryType, 0, std::vector<QuerySlot>(ci->queryCount)};
        if (auto *fb = (const VkQueryPoolVideoEncodeFeedbackCreateInfoKHR *)find_struct(ci->pNext, VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR))
            p->feedback = fb->encodeFeedbackFlags;
        *out = (VkQueryPool)(uintptr_t)p;
        std::unique_lock l(d->objs);
        d->fake_query_pools.insert(*out);
        return VK_SUCCESS;
    }
    return d->CreateQueryPool(dev, ci, a, out);
}
VKAPI_ATTR void VKAPI_CALL DestroyQueryPool(VkDevice dev, VkQueryPool pool, const VkAllocationCallbacks *a)
{
    Device *d = dev_of(dev);
    if (QueryPool *p = fake_qpool(d, pool)) {
        std::unique_lock l(d->objs);
        d->fake_query_pools.erase(pool);
        delete p;
        return;
    }
    d->DestroyQueryPool(dev, pool, a);
}
void reset_queries(Device *d, QueryPool *p, uint32_t first, uint32_t n)
{
    std::lock_guard l(d->query_m);
    for (uint32_t i = first; i < first + n && i < p->slots.size(); i++)
        p->slots[i] = QuerySlot{};
}
VKAPI_ATTR void VKAPI_CALL ResetQueryPool(VkDevice dev, VkQueryPool pool, uint32_t first, uint32_t n)
{
    Device *d = dev_of(dev);
    if (QueryPool *p = fake_qpool(d, pool))
        return reset_queries(d, p, first, n);
    d->ResetQueryPool(dev, pool, first, n);
}
VKAPI_ATTR VkResult VKAPI_CALL GetQueryPoolResults(VkDevice dev, VkQueryPool pool, uint32_t first, uint32_t n, size_t size, void *data,
                                                   VkDeviceSize stride, VkQueryResultFlags flags)
{
    Device *d = dev_of(dev);
    QueryPool *p = fake_qpool(d, pool);
    if (!p)
        return d->GetQueryPoolResults(dev, pool, first, n, size, data, stride, flags);
    VkResult res = VK_SUCCESS;
    std::unique_lock l(d->query_m);
    for (uint32_t q = 0; q < n; q++) {
        uint32_t qi = first + q;
        if (qi >= p->slots.size())
            break;
        if ((flags & VK_QUERY_RESULT_WAIT_BIT) && !p->slots[qi].avail)
            d->query_cv.wait_for(l, std::chrono::seconds(5), [&] { return p->slots[qi].avail; });
        QuerySlot &s = p->slots[qi];
        std::vector<int64_t> vals;
        if (p->type == VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR) {
            if (p->feedback & VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR)
                vals.push_back((int64_t)s.offset);
            if (p->feedback & VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR)
                vals.push_back((int64_t)s.bytes);
            if (p->feedback & VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_HAS_OVERRIDES_BIT_KHR)
                vals.push_back((int64_t)s.overrides);
        }
        if (flags & VK_QUERY_RESULT_WITH_STATUS_BIT_KHR || p->type == VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR)
            vals.push_back(s.status);
        if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
            vals.push_back(s.avail ? 1 : 0);
        if (!s.avail)
            res = VK_NOT_READY;
        bool write = s.avail || (flags & VK_QUERY_RESULT_PARTIAL_BIT);
        auto *dst = (uint8_t *)data + (size_t)q * stride;
        for (size_t i = 0; i < vals.size(); i++) {
            bool is_avail = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) && i == vals.size() - 1;
            if (!write && !is_avail)
                continue;
            if (flags & VK_QUERY_RESULT_64_BIT)
                ((int64_t *)dst)[i] = vals[i];
            else
                ((int32_t *)dst)[i] = (int32_t)vals[i];
        }
    }
    return res;
}
VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t n)
{
    Device *d = dev_of(cb);
    QueryPool *p = fake_qpool(d, pool);
    if (is_fake_cmd(d, cb)) {
        Op op{Op::ResetQuery};
        op.pool = p;
        op.first = first;
        op.count = n;
        if (p)
            ((FakeCmd *)cb)->ops.push_back(std::move(op));
        return;
    }
    if (p) {   // a video query pool reset on another queue: applied now (the closest the layer can do)
        reset_queries(d, p, first, n);
        return;
    }
    d->CmdResetQueryPool(cb, pool, first, n);
}
VKAPI_ATTR void VKAPI_CALL CmdBeginQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t q, VkQueryControlFlags f)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb)) {
        Op op{Op::BeginQuery};
        op.pool = fake_qpool(d, pool);
        op.first = q;
        if (op.pool)
            ((FakeCmd *)cb)->ops.push_back(std::move(op));
        return;
    }
    if (!fake_qpool(d, pool))
        d->CmdBeginQuery(cb, pool, q, f);
}
VKAPI_ATTR void VKAPI_CALL CmdEndQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t q)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb)) {
        Op op{Op::EndQuery};
        op.pool = fake_qpool(d, pool);
        op.first = q;
        if (op.pool)
            ((FakeCmd *)cb)->ops.push_back(std::move(op));
        return;
    }
    if (!fake_qpool(d, pool))
        d->CmdEndQuery(cb, pool, q);
}
VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool pool, uint32_t q)
{
    Device *d = dev_of(cb);
    if (!is_fake_cmd(d, cb))
        d->CmdWriteTimestamp(cb, (VkPipelineStageFlagBits)stage1(st), pool, q);
}
VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 st, VkQueryPool pool, uint32_t q)
{
    Device *d = dev_of(cb);
    if (!is_fake_cmd(d, cb))
        d->CmdWriteTimestamp2(cb, map_stage2(st), pool, q);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t n, VkBuffer dst,
                                                   VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags f)
{
    Device *d = dev_of(cb);
    if (is_fake_cmd(d, cb))
        return;
    if (fake_qpool(d, pool)) {
        LOGE("vkCmdCopyQueryPoolResults on a video encode feedback query pool is not supported");
        return;
    }
    d->CmdCopyQueryPoolResults(cb, pool, first, n, dst, off, stride, f);
}

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(VkDevice dev, const VkDebugUtilsObjectNameInfoEXT *info)
{
    Device *d = dev_of(dev);
    if (d->active) {
        std::shared_lock l(d->objs);
        uint64_t h = info->objectHandle;
        if (d->fake_cmds.count((void *)(uintptr_t)h) || d->fake_pools.count((VkCommandPool)h) ||
            d->fake_query_pools.count((VkQueryPool)h) || d->fake_sessions.count(h))
            return VK_SUCCESS;
        for (auto &q : d->queues)
            if ((uint64_t)(uintptr_t)q.get() == h)
                return VK_SUCCESS;
    }
    return d->SetDebugUtilsObjectNameEXT ? d->SetDebugUtilsObjectNameEXT(dev, info) : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectTagEXT(VkDevice dev, const VkDebugUtilsObjectTagInfoEXT *info)
{
    Device *d = dev_of(dev);
    if (d->active) {
        std::shared_lock l(d->objs);
        uint64_t h = info->objectHandle;
        if (d->fake_cmds.count((void *)(uintptr_t)h) || d->fake_pools.count((VkCommandPool)h) ||
            d->fake_query_pools.count((VkQueryPool)h) || d->fake_sessions.count(h))
            return VK_SUCCESS;
    }
    return d->SetDebugUtilsObjectTagEXT ? d->SetDebugUtilsObjectTagEXT(dev, info) : VK_SUCCESS;
}

// ------------------------------------------------------------------------------------------------- sessions and parameters
VKAPI_ATTR VkResult VKAPI_CALL CreateVideoSessionKHR(VkDevice dev, const VkVideoSessionCreateInfoKHR *ci, const VkAllocationCallbacks *,
                                                     VkVideoSessionKHR *out)
{
    Device *d = dev_of(dev);
    uint32_t bd = 8;
    uint32_t codec = d->active ? check_profile(*d->phys, ci->pVideoProfile, &bd) : 0;
    if (!codec)
        return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;
    const venc_limits &l = codec == VENC_H264 ? d->phys->l264 : d->phys->l265;
    if (ci->maxCodedExtent.width > l.max_width || ci->maxCodedExtent.height > l.max_height)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto *s = new Session{};
    s->codec = codec;
    s->max_extent = ci->maxCodedExtent;
    s->picture_format = ci->pictureFormat;
    s->bit_depth = bd;
    s->profile_idc = 0;
    if (auto *h = (const VkVideoEncodeH264ProfileInfoKHR *)find_struct(ci->pVideoProfile->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR))
        s->profile_idc = h->stdProfileIdc;
    if (auto *h = (const VkVideoEncodeH265ProfileInfoKHR *)find_struct(ci->pVideoProfile->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR))
        s->profile_idc = h->stdProfileIdc;
    s->inline_queries = ci->flags & VK_VIDEO_SESSION_CREATE_INLINE_QUERIES_BIT_KHR;
    *out = (VkVideoSessionKHR)(uintptr_t)s;
    std::unique_lock lk(d->objs);
    d->fake_sessions.insert((uint64_t)(uintptr_t)s);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyVideoSessionKHR(VkDevice dev, VkVideoSessionKHR s, const VkAllocationCallbacks *)
{
    Device *d = dev_of(dev);
    if (!s)
        return;
    for (auto &q : d->queues)
        wait_queue_idle(q.get());   // the application must not destroy it while in use; be safe anyway
    auto *ss = (Session *)(uintptr_t)s;
    if (ss->enc)
        g_backend->destroy(ss->enc);
    std::unique_lock lk(d->objs);
    d->fake_sessions.erase((uint64_t)(uintptr_t)s);
    delete ss;
}
VKAPI_ATTR VkResult VKAPI_CALL GetVideoSessionMemoryRequirementsKHR(VkDevice, VkVideoSessionKHR, uint32_t *count,
                                                                    VkVideoSessionMemoryRequirementsKHR *)
{
    *count = 0;   // the backend keeps its own memory
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL BindVideoSessionMemoryKHR(VkDevice, VkVideoSessionKHR, uint32_t, const VkBindVideoSessionMemoryInfoKHR *)
{
    return VK_SUCCESS;
}

void add_h264_params(SessionParams *p, const VkVideoEncodeH264SessionParametersAddInfoKHR *add)
{
    if (!add)
        return;
    for (uint32_t i = 0; i < add->stdSPSCount; i++) {
        const StdVideoH264SequenceParameterSet &s = add->pStdSPSs[i];
        ParamSummary m;
        uint32_t w = (s.pic_width_in_mbs_minus1 + 1) * 16;
        uint32_t h = (s.pic_height_in_map_units_minus1 + 1) * 16 * (s.flags.frame_mbs_only_flag ? 1 : 2);
        if (s.flags.frame_cropping_flag) {
            uint32_t cy = 2 * (s.flags.frame_mbs_only_flag ? 1 : 2);
            w -= 2 * (s.frame_crop_left_offset + s.frame_crop_right_offset);
            h -= cy * (s.frame_crop_top_offset + s.frame_crop_bottom_offset);
        }
        m.width = w;
        m.height = h;
        m.profile_idc = s.profile_idc;
        m.bit_depth = 8 + s.bit_depth_luma_minus8;
        if (s.flags.vui_parameters_present_flag && s.pSequenceParameterSetVui && s.pSequenceParameterSetVui->flags.timing_info_present_flag &&
            s.pSequenceParameterSetVui->num_units_in_tick) {
            m.fps_num = s.pSequenceParameterSetVui->time_scale;
            m.fps_den = 2 * s.pSequenceParameterSetVui->num_units_in_tick;
        }
        p->sps[s.seq_parameter_set_id] = m;
    }
    for (uint32_t i = 0; i < add->stdSPSCount; i++)
        p->h264_sps[add->pStdSPSs[i].seq_parameter_set_id] = std::make_shared<H264SpsStore>(add->pStdSPSs[i]);
    for (uint32_t i = 0; i < add->stdPPSCount; i++) {
        p->pps_cabac[add->pStdPPSs[i].pic_parameter_set_id] = add->pStdPPSs[i].flags.entropy_coding_mode_flag;
        p->h264_pps[add->pStdPPSs[i].pic_parameter_set_id] = std::make_shared<H264PpsStore>(add->pStdPPSs[i]);
    }
}
void add_h265_params(SessionParams *p, const VkVideoEncodeH265SessionParametersAddInfoKHR *add)
{
    if (!add)
        return;
    for (uint32_t i = 0; i < add->stdSPSCount; i++) {
        const StdVideoH265SequenceParameterSet &s = add->pStdSPSs[i];
        ParamSummary m;
        uint32_t w = s.pic_width_in_luma_samples, h = s.pic_height_in_luma_samples;
        if (s.flags.conformance_window_flag) {
            w -= 2 * (s.conf_win_left_offset + s.conf_win_right_offset);
            h -= 2 * (s.conf_win_top_offset + s.conf_win_bottom_offset);
        }
        m.width = w;
        m.height = h;
        m.profile_idc = s.pProfileTierLevel ? s.pProfileTierLevel->general_profile_idc : 1;
        m.bit_depth = 8 + s.bit_depth_luma_minus8;
        if (s.flags.vui_parameters_present_flag && s.pSequenceParameterSetVui && s.pSequenceParameterSetVui->flags.vui_timing_info_present_flag &&
            s.pSequenceParameterSetVui->vui_num_units_in_tick) {
            m.fps_num = s.pSequenceParameterSetVui->vui_time_scale;
            m.fps_den = s.pSequenceParameterSetVui->vui_num_units_in_tick;
        }
        p->sps[s.sps_seq_parameter_set_id] = m;
        p->h265_sps[s.sps_seq_parameter_set_id] = std::make_shared<H265SpsStore>(s);
    }
    for (uint32_t i = 0; i < add->stdVPSCount; i++)
        p->h265_vps[add->pStdVPSs[i].vps_video_parameter_set_id] = std::make_shared<H265VpsStore>(add->pStdVPSs[i]);
    for (uint32_t i = 0; i < add->stdPPSCount; i++)
        p->h265_pps[add->pStdPPSs[i].pps_pic_parameter_set_id] = std::make_shared<H265PpsStore>(add->pStdPPSs[i]);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateVideoSessionParametersKHR(VkDevice dev, const VkVideoSessionParametersCreateInfoKHR *ci,
                                                               const VkAllocationCallbacks *, VkVideoSessionParametersKHR *out)
{
    Device *d = dev_of(dev);
    auto *s = (Session *)(uintptr_t)ci->videoSession;
    auto *p = new SessionParams{};
    p->codec = s->codec;
    if (ci->videoSessionParametersTemplate) {
        auto *t = (SessionParams *)(uintptr_t)ci->videoSessionParametersTemplate;
        p->sps = t->sps;
        p->pps_cabac = t->pps_cabac;
        p->h264_sps = t->h264_sps;
        p->h264_pps = t->h264_pps;
        p->h265_vps = t->h265_vps;
        p->h265_sps = t->h265_sps;
        p->h265_pps = t->h265_pps;
    }
    if (s->codec == VENC_H264) {
        auto *h = (const VkVideoEncodeH264SessionParametersCreateInfoKHR *)find_struct(ci->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR);
        if (h)
            add_h264_params(p, h->pParametersAddInfo);
    } else {
        auto *h = (const VkVideoEncodeH265SessionParametersCreateInfoKHR *)find_struct(ci->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR);
        if (h)
            add_h265_params(p, h->pParametersAddInfo);
    }
    *out = (VkVideoSessionParametersKHR)(uintptr_t)p;
    std::unique_lock lk(d->objs);
    d->fake_sessions.insert((uint64_t)(uintptr_t)p);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL UpdateVideoSessionParametersKHR(VkDevice, VkVideoSessionParametersKHR params, const VkVideoSessionParametersUpdateInfoKHR *ui)
{
    auto *p = (SessionParams *)(uintptr_t)params;
    if (p->codec == VENC_H264)
        add_h264_params(p, (const VkVideoEncodeH264SessionParametersAddInfoKHR *)find_struct(ui->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR));
    else
        add_h265_params(p, (const VkVideoEncodeH265SessionParametersAddInfoKHR *)find_struct(ui->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyVideoSessionParametersKHR(VkDevice dev, VkVideoSessionParametersKHR params, const VkAllocationCallbacks *)
{
    Device *d = dev_of(dev);
    if (!params)
        return;
    auto *p = (SessionParams *)(uintptr_t)params;
    if (p->probe)
        g_backend->destroy(p->probe);
    std::unique_lock lk(d->objs);
    d->fake_sessions.erase((uint64_t)(uintptr_t)p);
    delete p;
}

// The stream identity (everything but rate control) for the given parameter set ids.
bool identity_config(const SessionParams *p, uint32_t sps_id, uint32_t pps_id, uint32_t profile_hint, venc_config *c)
{
    auto it = p->sps.find(sps_id);
    if (it == p->sps.end())
        return false;
    const ParamSummary &m = it->second;
    *c = venc_config{};
    c->codec = p->codec;
    c->width = m.width;
    c->height = m.height;
    c->profile_idc = m.profile_idc ? m.profile_idc : profile_hint;
    c->bit_depth = m.bit_depth;
    auto pi = p->pps_cabac.find(pps_id);
    c->cabac = pi == p->pps_cabac.end() ? 1 : pi->second;
    c->transform_8x8 = 1;
    auto hp = p->h264_pps.find(pps_id);
    if (hp != p->h264_pps.end()) {
        c->transform_8x8 = hp->second->pps.flags.transform_8x8_mode_flag;
        c->chroma_qp_offset = hp->second->pps.chroma_qp_index_offset;
    }
    c->fps_num = m.fps_num ? m.fps_num : 90;
    c->fps_den = m.fps_num ? m.fps_den : 1;
    auto h5 = p->h265_sps.find(sps_id);
    if (p->codec == VENC_H265 && h5 != p->h265_sps.end()) {
        const StdVideoH265SequenceParameterSet &x = h5->second->sps;
        // the coded size: the encoder's picture grid must be the application's (the conformance window only
        // crops the display); the layer pads source pictures to it
        c->width = x.pic_width_in_luma_samples;
        c->height = x.pic_height_in_luma_samples;
        c->h265_log2_min_cb = 3 + x.log2_min_luma_coding_block_size_minus3;
        c->h265_log2_ctb = c->h265_log2_min_cb + x.log2_diff_max_min_luma_coding_block_size;
        c->h265_log2_min_tb = 2 + x.log2_min_luma_transform_block_size_minus2;
        c->h265_log2_max_tb = c->h265_log2_min_tb + x.log2_diff_max_min_luma_transform_block_size;
        c->h265_tu_depth_inter = x.max_transform_hierarchy_depth_inter + 1u;
        c->h265_tu_depth_intra = x.max_transform_hierarchy_depth_intra + 1u;
        c->h265_amp = x.flags.amp_enabled_flag ? 2 : 1;
        c->h265_sao = x.flags.sample_adaptive_offset_enabled_flag ? 2 : 1;
        c->h265_tmvp = x.flags.sps_temporal_mvp_enabled_flag ? 2 : 1;
        c->h265_strong_intra = x.flags.strong_intra_smoothing_enabled_flag ? 2 : 1;
    }
    if (p->codec == VENC_H264 && c->profile_idc != 66 && c->profile_idc != 77 && c->profile_idc != 100)
        c->profile_idc = c->transform_8x8 ? 100 : (c->cabac ? 77 : 66);   // unknown/invalid: the least that fits
    return true;
}
bool same_identity(const venc_config &a, const venc_config &b)
{
    return a.codec == b.codec && a.width == b.width && a.height == b.height && a.profile_idc == b.profile_idc &&
           a.bit_depth == b.bit_depth && a.cabac == b.cabac && a.transform_8x8 == b.transform_8x8 &&
           a.chroma_qp_offset == b.chroma_qp_offset && a.fps_num == b.fps_num && a.fps_den == b.fps_den &&
           a.h265_log2_ctb == b.h265_log2_ctb && a.h265_log2_min_cb == b.h265_log2_min_cb && a.h265_log2_min_tb == b.h265_log2_min_tb &&
           a.h265_log2_max_tb == b.h265_log2_max_tb && a.h265_tu_depth_inter == b.h265_tu_depth_inter &&
           a.h265_tu_depth_intra == b.h265_tu_depth_intra && a.h265_amp == b.h265_amp && a.h265_sao == b.h265_sao &&
           a.h265_tmvp == b.h265_tmvp && a.h265_strong_intra == b.h265_strong_intra;
}

// The backend's H.264 SPS/PPS, parsed (for rewriting its slices and for PPS overrides).
bool backend_h264_params(void *enc, venc::Sps &sps, venc::Pps &pps)
{
    uint8_t buf[4096];
    size_t n = sizeof(buf);
    if (g_backend->headers(enc, buf, &n) != 0)
        return false;
    bool have_sps = false, have_pps = false;
    for (size_t i = 0; i + 4 < n;) {
        size_t j = i + 4;
        while (j + 4 <= n && !(buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 0 && buf[j + 3] == 1))
            j++;
        if (j + 4 > n)
            j = n;
        uint32_t t = buf[i + 4] & 0x1f;
        if (t == 7)
            have_sps = venc::parse_sps(buf + i + 4, j - i - 4, sps);
        else if (t == 8 && have_sps)
            have_pps = venc::parse_pps(buf + i + 4, j - i - 4, sps, pps);
        i = j;
    }
    return have_sps && have_pps;
}

// The backend's H.265 SPS/PPS, parsed.
bool backend_h265_params(void *enc, venc::h265::Sps &sps, venc::h265::Pps &pps)
{
    uint8_t buf[4096];
    size_t n = sizeof(buf);
    if (g_backend->headers(enc, buf, &n) != 0)
        return false;
    bool have_sps = false, have_pps = false;
    for (size_t i = 0; i + 4 < n;) {
        size_t j = i + 4;
        while (j + 4 <= n && !(buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 0 && buf[j + 3] == 1))
            j++;
        if (j + 4 > n)
            j = n;
        uint32_t t = (buf[i + 4] >> 1) & 0x3f;
        if (t == 33)
            have_sps = venc::h265::parse_sps(buf + i + 4, j - i - 4, sps);
        else if (t == 34)
            have_pps = venc::h265::parse_pps(buf + i + 4, j - i - 4, pps);
        i = j;
    }
    return have_sps && have_pps;
}

// The application's PPS with the fields slice data depends on set to the backend's. Returns true if any changed.
bool override_h264_pps(StdVideoH264PictureParameterSet &p, const venc::Pps &e)
{
    StdVideoH264PictureParameterSet o = p;
    p.flags.entropy_coding_mode_flag = e.cabac;
    p.flags.transform_8x8_mode_flag = e.transform_8x8;
    p.chroma_qp_index_offset = (int8_t)e.chroma_qp_offset;
    p.second_chroma_qp_index_offset = (int8_t)e.second_chroma_qp_offset;
    p.flags.constrained_intra_pred_flag = e.constrained_intra;
    if (e.deblocking_control)
        p.flags.deblocking_filter_control_present_flag = 1;
    if (!e.weighted_pred)
        p.flags.weighted_pred_flag = p.flags.weighted_pred_flag;  // unweighted slices decode under either value
    p.flags.pic_scaling_matrix_present_flag = 0;
    p.pScalingLists = nullptr;
    return memcmp(&o.flags, &p.flags, sizeof(o.flags)) != 0 || o.chroma_qp_index_offset != p.chroma_qp_index_offset ||
           o.second_chroma_qp_index_offset != p.second_chroma_qp_index_offset;
}

VKAPI_ATTR VkResult VKAPI_CALL GetEncodedVideoSessionParametersKHR(VkDevice, const VkVideoEncodeSessionParametersGetInfoKHR *info,
                                                                   VkVideoEncodeSessionParametersFeedbackInfoKHR *fb, size_t *size, void *data)
{
    auto *p = (SessionParams *)(uintptr_t)info->videoSessionParameters;
    bool want_vps = false, want_sps = false, want_pps = false;
    uint32_t sps_id = 0, pps_id = 0;
    if (p->codec == VENC_H264) {
        auto *g = (const VkVideoEncodeH264SessionParametersGetInfoKHR *)find_struct(info->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR);
        if (!g)
            return VK_ERROR_INITIALIZATION_FAILED;
        want_sps = g->writeStdSPS;
        want_pps = g->writeStdPPS;
        sps_id = g->stdSPSId;
        pps_id = g->stdPPSId;
    } else {
        auto *g = (const VkVideoEncodeH265SessionParametersGetInfoKHR *)find_struct(info->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_KHR);
        if (!g)
            return VK_ERROR_INITIALIZATION_FAILED;
        want_vps = g->writeStdVPS;
        want_sps = g->writeStdSPS;
        want_pps = g->writeStdPPS;
        sps_id = g->stdSPSId;
        pps_id = g->stdPPSId;
    }
    venc_config c;
    if (!identity_config(p, sps_id, pps_id, 0, &c))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!p->probe || !same_identity(c, p->probe_cfg)) {
        if (p->probe)
            g_backend->destroy(p->probe);
        p->probe = g_backend->create(&c);
        p->probe_cfg = c;
        if (!p->probe)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (p->codec == VENC_H264) {
        // the application's SPS as it is; its PPS with the fields the backend's slice data needs
        auto si = p->h264_sps.find(sps_id);
        auto pi = p->h264_pps.find(pps_id);
        venc::Sps esps;
        venc::Pps epps;
        if (si == p->h264_sps.end() || (want_pps && pi == p->h264_pps.end()) || !backend_h264_params(p->probe, esps, epps))
            return VK_ERROR_INITIALIZATION_FAILED;
        std::vector<uint8_t> out;
        bool pps_changed = false;
        if (want_sps)
            venc::write_sps(out, si->second->sps);
        if (want_pps) {
            StdVideoH264PictureParameterSet pps = pi->second->pps;
            pps_changed = override_h264_pps(pps, epps);
            venc::write_pps(out, pps, si->second->sps);
        }
        if (fb) {
            fb->hasOverrides = pps_changed;
            if (auto *h = (VkVideoEncodeH264SessionParametersFeedbackInfoKHR *)find_struct_out(fb->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR)) {
                h->hasStdSPSOverrides = VK_FALSE;
                h->hasStdPPSOverrides = pps_changed;
            }
        }
        if (!data) {
            *size = out.size();
            return VK_SUCCESS;
        }
        if (*size < out.size()) {
            *size = 0;
            return VK_INCOMPLETE;
        }
        memcpy(data, out.data(), out.size());
        *size = out.size();
        return VK_SUCCESS;
    }
    if (p->codec == VENC_H265) {
        auto si = p->h265_sps.find(sps_id);
        auto pi = p->h265_pps.find(pps_id);
        uint32_t vps_id = si != p->h265_sps.end() ? si->second->sps.sps_video_parameter_set_id : 0;
        auto vi = p->h265_vps.find(vps_id);
        venc::h265::Sps esps;
        venc::h265::Pps epps;
        if ((want_sps && si == p->h265_sps.end()) || (want_pps && pi == p->h265_pps.end()) ||
            (want_vps && vi == p->h265_vps.end()) || !backend_h265_params(p->probe, esps, epps))
            return VK_ERROR_INITIALIZATION_FAILED;
        std::vector<uint8_t> out;
        bool pps_changed = false, sps_changed = false;
        if (want_vps)
            venc::h265::write_vps(out, vi->second->vps);
        if (want_sps) {
            StdVideoH265SequenceParameterSet sps = si->second->sps;
            sps_changed = venc::h265::override_sps(sps, esps);
            venc::h265::write_sps(out, sps);
        }
        if (want_pps) {
            StdVideoH265PictureParameterSet pps = pi->second->pps;
            pps_changed = venc::h265::override_pps(pps, epps);
            venc::h265::write_pps(out, pps);
        }
        if (fb) {
            fb->hasOverrides = pps_changed || sps_changed;
            if (auto *h = (VkVideoEncodeH265SessionParametersFeedbackInfoKHR *)find_struct_out(fb->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_FEEDBACK_INFO_KHR)) {
                h->hasStdVPSOverrides = VK_FALSE;
                h->hasStdSPSOverrides = sps_changed;
                h->hasStdPPSOverrides = pps_changed;
            }
        }
        if (!data) {
            *size = out.size();
            return VK_SUCCESS;
        }
        if (*size < out.size()) {
            *size = 0;
            return VK_INCOMPLETE;
        }
        memcpy(data, out.data(), out.size());
        *size = out.size();
        return VK_SUCCESS;
    }
    uint8_t all[4096], sel[4096];
    size_t n = sizeof(all);
    if (g_backend->headers(p->probe, all, &n) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    // keep the requested NAL unit types
    size_t w = 0;
    for (size_t i = 0; i + 4 < n;) {
        size_t j = i + 4;
        while (j + 4 <= n && !(all[j] == 0 && all[j + 1] == 0 && all[j + 2] == 0 && all[j + 3] == 1))
            j++;
        if (j + 4 > n)
            j = n;
        uint32_t t = venc_nal_type(p->codec, all[i + 4]);
        bool keep = p->codec == VENC_H264 ? ((t == 7 && want_sps) || (t == 8 && want_pps))
                                          : ((t == 32 && want_vps) || (t == 33 && want_sps) || (t == 34 && want_pps));
        if (keep) {
            memcpy(sel + w, all + i, j - i);
            w += j - i;
        }
        i = j;
    }
    if (fb) {
        fb->hasOverrides = VK_TRUE;
        if (auto *h = (VkVideoEncodeH264SessionParametersFeedbackInfoKHR *)find_struct_out(fb->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR)) {
            h->hasStdSPSOverrides = want_sps;
            h->hasStdPPSOverrides = want_pps;
        }
        if (auto *h = (VkVideoEncodeH265SessionParametersFeedbackInfoKHR *)find_struct_out(fb->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_FEEDBACK_INFO_KHR)) {
            h->hasStdVPSOverrides = want_vps;
            h->hasStdSPSOverrides = want_sps;
            h->hasStdPPSOverrides = want_pps;
        }
    }
    if (!data) {
        *size = w;
        return VK_SUCCESS;
    }
    if (*size < w) {
        *size = 0;
        return VK_INCOMPLETE;
    }
    memcpy(data, sel, w);
    *size = w;
    return VK_SUCCESS;
}

// ------------------------------------------------------------------------------------------------- video coding commands
bool rc_from(const VkVideoEncodeRateControlInfoKHR *rc, venc_config *out)
{
    if (!rc)
        return false;
    *out = venc_config{};
    switch (rc->rateControlMode) {
    case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR: out->rc = VENC_RC_CQP; break;
    case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR: out->rc = VENC_RC_CBR; break;
    case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR: out->rc = VENC_RC_VBR; break;
    default: out->rc = VENC_RC_DEFAULT; break;
    }
    if (rc->layerCount && rc->pLayers) {
        out->avg_bitrate = rc->pLayers[0].averageBitrate;
        out->max_bitrate = rc->pLayers[0].maxBitrate;
        out->fps_num = rc->pLayers[0].frameRateNumerator;
        out->fps_den = rc->pLayers[0].frameRateDenominator;
    }
    out->vbv_ms = rc->virtualBufferSizeInMs;
    return true;
}

FakeCmd *rec(Device *d, VkCommandBuffer cb)
{
    return is_fake_cmd(d, cb) ? (FakeCmd *)cb : nullptr;
}

VKAPI_ATTR void VKAPI_CALL CmdBeginVideoCodingKHR(VkCommandBuffer cb, const VkVideoBeginCodingInfoKHR *bi)
{
    Device *d = dev_of(cb);
    FakeCmd *c = rec(d, cb);
    if (!c)
        return;
    Op op{Op::BeginCoding};
    op.session = (Session *)(uintptr_t)bi->videoSession;
    op.params = (SessionParams *)(uintptr_t)bi->videoSessionParameters;
    op.has_rc = rc_from((const VkVideoEncodeRateControlInfoKHR *)find_struct(bi->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR), &op.rc);
    c->ops.push_back(std::move(op));
}
VKAPI_ATTR void VKAPI_CALL CmdControlVideoCodingKHR(VkCommandBuffer cb, const VkVideoCodingControlInfoKHR *ci)
{
    Device *d = dev_of(cb);
    FakeCmd *c = rec(d, cb);
    if (!c)
        return;
    Op op{Op::Control};
    op.ctrl = ci->flags;
    if (ci->flags & VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR)
        op.has_rc = rc_from((const VkVideoEncodeRateControlInfoKHR *)find_struct(ci->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR), &op.rc);
    c->ops.push_back(std::move(op));
}
VKAPI_ATTR void VKAPI_CALL CmdEndVideoCodingKHR(VkCommandBuffer cb, const VkVideoEndCodingInfoKHR *)
{
    Device *d = dev_of(cb);
    if (FakeCmd *c = rec(d, cb))
        c->ops.push_back(Op{Op::EndCoding});
}
VKAPI_ATTR void VKAPI_CALL CmdEncodeVideoKHR(VkCommandBuffer cb, const VkVideoEncodeInfoKHR *ei)
{
    Device *d = dev_of(cb);
    FakeCmd *c = rec(d, cb);
    if (!c)
        return;
    Op op{Op::Encode};
    {
        std::shared_lock l(d->objs);
        auto v = d->views.find(ei->srcPictureResource.imageViewBinding);
        if (v == d->views.end()) {
            LOGE("encode: source view %p unknown (image not created with VIDEO_ENCODE_SRC usage)",
                 (void *)(uintptr_t)ei->srcPictureResource.imageViewBinding);
            return;
        }
        op.src = v->second.image;
        auto im = d->images.find(op.src);
        op.src_format = im != d->images.end() ? im->second.format : v->second.format;
        op.src_layer = v->second.base_layer + ei->srcPictureResource.baseArrayLayer;
        LOGT("record encode: view %#llx -> image %#llx layer %u", (unsigned long long)(uintptr_t)ei->srcPictureResource.imageViewBinding,
             (unsigned long long)(uintptr_t)op.src, op.src_layer);
    }
    op.off = ei->srcPictureResource.codedOffset;
    op.ext = ei->srcPictureResource.codedExtent;
    op.dst = ei->dstBuffer;
    op.dst_offset = ei->dstBufferOffset;
    op.dst_range = ei->dstBufferRange;
    if (auto *h = (const VkVideoEncodeH264PictureInfoKHR *)find_struct(ei->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR)) {
        if (h->pStdPictureInfo) {
            op.force_idr = h->pStdPictureInfo->flags.IdrPicFlag;
            op.sps_id = h->pStdPictureInfo->seq_parameter_set_id;
            op.pps_id = h->pStdPictureInfo->pic_parameter_set_id;
            op.pic.have = true;
            op.pic.frame_num = h->pStdPictureInfo->frame_num;
            op.pic.idr_pic_id = h->pStdPictureInfo->idr_pic_id;
            op.pic.poc = h->pStdPictureInfo->PicOrderCnt;
        }
    }
    if (auto *h = (const VkVideoEncodeH265PictureInfoKHR *)find_struct(ei->pNext, VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR)) {
        if (h->pStdPictureInfo) {
            op.force_idr = h->pStdPictureInfo->pic_type == STD_VIDEO_H265_PICTURE_TYPE_IDR || h->pStdPictureInfo->flags.IrapPicFlag;
            op.pic.have = true;
            op.pic.poc = h->pStdPictureInfo->PicOrderCntVal;
            op.sps_id = h->pStdPictureInfo->pps_seq_parameter_set_id;
            op.pps_id = h->pStdPictureInfo->pps_pic_parameter_set_id;
        }
    }
    if (auto *iq = (const VkVideoInlineQueryInfoKHR *)find_struct(ei->pNext, VK_STRUCTURE_TYPE_VIDEO_INLINE_QUERY_INFO_KHR)) {
        if (iq->queryPool && iq->queryCount) {
            op.inline_pool = fake_qpool(d, iq->queryPool);
            op.inline_query = iq->firstQuery;
        }
    }
    c->ops.push_back(std::move(op));
}

// ================================================================================================= worker
uint32_t find_mem_type(Device *d, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags *got)
{
    const VkPhysicalDeviceMemoryProperties &mp = d->phys->mem;
    // prefer host-cached (fast CPU reads), then any host-visible
    for (VkMemoryPropertyFlags extra : {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_CACHED_BIT), VkMemoryPropertyFlags(0)})
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (want | extra)) == (want | extra)) {
                *got = mp.memoryTypes[i].propertyFlags;
                return i;
            }
    return UINT32_MAX;
}

void release_staging(Device *d, FakeQueue::Staging &st)
{
    if (!st.buf)
        return;
    if (st.handle && g_backend->release_memory)
        g_backend->release_memory(st.handle);
    d->DestroyBuffer(d->handle, st.buf, nullptr);
    d->FreeMemory(d->handle, st.mem, nullptr);
    st = FakeQueue::Staging{};
}

// Host-visible staging memory; exported to the backend when it imports memory (the Mac backend reads pictures and
// writes bitstreams in place).
bool ensure_buffer(Device *d, VkDeviceSize need, VkBufferUsageFlags usage, FakeQueue::Staging &st)
{
    if (st.size >= need)
        return true;
    release_staging(d, st);
    VkDeviceSize sz = std::max<VkDeviceSize>(need, 1 << 20);
    sz = (sz + 0xffff) & ~VkDeviceSize(0xffff);
    VkExternalMemoryHandleTypeFlagBits ht = d->export_type == 1 ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
                                                                : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalMemoryBufferCreateInfo ebi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebi.handleTypes = ht;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = d->export_type ? &ebi : nullptr;
    bci.size = sz;
    bci.usage = usage;
    VkResult vr = d->CreateBuffer(d->handle, &bci, nullptr, &st.buf);
    if (vr != VK_SUCCESS) {
        LOGE("staging buffer: vkCreateBuffer %d (export %d)", vr, d->export_type);
        return false;
    }
    VkMemoryRequirements req;
    d->GetBufferMemoryRequirements(d->handle, st.buf, &req);
    VkMemoryPropertyFlags got = 0;
    uint32_t mt = find_mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &got);
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.buffer = st.buf;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &ded;
    exp.handleTypes = ht;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = d->export_type ? &exp : nullptr;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
    vr = mt == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : d->AllocateMemory(d->handle, &mai, nullptr, &st.mem);
    if (vr != VK_SUCCESS) {
        d->DestroyBuffer(d->handle, st.buf, nullptr);
        st = FakeQueue::Staging{};
        if (d->export_type == 1) {   // dma-buf export unavailable (lavapipe without udmabuf): opaque fds
            LOGI("staging memory: no dma-buf export (%d), using opaque fds", vr);
            d->export_type = 2;
            return ensure_buffer(d, need, usage, st);
        }
        LOGE("staging memory: %d (type %u of bits %#x, export %d)", vr, mt, req.memoryTypeBits, d->export_type);
        return false;
    }
    d->BindBufferMemory(d->handle, st.buf, st.mem, 0);
    void *p = nullptr;
    d->MapMemory(d->handle, st.mem, 0, VK_WHOLE_SIZE, 0, &p);
    st.ptr = (uint8_t *)p;
    st.coherent = got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    st.size = sz;
    if (d->export_type && d->GetMemoryFdKHR && g_backend->import_memory) {
        VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        gi.memory = st.mem;
        gi.handleType = ht;
        int fd = -1;
        if (d->GetMemoryFdKHR(d->handle, &gi, &fd) == VK_SUCCESS && fd >= 0) {
            int r = g_backend->import_memory(fd, req.size, d->export_type == 1, &st.handle);
            close(fd);
            if (r != 0) {
                LOGE("backend %s cannot import staging memory (%d): pictures go through pointers", g_backend->name, r);
                st.handle = 0;
            }
        }
    }
    return true;
}

void free_staging(Device *d, FakeQueue *q)
{
    release_staging(d, q->in);
    release_staging(d, q->out);
    if (q->fence)
        d->DestroyFence(d->handle, q->fence, nullptr);
    if (q->cb1 || q->cb2) {
        std::lock_guard l(d->work_pool_lock);
        VkCommandBuffer cbs[2] = {q->cb1, q->cb2};
        d->FreeCommandBuffers(d->handle, d->work_pool, 2, cbs);
    }
}

VkResult submit_v1(Device *d, const std::vector<VkSemaphoreSubmitInfo> &waits, VkCommandBuffer cb,
                   const std::vector<VkSemaphoreSubmitInfo> &signals, VkFence fence)
{
    std::vector<VkSemaphore> ws, ss;
    std::vector<VkPipelineStageFlags> wst;
    std::vector<uint64_t> wv, sv;
    bool timeline = false;
    for (auto &w : waits) {
        ws.push_back(w.semaphore);
        wst.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        wv.push_back(w.value);
        timeline |= w.value != 0;
    }
    for (auto &s : signals) {
        ss.push_back(s.semaphore);
        sv.push_back(s.value);
        timeline |= s.value != 0;
    }
    VkTimelineSemaphoreSubmitInfo tl{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tl.waitSemaphoreValueCount = (uint32_t)wv.size();
    tl.pWaitSemaphoreValues = wv.data();
    tl.signalSemaphoreValueCount = (uint32_t)sv.size();
    tl.pSignalSemaphoreValues = sv.data();
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = timeline ? &tl : nullptr;
    si.waitSemaphoreCount = (uint32_t)ws.size();
    si.pWaitSemaphores = ws.data();
    si.pWaitDstStageMask = wst.data();
    si.commandBufferCount = cb ? 1 : 0;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = (uint32_t)ss.size();
    si.pSignalSemaphores = ss.data();
    if (d->work_shared) {
        std::lock_guard l(d->queue_lock);
        return d->QueueSubmit(d->work_queue, 1, &si, fence);
    }
    return d->QueueSubmit(d->work_queue, 1, &si, fence);
}

void barrier_into(Device *d, VkCommandBuffer cb, const Op &op)
{
    VkPipelineStageFlags ss = 0, ds = 0;
    std::vector<VkMemoryBarrier> m;
    std::vector<VkBufferMemoryBarrier> b;
    std::vector<VkImageMemoryBarrier> im;
    for (auto &x : op.mem) {
        ss |= stage1(x.srcStageMask);
        ds |= stage1(x.dstStageMask);
        m.push_back({VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, access1(x.srcAccessMask), access1(x.dstAccessMask)});
    }
    for (auto &x : op.buf) {
        ss |= stage1(x.srcStageMask);
        ds |= stage1(x.dstStageMask);
        VkBufferMemoryBarrier y{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, access1(x.srcAccessMask), access1(x.dstAccessMask),
                                x.srcQueueFamilyIndex, x.dstQueueFamilyIndex, x.buffer, x.offset, x.size};
        fix_families(d, y.srcQueueFamilyIndex, y.dstQueueFamilyIndex);
        b.push_back(y);
    }
    for (auto &x : op.img) {
        ss |= stage1(x.srcStageMask);
        ds |= stage1(x.dstStageMask);
        VkImageMemoryBarrier y{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, access1(x.srcAccessMask), access1(x.dstAccessMask),
                               map_layout(x.oldLayout), map_layout(x.newLayout), x.srcQueueFamilyIndex, x.dstQueueFamilyIndex,
                               x.image, x.subresourceRange};
        fix_families(d, y.srcQueueFamilyIndex, y.dstQueueFamilyIndex);
        // the layer reads source pictures with transfers
        y.dstAccessMask |= VK_ACCESS_TRANSFER_READ_BIT;
        im.push_back(y);
    }
    if (!ss)
        ss = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    ds |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    d->CmdPipelineBarrier(cb, ss, ds, 0, (uint32_t)m.size(), m.data(), (uint32_t)b.size(), b.data(), (uint32_t)im.size(), im.data());
}

struct PlaneCopy {
    VkDeviceSize luma_off, chroma_off;
    uint32_t luma_stride, chroma_stride;
    uint32_t w, h;     // the picture handed to the encoder (padded to the coded size)
    uint32_t cw, ch;   // the region copied from the source image
};

// Replicates the last column/row of the copied region out to the padded size (both planes).
void pad_picture(uint8_t *base, const PlaneCopy &pc, uint32_t tb)
{
    if (pc.cw >= pc.w && pc.ch >= pc.h)
        return;
    auto pad = [&](uint8_t *plane, uint32_t stride, uint32_t cw_bytes, uint32_t w_bytes, uint32_t ch, uint32_t h, uint32_t unit) {
        for (uint32_t y = 0; y < ch; y++) {
            uint8_t *row = plane + (size_t)y * stride;
            for (uint32_t x = cw_bytes; x + unit <= w_bytes; x += unit)
                memcpy(row + x, row + cw_bytes - unit, unit);
        }
        for (uint32_t y = ch; y < h; y++)
            memcpy(plane + (size_t)y * stride, plane + (size_t)(ch - 1) * stride, w_bytes);
    };
    if (pc.cw == 0 || pc.ch == 0)
        return;
    pad(base + pc.luma_off, pc.luma_stride, pc.cw * tb, pc.w * tb, pc.ch, pc.h, tb);
    pad(base + pc.chroma_off, pc.chroma_stride, ((pc.cw + 1) / 2) * 2 * tb, ((pc.w + 1) / 2) * 2 * tb, (pc.ch + 1) / 2,
        (pc.h + 1) / 2, 2 * tb);
}

uint32_t texel_bytes(VkFormat f) { return f == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 ? 2 : 1; }

void ensure_session_encoder(Session *s, SessionParams *p, const Op &op)
{
    venc_config c;
    if (!identity_config(p, op.sps_id, op.pps_id, s->profile_idc, &c)) {
        // no parameters for these ids: encode the picture as it is
        c = venc_config{};
        c.codec = s->codec;
        c.width = op.ext.width;
        c.height = op.ext.height;
        c.profile_idc = s->profile_idc;
        c.bit_depth = s->bit_depth;
        c.cabac = 1;
        c.fps_num = 90;
        c.fps_den = 1;
    }
    if (s->have_rc) {
        c.rc = s->rc.rc;
        c.avg_bitrate = s->rc.avg_bitrate;
        c.max_bitrate = s->rc.max_bitrate;
        c.vbv_ms = s->rc.vbv_ms;
        c.qp = s->rc.qp;
    }
    if (s->enc && !s->reset_pending && same_identity(c, s->cfg)) {
        if (c.rc != s->cfg.rc || c.avg_bitrate != s->cfg.avg_bitrate || c.max_bitrate != s->cfg.max_bitrate || c.vbv_ms != s->cfg.vbv_ms) {
            if (g_backend->reconfigure(s->enc, &c) == 0)
                s->cfg = c;
            else
                s->reset_pending = true;
        }
        if (!s->reset_pending)
            return;
    }
    if (s->enc)
        g_backend->destroy(s->enc);
    s->enc = g_backend->create(&c);
    s->cfg = c;
    s->reset_pending = false;
    s->pocs.reset();
    s->enc_parsed = s->enc && (s->codec == VENC_H264 ? backend_h264_params(s->enc, s->enc_sps, s->enc_pps)
                                                     : backend_h265_params(s->enc, s->enc_sps5, s->enc_pps5));
    if (s->enc && !s->enc_parsed)
        LOGE("cannot parse the backend's parameter sets: slices are passed through unchanged");
    if (!s->enc)
        LOGE("backend %s could not create a %ux%u %s encoder", g_backend->name, c.width, c.height, c.codec == VENC_H264 ? "H.264" : "H.265");
    else
        LOGI("encoder: %s %ux%u, profile %u, rc %u, %llu bit/s", c.codec == VENC_H264 ? "H.264" : "H.265", c.width, c.height,
             c.profile_idc, c.rc, (unsigned long long)c.avg_bitrate);
}

void set_query(Device *d, QueryPool *p, uint32_t q, int32_t status, uint64_t bytes)
{
    if (!p || q >= p->slots.size())
        return;
    std::lock_guard l(d->query_m);
    QuerySlot &s = p->slots[q];
    s.avail = true;
    s.status = status;
    s.offset = 0;
    s.bytes = bytes;
    s.overrides = 0;   // the bitstream follows the parameter sets returned by vkGetEncodedVideoSessionParametersKHR
}

void run_batch(FakeQueue *q, Job::Batch &b, VkFence app_fence)
{
    Device *d = q->dev;
    if (g_log_level >= 3) {
        std::string w, s;
        for (auto &x : b.waits)
            w += " " + std::to_string((uintptr_t)x.semaphore & 0xffff) + "@" + std::to_string(x.value);
        for (auto &x : b.signals)
            s += " " + std::to_string((uintptr_t)x.semaphore & 0xffff) + "@" + std::to_string(x.value);
        LOGT("batch: waits%s signals%s fence %d", w.c_str(), s.c_str(), app_fence != VK_NULL_HANDLE);
    }
    // 1. source pictures to host memory, after the batch's waits
    std::vector<const Op *> encodes;
    std::vector<VkExtent2D> coded;   // per encode: the size the encoder works at (H.265: the SPS's coded size)
    {
        Session *cs = nullptr;
        SessionParams *cp = nullptr;
        for (auto &ops : b.snapshot)
            for (auto &op : ops) {
                if (op.kind == Op::BeginCoding) {
                    cs = op.session;
                    cp = op.params;
                } else if (op.kind == Op::Encode) {
                    encodes.push_back(&op);
                    VkExtent2D e = op.ext;
                    venc_config c;
                    if (cs && cp && cs->codec == VENC_H265 && identity_config(cp, op.sps_id, op.pps_id, cs->profile_idc, &c))
                        e = {std::max(e.width, c.width), std::max(e.height, c.height)};
                    coded.push_back(e);
                }
            }
    }
    std::vector<PlaneCopy> planes(encodes.size());
    VkDeviceSize need = 0;
    for (size_t i = 0; i < encodes.size(); i++) {
        const Op &op = *encodes[i];
        uint32_t tb = texel_bytes(op.src_format);
        PlaneCopy &pc = planes[i];
        pc.cw = op.ext.width;
        pc.ch = op.ext.height;
        pc.w = coded[i].width;
        pc.h = coded[i].height;
        pc.luma_stride = pc.w * tb;
        pc.chroma_stride = ((pc.w + 1) / 2) * 2 * tb;
        pc.luma_off = (need + 255) & ~VkDeviceSize(255);
        pc.chroma_off = (pc.luma_off + (VkDeviceSize)pc.luma_stride * pc.h + 255) & ~VkDeviceSize(255);
        need = pc.chroma_off + (VkDeviceSize)pc.chroma_stride * ((pc.h + 1) / 2);
    }
    VkDeviceSize out_need = 0;
    for (const Op *op : encodes)
        out_need += std::min<VkDeviceSize>(op->dst_range, 64ull << 20) + 256;
    if (!encodes.empty() && (!ensure_buffer(d, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, q->in) ||
                             !ensure_buffer(d, out_need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, q->out))) {
        LOGE("cannot allocate %llu bytes of staging memory", (unsigned long long)need);
        encodes.clear();
    }

    bool has_barriers = false;
    for (auto &ops : b.snapshot)
        for (auto &op : ops)
            has_barriers |= op.kind == Op::Barrier;

    if (encodes.empty() && !has_barriers) {
        // pure synchronization (or only state changes): forward the semaphores, then run the state ops
        for (auto &ops : b.snapshot)
            for (auto &op : ops)
                if (op.kind == Op::ResetQuery)
                    reset_queries(d, op.pool, op.first, op.count);
        submit_v1(d, b.waits, VK_NULL_HANDLE, b.signals, app_fence);
        return;
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->ResetCommandBuffer(q->cb1, 0);
    d->BeginCommandBuffer(q->cb1, &bi);
    size_t ei = 0;
    for (auto &ops : b.snapshot)
        for (auto &op : ops) {
            if (op.kind == Op::Barrier) {
                barrier_into(d, q->cb1, op);
            } else if (op.kind == Op::Encode && ei < encodes.size() && encodes[ei] == &op) {
                const PlaneCopy &pc = planes[ei++];
                VkBufferImageCopy r[2]{};
                r[0].bufferOffset = pc.luma_off;
                r[0].bufferRowLength = pc.w;
                r[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, op.src_layer, 1};
                r[0].imageOffset = {op.off.x, op.off.y, 0};
                r[0].imageExtent = {pc.cw, pc.ch, 1};
                r[1].bufferOffset = pc.chroma_off;
                r[1].bufferRowLength = (pc.w + 1) / 2;
                r[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, op.src_layer, 1};
                r[1].imageOffset = {op.off.x / 2, op.off.y / 2, 0};
                r[1].imageExtent = {(pc.cw + 1) / 2, (pc.ch + 1) / 2, 1};
                d->CmdCopyImageToBuffer(q->cb1, op.src, VK_IMAGE_LAYOUT_GENERAL, q->in.buf, 2, r);
            }
        }
    VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    d->CmdPipelineBarrier(q->cb1, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
    d->EndCommandBuffer(q->cb1);
    d->ResetFences(d->handle, 1, &q->fence);
    if (submit_v1(d, b.waits, q->cb1, {}, q->fence) != VK_SUCCESS) {
        LOGE("staging submit failed");
        submit_v1(d, {}, VK_NULL_HANDLE, b.signals, app_fence);
        return;
    }
    d->WaitForFences(d->handle, 1, &q->fence, VK_TRUE, UINT64_MAX);
    if (!q->in.coherent && q->in.mem) {
        VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, q->in.mem, 0, VK_WHOLE_SIZE};
        d->InvalidateMappedMemoryRanges(d->handle, 1, &mr);
    }

    // 2. encode on the CPU side, in command order
    struct Out { VkBuffer dst; VkDeviceSize dst_off, src_off, size; };
    std::vector<Out> outs;
    size_t out_at = 0;   // bitstreams are encoded straight into the out staging memory
    Session *cur = nullptr;
    SessionParams *curp = nullptr;
    QueryPool *active_pool = nullptr;
    uint32_t active_query = 0;
    ei = 0;
    for (auto &ops : b.snapshot)
        for (auto &op : ops) {
            switch (op.kind) {
            case Op::ResetQuery:
                reset_queries(d, op.pool, op.first, op.count);
                break;
            case Op::BeginQuery:
                active_pool = op.pool;
                active_query = op.first;
                break;
            case Op::EndQuery:
                active_pool = nullptr;
                break;
            case Op::BeginCoding:
                cur = op.session;
                curp = op.params;
                if (op.has_rc && cur && !cur->have_rc) {
                    cur->rc = op.rc;
                    cur->have_rc = true;
                }
                break;
            case Op::Control:
                if (!cur)
                    break;
                if (op.ctrl & VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR)
                    cur->reset_pending = true;
                if (op.has_rc) {
                    cur->rc = op.rc;
                    cur->have_rc = true;
                }
                break;
            case Op::EndCoding:
                cur = nullptr;
                curp = nullptr;
                break;
            case Op::Encode: {
                QueryPool *qp = op.inline_pool ? op.inline_pool : active_pool;
                uint32_t qi = op.inline_pool ? op.inline_query : active_query;
                if (ei >= encodes.size() || encodes[ei] != &op || !cur || !curp) {
                    set_query(d, qp, qi, VK_QUERY_RESULT_STATUS_ERROR_KHR, 0);
                    break;
                }
                const PlaneCopy &pc = planes[ei++];
                ensure_session_encoder(cur, curp, op);
                if (!cur->enc) {
                    set_query(d, qp, qi, VK_QUERY_RESULT_STATUS_ERROR_KHR, 0);
                    break;
                }
                venc_picture pic{};
                pic.luma = q->in.ptr + pc.luma_off;
                pic.chroma = q->in.ptr + pc.chroma_off;
                pic.mem = q->in.handle;
                pic.luma_offset = pc.luma_off;
                pic.chroma_offset = pc.chroma_off;
                pic.luma_stride = pc.luma_stride;
                pic.chroma_stride = pc.chroma_stride;
                pad_picture(q->in.ptr, pc, texel_bytes(op.src_format));
                pic.width = std::min(pc.w, cur->cfg.width);
                pic.height = std::min(pc.h, cur->cfg.height);
                pic.force_idr = op.force_idr;
                if (const char *dump = getenv("VENC_DUMP")) {   // debugging: the staged pictures as raw NV12
                    static std::atomic<int> nd{0};
                    char path[512];
                    snprintf(path, sizeof(path), "%s/in-%04d.nv12", dump, nd++);
                    if (FILE *f = fopen(path, "wb")) {
                        for (uint32_t y = 0; y < pic.height; y++)
                            fwrite(pic.luma + (size_t)y * pic.luma_stride, 1, pic.width * texel_bytes(op.src_format), f);
                        for (uint32_t y = 0; y < (pic.height + 1) / 2; y++)
                            fwrite(pic.chroma + (size_t)y * pic.chroma_stride, 1, ((pic.width + 1) / 2) * 2 * texel_bytes(op.src_format), f);
                        fclose(f);
                    }
                }
                size_t at = (out_at + 255) & ~size_t(255);
                size_t cap = (size_t)std::min<VkDeviceSize>(op.dst_range, 64ull << 20);
                if (at + cap > q->out.size)
                    cap = at < q->out.size ? q->out.size - at : 0;
                uint8_t *dst = q->out.ptr + at;
                pic.out_mem = q->out.handle;
                pic.out_offset = at;
                int idr = 0;
                long n = cap ? g_backend->encode(cur->enc, &pic, dst, cap, &idr) : -ENOSPC;
                if (n == -ENOSPC) {
                    set_query(d, qp, qi, VK_QUERY_RESULT_STATUS_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_KHR, 0);
                    break;
                }
                if (n < 0) {
                    set_query(d, qp, qi, VK_QUERY_RESULT_STATUS_ERROR_KHR, 0);
                    break;
                }
                if (!q->out.coherent) {   // the backend may have written through another mapping (the host's)
                    VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, q->out.mem, 0, VK_WHOLE_SIZE};
                    d->InvalidateMappedMemoryRanges(d->handle, 1, &mr);
                }
                if (cur->codec == VENC_H264 && cur->enc_parsed && n > 0) {
                    auto si = curp->h264_sps.find(op.sps_id);
                    auto pi = curp->h264_pps.find(op.pps_id);
                    if (si != curp->h264_sps.end() && pi != curp->h264_pps.end()) {
                        venc::Sps as = venc::sps_from_std(si->second->sps);
                        venc::Pps ap = venc::pps_from_std(pi->second->pps);
                        std::string inc = venc::incompatibility(cur->enc_sps, cur->enc_pps, as, ap);
                        if (!inc.empty() && inc != cur->warned) {
                            LOGE("the application's H.264 parameter sets differ from the encoder's in: %s (picture corruption likely)", inc.c_str());
                            cur->warned = inc;
                        }
                        std::vector<uint8_t> src(dst, dst + n), nb;
                        std::string err;
                        bool ok = true;
                        size_t len = (size_t)n;
                        for (size_t i = 0; i + 4 < len && ok;) {
                            size_t j = i + 4;
                            while (j + 4 <= len && !(src[j] == 0 && src[j + 1] == 0 && src[j + 2] == 0 && src[j + 3] == 1))
                                j++;
                            if (j + 4 > len)
                                j = len;
                            ok = venc::rewrite_slice(src.data() + i + 4, j - i - 4, cur->enc_sps, cur->enc_pps, as, ap, op.pic, nb, &err);
                            i = j;
                        }
                        if (!ok) {
                            LOGE("slice rewrite failed: %s", err.c_str());
                        } else if (nb.size() > cap) {
                            set_query(d, qp, qi, VK_QUERY_RESULT_STATUS_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_KHR, 0);
                            break;
                        } else {
                            memcpy(dst, nb.data(), nb.size());
                            n = (long)nb.size();
                        }
                    }
                }
                if (cur->codec == VENC_H265 && cur->enc_parsed && n > 0) {
                    auto si = curp->h265_sps.find(op.sps_id);
                    auto pi = curp->h265_pps.find(op.pps_id);
                    if (si != curp->h265_sps.end() && pi != curp->h265_pps.end()) {
                        venc::h265::Sps as = venc::h265::sps_from_std(si->second->sps);
                        venc::h265::Pps ap = venc::h265::pps_from_std(pi->second->pps);
                        std::string inc = venc::h265::incompatibility(cur->enc_sps5, cur->enc_pps5, as, ap);
                        if (!inc.empty() && inc != cur->warned) {
                            LOGE("the application's H.265 parameter sets differ from the encoder's in: %s (picture corruption likely)", inc.c_str());
                            cur->warned = inc;
                        }
                        venc::h265::PicInfo pinfo;
                        pinfo.have = op.pic.have;
                        pinfo.idr = op.force_idr;
                        pinfo.poc = op.pic.poc;
                        std::vector<uint8_t> src(dst, dst + n), nb;
                        std::string err;
                        bool ok = true;
                        size_t len = (size_t)n;
                        for (size_t i = 0; i + 4 < len && ok;) {
                            size_t j = i + 4;
                            while (j + 4 <= len && !(src[j] == 0 && src[j + 1] == 0 && src[j + 2] == 0 && src[j + 3] == 1))
                                j++;
                            if (j + 4 > len)
                                j = len;
                            ok = venc::h265::rewrite_slice(src.data() + i + 4, j - i - 4, cur->enc_sps5, cur->enc_pps5, as, ap, pinfo,
                                                           cur->pocs, nb, &err);
                            i = j;
                        }
                        if (!ok) {
                            LOGE("H.265 slice rewrite failed: %s", err.c_str());
                        } else if (nb.size() > cap) {
                            set_query(d, qp, qi, VK_QUERY_RESULT_STATUS_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_KHR, 0);
                            break;
                        } else {
                            memcpy(dst, nb.data(), nb.size());
                            n = (long)nb.size();
                        }
                    }
                }
                if (op.force_idr && !idr)
                    LOGE("backend did not produce an IDR picture when asked");
                LOGD("encoded %ld bytes (%s)", n, idr ? "IDR" : "P");
                if (n > 0) {
                    outs.push_back({op.dst, op.dst_offset, at, (VkDeviceSize)n});
                    out_at = at + (size_t)n;
                }
                set_query(d, qp, qi, VK_QUERY_RESULT_STATUS_COMPLETE_KHR, (uint64_t)n);
                break;
            }
            default:
                break;
            }
        }

    // 3. bitstreams into the application's buffers, then the batch's signals
    VkCommandBuffer cb2 = VK_NULL_HANDLE;
    if (!outs.empty()) {
        if (!q->out.coherent) {
            VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, q->out.mem, 0, VK_WHOLE_SIZE};
            d->FlushMappedMemoryRanges(d->handle, 1, &mr);
        }
        d->ResetCommandBuffer(q->cb2, 0);
        d->BeginCommandBuffer(q->cb2, &bi);
        VkMemoryBarrier hw{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        d->CmdPipelineBarrier(q->cb2, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &hw, 0, nullptr, 0, nullptr);
        for (auto &o : outs) {
            VkBufferCopy c{o.src_off, o.dst_off, o.size};
            d->CmdCopyBuffer(q->cb2, q->out.buf, o.dst, 1, &c);
        }
        VkMemoryBarrier ab{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT};
        d->CmdPipelineBarrier(q->cb2, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                              &ab, 0, nullptr, 0, nullptr);
        d->EndCommandBuffer(q->cb2);
        cb2 = q->cb2;
    }
    if (cb2) {
        // cb2 is reused by the next batch: wait for it before returning
        d->ResetFences(d->handle, 1, &q->fence);
        submit_v1(d, {}, cb2, b.signals, q->fence);
        d->WaitForFences(d->handle, 1, &q->fence, VK_TRUE, UINT64_MAX);
        if (app_fence)
            submit_v1(d, {}, VK_NULL_HANDLE, {}, app_fence);
    } else {
        submit_v1(d, {}, VK_NULL_HANDLE, b.signals, app_fence);
    }
    d->query_cv.notify_all();
}

void worker_main(FakeQueue *q)
{
    Device *d = q->dev;
    {
        std::lock_guard l(d->work_pool_lock);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = d->work_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 2;
        VkCommandBuffer cbs[2];
        d->AllocateCommandBuffers(d->handle, &ai, cbs);
        q->cb1 = cbs[0];
        q->cb2 = cbs[1];
        *(void **)q->cb1 = dkey(d->handle);   // driver objects created inside the layer need the loader's dispatch
        *(void **)q->cb2 = dkey(d->handle);
    }
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    d->CreateFence(d->handle, &fci, nullptr, &q->fence);
    for (;;) {
        Job j;
        {
            std::unique_lock l(q->m);
            q->cv.wait(l, [&] { return q->quit || !q->jobs.empty(); });
            if (q->jobs.empty())
                return;
            j = std::move(q->jobs.front());
            q->jobs.pop_front();
            q->busy = true;
        }
        if (j.batches.empty()) {
            if (j.fence)
                submit_v1(d, {}, VK_NULL_HANDLE, {}, j.fence);
        }
        for (size_t i = 0; i < j.batches.size(); i++)
            run_batch(q, j.batches[i], i + 1 == j.batches.size() ? j.fence : VK_NULL_HANDLE);
        {
            std::lock_guard l(q->m);
            q->busy = false;
        }
        q->idle_cv.notify_all();
    }
}

// ================================================================================================= proc addr
struct Hook { const char *name; PFN_vkVoidFunction fn; bool needs_active; };

#define H(n) {"vk" #n, (PFN_vkVoidFunction)n, false}
#define HV(n) {"vk" #n, (PFN_vkVoidFunction)n, true}
const Hook kDeviceHooks[] = {
    H(DestroyDevice), H(GetDeviceQueue), H(GetDeviceQueue2), H(QueueSubmit), H(QueueSubmit2), {"vkQueueSubmit2KHR", (PFN_vkVoidFunction)QueueSubmit2, false},
    H(QueueWaitIdle), H(DeviceWaitIdle), H(QueuePresentKHR), H(QueueBindSparse), H(CreateCommandPool), H(DestroyCommandPool),
    H(ResetCommandPool), H(TrimCommandPool), {"vkTrimCommandPoolKHR", (PFN_vkVoidFunction)TrimCommandPool, false}, H(AllocateCommandBuffers),
    H(FreeCommandBuffers), H(BeginCommandBuffer), H(EndCommandBuffer), H(ResetCommandBuffer), H(CmdPipelineBarrier), H(CmdPipelineBarrier2),
    {"vkCmdPipelineBarrier2KHR", (PFN_vkVoidFunction)CmdPipelineBarrier2, false}, H(CmdWaitEvents), H(CmdWaitEvents2),
    {"vkCmdWaitEvents2KHR", (PFN_vkVoidFunction)CmdWaitEvents2, false}, H(CmdSetEvent2), {"vkCmdSetEvent2KHR", (PFN_vkVoidFunction)CmdSetEvent2, false},
    H(CmdResetQueryPool), H(CmdBeginQuery), H(CmdEndQuery), H(CmdWriteTimestamp), H(CmdWriteTimestamp2),
    {"vkCmdWriteTimestamp2KHR", (PFN_vkVoidFunction)CmdWriteTimestamp2, false}, H(CmdCopyQueryPoolResults), H(CreateImage), H(DestroyImage),
    H(CreateImageView), H(DestroyImageView), H(BindImageMemory2), {"vkBindImageMemory2KHR", (PFN_vkVoidFunction)BindImageMemory2, false}, H(CreateBuffer), H(GetDeviceImageMemoryRequirements),
    {"vkGetDeviceImageMemoryRequirementsKHR", (PFN_vkVoidFunction)GetDeviceImageMemoryRequirements, false}, H(GetDeviceBufferMemoryRequirements),
    {"vkGetDeviceBufferMemoryRequirementsKHR", (PFN_vkVoidFunction)GetDeviceBufferMemoryRequirements, false}, H(CreateQueryPool),
    H(DestroyQueryPool), H(GetQueryPoolResults), H(ResetQueryPool), {"vkResetQueryPoolEXT", (PFN_vkVoidFunction)ResetQueryPool, false},
    H(SetDebugUtilsObjectNameEXT), H(SetDebugUtilsObjectTagEXT), H(CmdBeginDebugUtilsLabelEXT), H(CmdEndDebugUtilsLabelEXT),
    H(CmdInsertDebugUtilsLabelEXT), H(QueueBeginDebugUtilsLabelEXT), H(QueueEndDebugUtilsLabelEXT), H(QueueInsertDebugUtilsLabelEXT),
    HV(CreateVideoSessionKHR), HV(DestroyVideoSessionKHR), HV(GetVideoSessionMemoryRequirementsKHR), HV(BindVideoSessionMemoryKHR),
    HV(CreateVideoSessionParametersKHR), HV(UpdateVideoSessionParametersKHR), HV(DestroyVideoSessionParametersKHR),
    HV(GetEncodedVideoSessionParametersKHR), HV(CmdBeginVideoCodingKHR), HV(CmdEndVideoCodingKHR), HV(CmdControlVideoCodingKHR),
    HV(CmdEncodeVideoKHR),
};
const Hook kInstanceHooks[] = {
    H(CreateInstance), H(DestroyInstance), H(CreateDevice), H(EnumerateDeviceExtensionProperties),
    H(GetPhysicalDeviceQueueFamilyProperties), H(GetPhysicalDeviceQueueFamilyProperties2),
    {"vkGetPhysicalDeviceQueueFamilyProperties2KHR", (PFN_vkVoidFunction)GetPhysicalDeviceQueueFamilyProperties2, false},
    H(GetPhysicalDeviceFeatures2), {"vkGetPhysicalDeviceFeatures2KHR", (PFN_vkVoidFunction)GetPhysicalDeviceFeatures2, false},
    H(GetPhysicalDeviceFormatProperties), H(GetPhysicalDeviceFormatProperties2),
    {"vkGetPhysicalDeviceFormatProperties2KHR", (PFN_vkVoidFunction)GetPhysicalDeviceFormatProperties2, false},
    H(GetPhysicalDeviceImageFormatProperties2),
    {"vkGetPhysicalDeviceImageFormatProperties2KHR", (PFN_vkVoidFunction)GetPhysicalDeviceImageFormatProperties2, false},
    H(GetPhysicalDeviceVideoCapabilitiesKHR), H(GetPhysicalDeviceVideoFormatPropertiesKHR),
    H(GetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR),
};
#undef H
#undef HV

PFN_vkVoidFunction find_hook(const Hook *t, size_t n, const char *name, bool *needs_active)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(t[i].name, name)) {
            if (needs_active)
                *needs_active = t[i].needs_active;
            return t[i].fn;
        }
    return nullptr;
}

}  // namespace

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *name)
{
    Device *d = dev_of(dev);
    // a device without video encode: the driver's functions, the layer stays out of the way (DestroyDevice only)
    if (d && !d->active && strcmp(name, "vkDestroyDevice") && strcmp(name, "vkGetDeviceProcAddr"))
        return d->gdpa(dev, name);
    bool va = false;
    PFN_vkVoidFunction f = find_hook(kDeviceHooks, sizeof(kDeviceHooks) / sizeof(kDeviceHooks[0]), name, &va);
    if (f && (!va || (d && d->active)))
        return f;
    return d ? d->gdpa(dev, name) : nullptr;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char *name)
{
    if (PFN_vkVoidFunction f = find_hook(kInstanceHooks, sizeof(kInstanceHooks) / sizeof(kInstanceHooks[0]), name, nullptr))
        return f;
    if (PFN_vkVoidFunction f = find_hook(kDeviceHooks, sizeof(kDeviceHooks) / sizeof(kDeviceHooks[0]), name, nullptr))
        return f;
    if (!inst)
        return nullptr;
    Instance *in = inst_of(inst);
    return in ? in->gipa(inst, name) : nullptr;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (v->loaderLayerInterfaceVersion > 2)
        v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
