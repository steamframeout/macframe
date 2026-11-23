// H.264 parameter sets and slice-header rewriting for the encode layer.
//
// Applications write their own SPS/PPS (FFmpeg only takes PPS overrides; vrlink may take none), so the slices a
// backend produces must follow the application's parameter sets, not the backend's. Slice data (the macroblock
// layer) only depends on a few parameter-set fields; everything else lives in the slice header, which is
// rewritten here: frame_num and POC lengths and values, parameter set ids, reference counts, QP delta, ...
// CABAC slice data is byte aligned and copied as is; CAVLC slice data is bit-shifted.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace venc {

struct BitReader {
    const uint8_t *d;
    size_t n;      // bytes
    size_t pos = 0;  // bits
    bool err = false;
    BitReader(const uint8_t *data, size_t size) : d(data), n(size) {}
    uint32_t u(int bits);
    uint32_t ue();
    int32_t se();
    bool aligned() const { return (pos & 7) == 0; }
    size_t left() const { return pos <= n * 8 ? n * 8 - pos : 0; }
};

struct BitWriter {
    std::vector<uint8_t> b;
    uint32_t acc = 0;
    int nacc = 0;
    void u(uint32_t v, int bits);
    void ue(uint32_t v);
    void se(int32_t v);
    void align_zero();
    void align_one();
    void trailing();   // rbsp_trailing_bits
    void bits_from(BitReader &r, size_t count);
    bool aligned() const { return nacc == 0; }
};

std::vector<uint8_t> unescape(const uint8_t *nal, size_t n);   // NAL payload -> RBSP
void escape_append(std::vector<uint8_t> &out, const std::vector<uint8_t> &rbsp);  // adds emulation prevention

// Parsed fields the rewriter needs.
struct Sps {
    uint32_t id = 0, profile_idc = 0, chroma_format_idc = 1, bit_depth_luma = 8, bit_depth_chroma = 8;
    bool separate_colour_plane = false, scaling_matrix = false, qpprime_bypass = false;
    uint32_t log2_max_frame_num = 4, poc_type = 0, log2_max_poc_lsb = 4;
    bool delta_pic_order_always_zero = false;
    uint32_t max_num_ref_frames = 1, width_mbs = 0, height_map_units = 0;
    bool frame_mbs_only = true, mb_adaptive = false, direct_8x8 = true;
};
struct Pps {
    uint32_t id = 0, sps_id = 0;
    bool cabac = false, bottom_field_pic_order = false;
    uint32_t num_slice_groups = 1, num_ref_l0 = 1, num_ref_l1 = 1;
    bool weighted_pred = false;
    uint32_t weighted_bipred_idc = 0;
    int32_t pic_init_qp = 26, chroma_qp_offset = 0, second_chroma_qp_offset = 0;
    bool deblocking_control = false, constrained_intra = false, redundant_pic_cnt = false, transform_8x8 = false;
    bool scaling_matrix = false;
};

bool parse_sps(const uint8_t *nal, size_t n, Sps &s);   // nal: starting at the NAL header byte
bool parse_pps(const uint8_t *nal, size_t n, const Sps &sps, Pps &p);

Sps sps_from_std(const StdVideoH264SequenceParameterSet &s);
Pps pps_from_std(const StdVideoH264PictureParameterSet &p);

// Writes NAL units (with a 4-byte start code) from the Vulkan Std structures.
void write_sps(std::vector<uint8_t> &out, const StdVideoH264SequenceParameterSet &s);
void write_pps(std::vector<uint8_t> &out, const StdVideoH264PictureParameterSet &p, const StdVideoH264SequenceParameterSet &sps);

// Slice-data compatibility: returns an empty string when slices coded for (enc_sps, enc_pps) decode the same under
// (app_sps, app_pps) once their headers are rewritten, else what differs.
std::string incompatibility(const Sps &enc_sps, const Pps &enc_pps, const Sps &app_sps, const Pps &app_pps);

struct PicInfo {      // per picture, from VkVideoEncodeH264PictureInfoKHR
    bool have = false;
    uint32_t frame_num = 0, idr_pic_id = 0;
    int32_t poc = 0;
};

// Rewrites one slice NAL unit (payload after the start code). Appends the result with a 4-byte start code.
bool rewrite_slice(const uint8_t *nal, size_t n, const Sps &enc_sps, const Pps &enc_pps, const Sps &app_sps, const Pps &app_pps,
                   const PicInfo &pic, std::vector<uint8_t> &out, std::string *err);

}  // namespace venc
