// H.265 parameter sets and slice-segment-header rewriting for the encode layer (the H.264 counterpart is
// h264_rewrite.h; same idea). The application's VPS/SPS are written as it gave them; its PPS takes the encoder's
// coding tools (reported as overrides); every slice segment header the backend produced is rewritten for the
// application's parameter sets: POC length and value, reference picture set (explicit, with the application's POC
// numbering), parameter set ids, reference counts, QP delta, SAO/deblocking/temporal-MVP flags. Slice data is CABAC,
// byte aligned after the header: copied unchanged.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "h264_rewrite.h"   // BitReader, BitWriter, unescape, escape_append

namespace venc {
namespace h265 {

struct Rps {
    uint32_t num_neg = 0, num_pos = 0;
    int32_t delta_neg[16] = {}, delta_pos[16] = {};   // POC deltas (negative / positive)
    bool used_neg[16] = {}, used_pos[16] = {};
};

struct Sps {
    uint32_t id = 0, vps_id = 0, max_sub_layers = 1, chroma_format_idc = 1;
    uint32_t width = 0, height = 0, bit_depth_luma = 8, bit_depth_chroma = 8;
    uint32_t log2_max_poc_lsb = 4;
    uint32_t log2_min_cb = 3, log2_ctb = 4, log2_min_tb = 2, log2_max_tb = 5, depth_inter = 0, depth_intra = 0;
    bool scaling_list = false, amp = false, sao = false, pcm = false, long_term = false, tmvp = false, strong_intra = false;
    uint32_t pcm_bits = 0;
    std::vector<Rps> rps;   // num_short_term_ref_pic_sets
    uint32_t num_long_term_sps = 0;
};

struct Pps {
    uint32_t id = 0, sps_id = 0;
    bool dependent_slices = false, output_flag = false, sign_data_hiding = false, cabac_init_present = false;
    uint32_t num_extra_bits = 0, num_ref_l0 = 1, num_ref_l1 = 1;
    int32_t init_qp = 26, cb_qp_offset = 0, cr_qp_offset = 0;
    bool constrained_intra = false, transform_skip = false, cu_qp_delta = false;
    uint32_t diff_cu_qp_delta_depth = 0;
    bool slice_chroma_qp_offsets = false, weighted_pred = false, weighted_bipred = false, transquant_bypass = false;
    bool tiles = false, entropy_sync = false, loop_filter_across_slices = false;
    bool deblocking_control = false, deblocking_override = false, deblocking_disabled = false;
    int32_t beta_offset_div2 = 0, tc_offset_div2 = 0;
    bool scaling_list = false, lists_modification = false, header_extension = false;
    uint32_t log2_parallel_merge = 2;
};

bool parse_sps(const uint8_t *nal, size_t n, Sps &s);   // nal: from the 2-byte NAL header
bool parse_pps(const uint8_t *nal, size_t n, Pps &p);

Sps sps_from_std(const StdVideoH265SequenceParameterSet &s);
Pps pps_from_std(const StdVideoH265PictureParameterSet &p);

void write_vps(std::vector<uint8_t> &out, const StdVideoH265VideoParameterSet &v);
void write_sps(std::vector<uint8_t> &out, const StdVideoH265SequenceParameterSet &s);
void write_pps(std::vector<uint8_t> &out, const StdVideoH265PictureParameterSet &p);

// The application's PPS with the coding tools of the encoder's; true if anything changed.
bool override_pps(StdVideoH265PictureParameterSet &p, const Pps &enc);

std::string incompatibility(const Sps &es, const Pps &ep, const Sps &as, const Pps &ap);

// POC bookkeeping across pictures: the encoder's POC of each recent picture -> the application's.
struct PocMap {
    int32_t enc_prev_tid0 = 0;
    std::map<int32_t, int32_t> enc_to_app;
    void reset() { enc_to_app.clear(); enc_prev_tid0 = 0; }
};

struct PicInfo {
    bool have = false;
    int32_t poc = 0;   // PicOrderCntVal
};

// Rewrites one slice segment NAL unit; appends it with a 4-byte start code.
bool rewrite_slice(const uint8_t *nal, size_t n, const Sps &es, const Pps &ep, const Sps &as, const Pps &ap, const PicInfo &pic,
                   PocMap &pocs, std::vector<uint8_t> &out, std::string *err);

}  // namespace h265
}  // namespace venc
