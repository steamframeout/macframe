// H.265 parameter sets and slice-segment-header rewriting (see h265_rewrite.h).
#include "h265_rewrite.h"

#include <string.h>

namespace venc {
namespace h265 {

static void start_code(std::vector<uint8_t> &out)
{
    static const uint8_t sc[4] = {0, 0, 0, 1};
    out.insert(out.end(), sc, sc + 4);
}

static void finish_nal(std::vector<uint8_t> &out, BitWriter &w)
{
    w.trailing();
    start_code(out);
    out.push_back(w.b[0]);
    out.push_back(w.b[1]);
    escape_append(out, std::vector<uint8_t>(w.b.begin() + 2, w.b.end()));
}

static uint32_t ceil_log2(uint32_t v)
{
    uint32_t r = 0;
    while ((1u << r) < v)
        r++;
    return r;
}

// ------------------------------------------------------------------------------------------------- parsing
static void skip_ptl(BitReader &r, bool profile_present, uint32_t max_sub_layers_minus1, uint32_t *profile = nullptr)
{
    if (profile_present) {
        r.u(2);
        r.u(1);
        uint32_t p = r.u(5);
        if (profile)
            *profile = p;
        r.u(32);
        r.u(4);
        r.u(32);   // 43 constraint bits + 1 = 44 bits after the 4 source flags
        r.u(11);
        r.u(1);
    }
    r.u(8);   // general_level_idc
    bool sub_profile[8] = {}, sub_level[8] = {};
    for (uint32_t i = 0; i < max_sub_layers_minus1; i++) {
        sub_profile[i] = r.u(1);
        sub_level[i] = r.u(1);
    }
    if (max_sub_layers_minus1 > 0)
        for (uint32_t i = max_sub_layers_minus1; i < 8; i++)
            r.u(2);
    for (uint32_t i = 0; i < max_sub_layers_minus1; i++) {
        if (sub_profile[i]) {
            r.u(32);
            r.u(32);
            r.u(24);
        }
        if (sub_level[i])
            r.u(8);
    }
}

static void skip_scaling_list_data(BitReader &r)
{
    for (int size = 0; size < 4; size++)
        for (int m = 0; m < 6; m += (size == 3) ? 3 : 1) {
            if (!r.u(1)) {
                r.ue();
            } else {
                int n = std::min(64, 1 << (4 + (size << 1)));
                if (size > 1)
                    r.se();
                for (int i = 0; i < n; i++)
                    r.se();
            }
        }
}

// st_ref_pic_set(idx) (7.3.7) with the derivation of 7.4.8; `sets` holds the sets before idx.
static bool parse_rps(BitReader &r, uint32_t idx, uint32_t num_sps_sets, const std::vector<Rps> &sets, Rps &out)
{
    out = Rps{};
    bool inter = idx != 0 && r.u(1);
    if (inter) {
        uint32_t delta_idx = 1;
        if (idx == num_sps_sets)
            delta_idx = r.ue() + 1;
        if (delta_idx > idx)
            return false;
        const Rps &ref = sets[idx - delta_idx];
        uint32_t sign = r.u(1);
        int32_t delta_rps = (1 - 2 * (int32_t)sign) * (int32_t)(r.ue() + 1);
        uint32_t n = ref.num_neg + ref.num_pos;
        bool used[33] = {}, use_delta[33] = {};
        for (uint32_t j = 0; j <= n; j++) {
            used[j] = r.u(1);
            use_delta[j] = used[j] ? true : r.u(1);
        }
        // 7-61 / 7-62
        auto dpoc = [&](uint32_t j) { return j < ref.num_neg ? ref.delta_neg[j] : ref.delta_pos[j - ref.num_neg]; };
        uint32_t i = 0;
        for (int32_t j = (int32_t)ref.num_pos - 1; j >= 0; j--) {
            int32_t d = ref.delta_pos[j] + delta_rps;
            uint32_t k = ref.num_neg + (uint32_t)j;
            if (d < 0 && use_delta[k] && i < 16) {
                out.delta_neg[i] = d;
                out.used_neg[i++] = used[k];
            }
        }
        if (delta_rps < 0 && use_delta[n] && i < 16) {
            out.delta_neg[i] = delta_rps;
            out.used_neg[i++] = used[n];
        }
        for (uint32_t j = 0; j < ref.num_neg; j++) {
            int32_t d = ref.delta_neg[j] + delta_rps;
            if (d < 0 && use_delta[j] && i < 16) {
                out.delta_neg[i] = d;
                out.used_neg[i++] = used[j];
            }
        }
        out.num_neg = i;
        i = 0;
        for (int32_t j = (int32_t)ref.num_neg - 1; j >= 0; j--) {
            int32_t d = ref.delta_neg[j] + delta_rps;
            if (d > 0 && use_delta[j] && i < 16) {
                out.delta_pos[i] = d;
                out.used_pos[i++] = used[j];
            }
        }
        if (delta_rps > 0 && use_delta[n] && i < 16) {
            out.delta_pos[i] = delta_rps;
            out.used_pos[i++] = used[n];
        }
        for (uint32_t j = 0; j < ref.num_pos; j++) {
            int32_t d = ref.delta_pos[j] + delta_rps;
            uint32_t k = ref.num_neg + j;
            if (d > 0 && use_delta[k] && i < 16) {
                out.delta_pos[i] = d;
                out.used_pos[i++] = used[k];
            }
        }
        out.num_pos = i;
        (void)dpoc;
    } else {
        out.num_neg = r.ue();
        out.num_pos = r.ue();
        if (out.num_neg > 16 || out.num_pos > 16)
            return false;
        int32_t prev = 0;
        for (uint32_t i = 0; i < out.num_neg; i++) {
            prev -= (int32_t)(r.ue() + 1);
            out.delta_neg[i] = prev;
            out.used_neg[i] = r.u(1);
        }
        prev = 0;
        for (uint32_t i = 0; i < out.num_pos; i++) {
            prev += (int32_t)(r.ue() + 1);
            out.delta_pos[i] = prev;
            out.used_pos[i] = r.u(1);
        }
    }
    return !r.err;
}

bool parse_sps(const uint8_t *nal, size_t n, Sps &s)
{
    if (n < 3 || ((nal[0] >> 1) & 0x3f) != 33)
        return false;
    std::vector<uint8_t> rb = unescape(nal + 2, n - 2);
    BitReader r(rb.data(), rb.size());
    s = Sps{};
    s.vps_id = r.u(4);
    uint32_t msl = r.u(3);
    s.max_sub_layers = msl + 1;
    r.u(1);
    skip_ptl(r, true, msl);
    s.id = r.ue();
    s.chroma_format_idc = r.ue();
    if (s.chroma_format_idc == 3)
        r.u(1);
    s.width = r.ue();
    s.height = r.ue();
    if (r.u(1)) {   // conformance window: display only
        r.ue();
        r.ue();
        r.ue();
        r.ue();
    }
    s.bit_depth_luma = 8 + r.ue();
    s.bit_depth_chroma = 8 + r.ue();
    s.log2_max_poc_lsb = 4 + r.ue();
    bool ordering = r.u(1);
    for (uint32_t i = ordering ? 0 : msl; i <= msl; i++) {
        r.ue();
        r.ue();
        r.ue();
    }
    s.log2_min_cb = 3 + r.ue();
    s.log2_ctb = s.log2_min_cb + r.ue();
    s.log2_min_tb = 2 + r.ue();
    s.log2_max_tb = s.log2_min_tb + r.ue();
    s.depth_inter = r.ue();
    s.depth_intra = r.ue();
    s.scaling_list = r.u(1);
    if (s.scaling_list && r.u(1))
        skip_scaling_list_data(r);
    s.amp = r.u(1);
    s.sao = r.u(1);
    s.pcm = r.u(1);
    if (s.pcm) {
        s.pcm_bits = r.u(4);
        r.u(4);
        r.ue();
        r.ue();
        r.u(1);
    }
    uint32_t nsets = r.ue();
    if (nsets > 64)
        return false;
    for (uint32_t i = 0; i < nsets; i++) {
        Rps rps;
        if (!parse_rps(r, i, nsets, s.rps, rps))
            return false;
        s.rps.push_back(rps);
    }
    s.long_term = r.u(1);
    if (s.long_term) {
        s.num_long_term_sps = r.ue();
        for (uint32_t i = 0; i < s.num_long_term_sps; i++) {
            r.u((int)s.log2_max_poc_lsb);
            r.u(1);
        }
    }
    s.tmvp = r.u(1);
    s.strong_intra = r.u(1);
    return !r.err;   // VUI and extensions: not needed
}

bool parse_pps(const uint8_t *nal, size_t n, Pps &p)
{
    if (n < 3 || ((nal[0] >> 1) & 0x3f) != 34)
        return false;
    std::vector<uint8_t> rb = unescape(nal + 2, n - 2);
    BitReader r(rb.data(), rb.size());
    p = Pps{};
    p.id = r.ue();
    p.sps_id = r.ue();
    p.dependent_slices = r.u(1);
    p.output_flag = r.u(1);
    p.num_extra_bits = r.u(3);
    p.sign_data_hiding = r.u(1);
    p.cabac_init_present = r.u(1);
    p.num_ref_l0 = r.ue() + 1;
    p.num_ref_l1 = r.ue() + 1;
    p.init_qp = 26 + r.se();
    p.constrained_intra = r.u(1);
    p.transform_skip = r.u(1);
    p.cu_qp_delta = r.u(1);
    if (p.cu_qp_delta)
        p.diff_cu_qp_delta_depth = r.ue();
    p.cb_qp_offset = r.se();
    p.cr_qp_offset = r.se();
    p.slice_chroma_qp_offsets = r.u(1);
    p.weighted_pred = r.u(1);
    p.weighted_bipred = r.u(1);
    p.transquant_bypass = r.u(1);
    p.tiles = r.u(1);
    p.entropy_sync = r.u(1);
    if (p.tiles) {
        uint32_t cols = r.ue(), rows = r.ue();
        if (!r.u(1)) {
            for (uint32_t i = 0; i < cols; i++)
                r.ue();
            for (uint32_t i = 0; i < rows; i++)
                r.ue();
        }
        r.u(1);
    }
    p.loop_filter_across_slices = r.u(1);
    p.deblocking_control = r.u(1);
    if (p.deblocking_control) {
        p.deblocking_override = r.u(1);
        p.deblocking_disabled = r.u(1);
        if (!p.deblocking_disabled) {
            p.beta_offset_div2 = r.se();
            p.tc_offset_div2 = r.se();
        }
    }
    p.scaling_list = r.u(1);
    if (p.scaling_list)
        skip_scaling_list_data(r);
    p.lists_modification = r.u(1);
    p.log2_parallel_merge = 2 + r.ue();
    p.header_extension = r.u(1);
    return !r.err;
}

Sps sps_from_std(const StdVideoH265SequenceParameterSet &s)
{
    Sps o;
    o.id = s.sps_seq_parameter_set_id;
    o.vps_id = s.sps_video_parameter_set_id;
    o.max_sub_layers = s.sps_max_sub_layers_minus1 + 1u;
    o.chroma_format_idc = s.chroma_format_idc;
    o.width = s.pic_width_in_luma_samples;
    o.height = s.pic_height_in_luma_samples;
    o.bit_depth_luma = 8 + s.bit_depth_luma_minus8;
    o.bit_depth_chroma = 8 + s.bit_depth_chroma_minus8;
    o.log2_max_poc_lsb = 4 + s.log2_max_pic_order_cnt_lsb_minus4;
    o.log2_min_cb = 3 + s.log2_min_luma_coding_block_size_minus3;
    o.log2_ctb = o.log2_min_cb + s.log2_diff_max_min_luma_coding_block_size;
    o.log2_min_tb = 2 + s.log2_min_luma_transform_block_size_minus2;
    o.log2_max_tb = o.log2_min_tb + s.log2_diff_max_min_luma_transform_block_size;
    o.depth_inter = s.max_transform_hierarchy_depth_inter;
    o.depth_intra = s.max_transform_hierarchy_depth_intra;
    o.scaling_list = s.flags.scaling_list_enabled_flag;
    o.amp = s.flags.amp_enabled_flag;
    o.sao = s.flags.sample_adaptive_offset_enabled_flag;
    o.pcm = s.flags.pcm_enabled_flag;
    o.pcm_bits = s.pcm_sample_bit_depth_luma_minus1;
    o.long_term = s.flags.long_term_ref_pics_present_flag;
    o.num_long_term_sps = s.num_long_term_ref_pics_sps;
    o.tmvp = s.flags.sps_temporal_mvp_enabled_flag;
    o.strong_intra = s.flags.strong_intra_smoothing_enabled_flag;
    for (uint32_t i = 0; i < s.num_short_term_ref_pic_sets && s.pShortTermRefPicSet; i++) {
        const StdVideoH265ShortTermRefPicSet &x = s.pShortTermRefPicSet[i];
        Rps r;
        // explicit sets only (applications use those; inter-predicted SPS sets are left empty)
        if (!x.flags.inter_ref_pic_set_prediction_flag) {
            r.num_neg = std::min<uint32_t>(x.num_negative_pics, 16);
            r.num_pos = std::min<uint32_t>(x.num_positive_pics, 16);
            int32_t prev = 0;
            for (uint32_t k = 0; k < r.num_neg; k++) {
                prev -= x.delta_poc_s0_minus1[k] + 1;
                r.delta_neg[k] = prev;
                r.used_neg[k] = (x.used_by_curr_pic_s0_flag >> k) & 1;
            }
            prev = 0;
            for (uint32_t k = 0; k < r.num_pos; k++) {
                prev += x.delta_poc_s1_minus1[k] + 1;
                r.delta_pos[k] = prev;
                r.used_pos[k] = (x.used_by_curr_pic_s1_flag >> k) & 1;
            }
        }
        o.rps.push_back(r);
    }
    return o;
}

Pps pps_from_std(const StdVideoH265PictureParameterSet &p)
{
    Pps o;
    o.id = p.pps_pic_parameter_set_id;
    o.sps_id = p.pps_seq_parameter_set_id;
    o.dependent_slices = p.flags.dependent_slice_segments_enabled_flag;
    o.output_flag = p.flags.output_flag_present_flag;
    o.num_extra_bits = p.num_extra_slice_header_bits;
    o.sign_data_hiding = p.flags.sign_data_hiding_enabled_flag;
    o.cabac_init_present = p.flags.cabac_init_present_flag;
    o.num_ref_l0 = p.num_ref_idx_l0_default_active_minus1 + 1u;
    o.num_ref_l1 = p.num_ref_idx_l1_default_active_minus1 + 1u;
    o.init_qp = 26 + p.init_qp_minus26;
    o.constrained_intra = p.flags.constrained_intra_pred_flag;
    o.transform_skip = p.flags.transform_skip_enabled_flag;
    o.cu_qp_delta = p.flags.cu_qp_delta_enabled_flag;
    o.diff_cu_qp_delta_depth = p.diff_cu_qp_delta_depth;
    o.cb_qp_offset = p.pps_cb_qp_offset;
    o.cr_qp_offset = p.pps_cr_qp_offset;
    o.slice_chroma_qp_offsets = p.flags.pps_slice_chroma_qp_offsets_present_flag;
    o.weighted_pred = p.flags.weighted_pred_flag;
    o.weighted_bipred = p.flags.weighted_bipred_flag;
    o.transquant_bypass = p.flags.transquant_bypass_enabled_flag;
    o.tiles = p.flags.tiles_enabled_flag;
    o.entropy_sync = p.flags.entropy_coding_sync_enabled_flag;
    o.loop_filter_across_slices = p.flags.pps_loop_filter_across_slices_enabled_flag;
    o.deblocking_control = p.flags.deblocking_filter_control_present_flag;
    o.deblocking_override = p.flags.deblocking_filter_override_enabled_flag;
    o.deblocking_disabled = p.flags.pps_deblocking_filter_disabled_flag;
    o.beta_offset_div2 = p.pps_beta_offset_div2;
    o.tc_offset_div2 = p.pps_tc_offset_div2;
    o.scaling_list = p.flags.pps_scaling_list_data_present_flag;
    o.lists_modification = p.flags.lists_modification_present_flag;
    o.log2_parallel_merge = 2 + p.log2_parallel_merge_level_minus2;
    o.header_extension = p.flags.slice_segment_header_extension_present_flag;
    return o;
}

// ------------------------------------------------------------------------------------------------- writing
static uint32_t level_number(StdVideoH265LevelIdc l)
{
    static const uint32_t t[] = {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
    return (uint32_t)l < sizeof(t) / sizeof(t[0]) ? t[l] : 123;
}

static void write_ptl(BitWriter &w, const StdVideoH265ProfileTierLevel *ptl, uint32_t max_sub_layers_minus1)
{
    uint32_t profile = ptl ? (uint32_t)ptl->general_profile_idc : 1;
    if (profile > 31)
        profile = 1;
    w.u(0, 2);                                  // general_profile_space
    w.u(ptl ? ptl->flags.general_tier_flag : 0, 1);
    w.u(profile, 5);
    uint32_t compat = 1u << (31 - profile);
    if (profile == 1)
        compat |= 1u << (31 - 2);               // Main streams are also Main 10 streams
    w.u(compat, 32);
    w.u(ptl ? ptl->flags.general_progressive_source_flag : 1, 1);
    w.u(ptl ? ptl->flags.general_interlaced_source_flag : 0, 1);
    w.u(ptl ? ptl->flags.general_non_packed_constraint_flag : 0, 1);
    w.u(ptl ? ptl->flags.general_frame_only_constraint_flag : 1, 1);
    w.u(0, 32);                                 // 43 reserved/constraint bits + general_inbld_flag
    w.u(0, 12);
    w.u(ptl ? level_number(ptl->general_level_idc) : 123, 8);
    for (uint32_t i = 0; i < max_sub_layers_minus1; i++)
        w.u(0, 2);                              // no sub-layer profile/level
    if (max_sub_layers_minus1 > 0)
        for (uint32_t i = max_sub_layers_minus1; i < 8; i++)
            w.u(0, 2);
}

static void write_rps_explicit(BitWriter &w, const Rps &r, bool inter_flag_present)
{
    if (inter_flag_present)
        w.u(0, 1);
    w.ue(r.num_neg);
    w.ue(r.num_pos);
    int32_t prev = 0;
    for (uint32_t i = 0; i < r.num_neg; i++) {
        w.ue((uint32_t)(prev - r.delta_neg[i] - 1));
        prev = r.delta_neg[i];
        w.u(r.used_neg[i], 1);
    }
    prev = 0;
    for (uint32_t i = 0; i < r.num_pos; i++) {
        w.ue((uint32_t)(r.delta_pos[i] - prev - 1));
        prev = r.delta_pos[i];
        w.u(r.used_pos[i], 1);
    }
}

static void write_std_rps(BitWriter &w, const StdVideoH265ShortTermRefPicSet &x, uint32_t idx, uint32_t num_sets)
{
    if (idx != 0)
        w.u(x.flags.inter_ref_pic_set_prediction_flag, 1);
    if (x.flags.inter_ref_pic_set_prediction_flag) {
        if (idx == num_sets)
            w.ue(x.delta_idx_minus1);
        w.u(x.flags.delta_rps_sign, 1);
        w.ue(x.abs_delta_rps_minus1);
        // NumDeltaPocs of the referenced set: its used/use_delta bit masks carry one bit per entry (+1)
        for (uint32_t j = 0; j < 16; j++) {
            bool used = (x.used_by_curr_pic_flag >> j) & 1;
            w.u(used, 1);
            if (!used)
                w.u((x.use_delta_flag >> j) & 1, 1);
            if (((x.used_by_curr_pic_flag | x.use_delta_flag) >> (j + 1)) == 0)
                break;
        }
        return;
    }
    w.ue(x.num_negative_pics);
    w.ue(x.num_positive_pics);
    for (uint32_t i = 0; i < x.num_negative_pics; i++) {
        w.ue(x.delta_poc_s0_minus1[i]);
        w.u((x.used_by_curr_pic_s0_flag >> i) & 1, 1);
    }
    for (uint32_t i = 0; i < x.num_positive_pics; i++) {
        w.ue(x.delta_poc_s1_minus1[i]);
        w.u((x.used_by_curr_pic_s1_flag >> i) & 1, 1);
    }
}

void write_vps(std::vector<uint8_t> &out, const StdVideoH265VideoParameterSet &v)
{
    BitWriter w;
    w.u(32 << 1, 8);   // NAL header: type 32 (VPS), layer 0, tid 1
    w.u(1, 8);
    w.u(v.vps_video_parameter_set_id, 4);
    w.u(1, 1);         // vps_base_layer_internal_flag
    w.u(1, 1);         // vps_base_layer_available_flag
    w.u(0, 6);         // vps_max_layers_minus1
    w.u(v.vps_max_sub_layers_minus1, 3);
    w.u(v.flags.vps_temporal_id_nesting_flag, 1);
    w.u(0xffff, 16);
    write_ptl(w, v.pProfileTierLevel, v.vps_max_sub_layers_minus1);
    bool ordering = v.flags.vps_sub_layer_ordering_info_present_flag;
    w.u(ordering, 1);
    for (uint32_t i = ordering ? 0 : v.vps_max_sub_layers_minus1; i <= v.vps_max_sub_layers_minus1; i++) {
        w.ue(v.pDecPicBufMgr ? v.pDecPicBufMgr->max_dec_pic_buffering_minus1[i] : 1);
        w.ue(v.pDecPicBufMgr ? v.pDecPicBufMgr->max_num_reorder_pics[i] : 0);
        w.ue(v.pDecPicBufMgr ? v.pDecPicBufMgr->max_latency_increase_plus1[i] : 0);
    }
    w.u(0, 6);         // vps_max_layer_id
    w.ue(0);           // vps_num_layer_sets_minus1
    w.u(v.flags.vps_timing_info_present_flag, 1);
    if (v.flags.vps_timing_info_present_flag) {
        w.u(v.vps_num_units_in_tick, 32);
        w.u(v.vps_time_scale, 32);
        w.u(v.flags.vps_poc_proportional_to_timing_flag, 1);
        if (v.flags.vps_poc_proportional_to_timing_flag)
            w.ue(v.vps_num_ticks_poc_diff_one_minus1);
        w.ue(0);       // vps_num_hrd_parameters (HRD is not carried over)
    }
    w.u(0, 1);         // vps_extension_flag
    finish_nal(out, w);
}

void write_sps(std::vector<uint8_t> &out, const StdVideoH265SequenceParameterSet &s)
{
    BitWriter w;
    w.u(33 << 1, 8);
    w.u(1, 8);
    w.u(s.sps_video_parameter_set_id, 4);
    w.u(s.sps_max_sub_layers_minus1, 3);
    w.u(s.flags.sps_temporal_id_nesting_flag, 1);
    write_ptl(w, s.pProfileTierLevel, s.sps_max_sub_layers_minus1);
    w.ue(s.sps_seq_parameter_set_id);
    w.ue(s.chroma_format_idc);
    if (s.chroma_format_idc == 3)
        w.u(s.flags.separate_colour_plane_flag, 1);
    w.ue(s.pic_width_in_luma_samples);
    w.ue(s.pic_height_in_luma_samples);
    w.u(s.flags.conformance_window_flag, 1);
    if (s.flags.conformance_window_flag) {
        w.ue(s.conf_win_left_offset);
        w.ue(s.conf_win_right_offset);
        w.ue(s.conf_win_top_offset);
        w.ue(s.conf_win_bottom_offset);
    }
    w.ue(s.bit_depth_luma_minus8);
    w.ue(s.bit_depth_chroma_minus8);
    w.ue(s.log2_max_pic_order_cnt_lsb_minus4);
    bool ordering = s.flags.sps_sub_layer_ordering_info_present_flag;
    w.u(ordering, 1);
    for (uint32_t i = ordering ? 0 : s.sps_max_sub_layers_minus1; i <= s.sps_max_sub_layers_minus1; i++) {
        w.ue(s.pDecPicBufMgr ? s.pDecPicBufMgr->max_dec_pic_buffering_minus1[i] : 1);
        w.ue(s.pDecPicBufMgr ? s.pDecPicBufMgr->max_num_reorder_pics[i] : 0);
        w.ue(s.pDecPicBufMgr ? s.pDecPicBufMgr->max_latency_increase_plus1[i] : 0);
    }
    w.ue(s.log2_min_luma_coding_block_size_minus3);
    w.ue(s.log2_diff_max_min_luma_coding_block_size);
    w.ue(s.log2_min_luma_transform_block_size_minus2);
    w.ue(s.log2_diff_max_min_luma_transform_block_size);
    w.ue(s.max_transform_hierarchy_depth_inter);
    w.ue(s.max_transform_hierarchy_depth_intra);
    w.u(s.flags.scaling_list_enabled_flag, 1);
    if (s.flags.scaling_list_enabled_flag)
        w.u(0, 1);     // sps_scaling_list_data_present_flag: default lists (explicit lists are not carried over)
    w.u(s.flags.amp_enabled_flag, 1);
    w.u(s.flags.sample_adaptive_offset_enabled_flag, 1);
    w.u(s.flags.pcm_enabled_flag, 1);
    if (s.flags.pcm_enabled_flag) {
        w.u(s.pcm_sample_bit_depth_luma_minus1, 4);
        w.u(s.pcm_sample_bit_depth_chroma_minus1, 4);
        w.ue(s.log2_min_pcm_luma_coding_block_size_minus3);
        w.ue(s.log2_diff_max_min_pcm_luma_coding_block_size);
        w.u(s.flags.pcm_loop_filter_disabled_flag, 1);
    }
    uint32_t nsets = s.pShortTermRefPicSet ? s.num_short_term_ref_pic_sets : 0;
    w.ue(nsets);
    for (uint32_t i = 0; i < nsets; i++)
        write_std_rps(w, s.pShortTermRefPicSet[i], i, nsets);
    w.u(s.flags.long_term_ref_pics_present_flag, 1);
    if (s.flags.long_term_ref_pics_present_flag) {
        uint32_t n = s.pLongTermRefPicsSps ? s.num_long_term_ref_pics_sps : 0;
        w.ue(n);
        for (uint32_t i = 0; i < n; i++) {
            w.u(s.pLongTermRefPicsSps->lt_ref_pic_poc_lsb_sps[i], 4 + s.log2_max_pic_order_cnt_lsb_minus4);
            w.u((s.pLongTermRefPicsSps->used_by_curr_pic_lt_sps_flag >> i) & 1, 1);
        }
    }
    w.u(s.flags.sps_temporal_mvp_enabled_flag, 1);
    w.u(s.flags.strong_intra_smoothing_enabled_flag, 1);
    const StdVideoH265SequenceParameterSetVui *v = s.flags.vui_parameters_present_flag ? s.pSequenceParameterSetVui : nullptr;
    w.u(v != nullptr, 1);
    if (v) {
        w.u(v->flags.aspect_ratio_info_present_flag, 1);
        if (v->flags.aspect_ratio_info_present_flag) {
            w.u(v->aspect_ratio_idc, 8);
            if (v->aspect_ratio_idc == STD_VIDEO_H265_ASPECT_RATIO_IDC_EXTENDED_SAR) {
                w.u(v->sar_width, 16);
                w.u(v->sar_height, 16);
            }
        }
        w.u(v->flags.overscan_info_present_flag, 1);
        if (v->flags.overscan_info_present_flag)
            w.u(v->flags.overscan_appropriate_flag, 1);
        w.u(v->flags.video_signal_type_present_flag, 1);
        if (v->flags.video_signal_type_present_flag) {
            w.u(v->video_format, 3);
            w.u(v->flags.video_full_range_flag, 1);
            w.u(v->flags.colour_description_present_flag, 1);
            if (v->flags.colour_description_present_flag) {
                w.u(v->colour_primaries, 8);
                w.u(v->transfer_characteristics, 8);
                w.u(v->matrix_coeffs, 8);
            }
        }
        w.u(v->flags.chroma_loc_info_present_flag, 1);
        if (v->flags.chroma_loc_info_present_flag) {
            w.ue(v->chroma_sample_loc_type_top_field);
            w.ue(v->chroma_sample_loc_type_bottom_field);
        }
        w.u(v->flags.neutral_chroma_indication_flag, 1);
        w.u(v->flags.field_seq_flag, 1);
        w.u(v->flags.frame_field_info_present_flag, 1);
        w.u(v->flags.default_display_window_flag, 1);
        if (v->flags.default_display_window_flag) {
            w.ue(v->def_disp_win_left_offset);
            w.ue(v->def_disp_win_right_offset);
            w.ue(v->def_disp_win_top_offset);
            w.ue(v->def_disp_win_bottom_offset);
        }
        w.u(v->flags.vui_timing_info_present_flag, 1);
        if (v->flags.vui_timing_info_present_flag) {
            w.u(v->vui_num_units_in_tick, 32);
            w.u(v->vui_time_scale, 32);
            w.u(v->flags.vui_poc_proportional_to_timing_flag, 1);
            if (v->flags.vui_poc_proportional_to_timing_flag)
                w.ue(v->vui_num_ticks_poc_diff_one_minus1);
            w.u(0, 1);   // vui_hrd_parameters_present_flag (HRD is not carried over)
        }
        w.u(v->flags.bitstream_restriction_flag, 1);
        if (v->flags.bitstream_restriction_flag) {
            w.u(v->flags.tiles_fixed_structure_flag, 1);
            w.u(v->flags.motion_vectors_over_pic_boundaries_flag, 1);
            w.u(v->flags.restricted_ref_pic_lists_flag, 1);
            w.ue(v->min_spatial_segmentation_idc);
            w.ue(v->max_bytes_per_pic_denom);
            w.ue(v->max_bits_per_min_cu_denom);
            w.ue(v->log2_max_mv_length_horizontal);
            w.ue(v->log2_max_mv_length_vertical);
        }
    }
    w.u(0, 1);   // sps_extension_present_flag
    finish_nal(out, w);
}

void write_pps(std::vector<uint8_t> &out, const StdVideoH265PictureParameterSet &p)
{
    BitWriter w;
    w.u(34 << 1, 8);
    w.u(1, 8);
    w.ue(p.pps_pic_parameter_set_id);
    w.ue(p.pps_seq_parameter_set_id);
    w.u(p.flags.dependent_slice_segments_enabled_flag, 1);
    w.u(p.flags.output_flag_present_flag, 1);
    w.u(p.num_extra_slice_header_bits, 3);
    w.u(p.flags.sign_data_hiding_enabled_flag, 1);
    w.u(p.flags.cabac_init_present_flag, 1);
    w.ue(p.num_ref_idx_l0_default_active_minus1);
    w.ue(p.num_ref_idx_l1_default_active_minus1);
    w.se(p.init_qp_minus26);
    w.u(p.flags.constrained_intra_pred_flag, 1);
    w.u(p.flags.transform_skip_enabled_flag, 1);
    w.u(p.flags.cu_qp_delta_enabled_flag, 1);
    if (p.flags.cu_qp_delta_enabled_flag)
        w.ue(p.diff_cu_qp_delta_depth);
    w.se(p.pps_cb_qp_offset);
    w.se(p.pps_cr_qp_offset);
    w.u(p.flags.pps_slice_chroma_qp_offsets_present_flag, 1);
    w.u(p.flags.weighted_pred_flag, 1);
    w.u(p.flags.weighted_bipred_flag, 1);
    w.u(p.flags.transquant_bypass_enabled_flag, 1);
    w.u(p.flags.tiles_enabled_flag, 1);
    w.u(p.flags.entropy_coding_sync_enabled_flag, 1);
    if (p.flags.tiles_enabled_flag) {
        w.ue(p.num_tile_columns_minus1);
        w.ue(p.num_tile_rows_minus1);
        w.u(p.flags.uniform_spacing_flag, 1);
        if (!p.flags.uniform_spacing_flag) {
            for (uint32_t i = 0; i < p.num_tile_columns_minus1; i++)
                w.ue(p.column_width_minus1[i]);
            for (uint32_t i = 0; i < p.num_tile_rows_minus1; i++)
                w.ue(p.row_height_minus1[i]);
        }
        w.u(p.flags.loop_filter_across_tiles_enabled_flag, 1);
    }
    w.u(p.flags.pps_loop_filter_across_slices_enabled_flag, 1);
    w.u(p.flags.deblocking_filter_control_present_flag, 1);
    if (p.flags.deblocking_filter_control_present_flag) {
        w.u(p.flags.deblocking_filter_override_enabled_flag, 1);
        w.u(p.flags.pps_deblocking_filter_disabled_flag, 1);
        if (!p.flags.pps_deblocking_filter_disabled_flag) {
            w.se(p.pps_beta_offset_div2);
            w.se(p.pps_tc_offset_div2);
        }
    }
    w.u(0, 1);   // pps_scaling_list_data_present_flag (explicit lists are not carried over)
    w.u(p.flags.lists_modification_present_flag, 1);
    w.ue(p.log2_parallel_merge_level_minus2);
    w.u(p.flags.slice_segment_header_extension_present_flag, 1);
    w.u(0, 1);   // pps_extension_present_flag
    finish_nal(out, w);
}

bool override_sps(StdVideoH265SequenceParameterSet &s, const Sps &e)
{
    StdVideoH265SequenceParameterSet o = s;
    s.log2_min_luma_coding_block_size_minus3 = (uint8_t)(e.log2_min_cb - 3);
    s.log2_diff_max_min_luma_coding_block_size = (uint8_t)(e.log2_ctb - e.log2_min_cb);
    s.log2_min_luma_transform_block_size_minus2 = (uint8_t)(e.log2_min_tb - 2);
    s.log2_diff_max_min_luma_transform_block_size = (uint8_t)(e.log2_max_tb - e.log2_min_tb);
    s.max_transform_hierarchy_depth_inter = (uint8_t)e.depth_inter;
    s.max_transform_hierarchy_depth_intra = (uint8_t)e.depth_intra;
    s.flags.amp_enabled_flag = e.amp;
    s.flags.sample_adaptive_offset_enabled_flag = e.sao || s.flags.sample_adaptive_offset_enabled_flag;
    s.flags.sps_temporal_mvp_enabled_flag = e.tmvp || s.flags.sps_temporal_mvp_enabled_flag;
    s.flags.strong_intra_smoothing_enabled_flag = e.strong_intra;
    s.flags.pcm_enabled_flag = e.pcm;
    s.flags.scaling_list_enabled_flag = 0;
    return memcmp(&o.flags, &s.flags, sizeof(o.flags)) != 0 ||
           o.log2_min_luma_coding_block_size_minus3 != s.log2_min_luma_coding_block_size_minus3 ||
           o.log2_diff_max_min_luma_coding_block_size != s.log2_diff_max_min_luma_coding_block_size ||
           o.log2_min_luma_transform_block_size_minus2 != s.log2_min_luma_transform_block_size_minus2 ||
           o.log2_diff_max_min_luma_transform_block_size != s.log2_diff_max_min_luma_transform_block_size ||
           o.max_transform_hierarchy_depth_inter != s.max_transform_hierarchy_depth_inter ||
           o.max_transform_hierarchy_depth_intra != s.max_transform_hierarchy_depth_intra;
}

bool override_pps(StdVideoH265PictureParameterSet &p, const Pps &e)
{
    StdVideoH265PictureParameterSet o = p;
    p.flags.sign_data_hiding_enabled_flag = e.sign_data_hiding;
    p.flags.cabac_init_present_flag = e.cabac_init_present || p.flags.cabac_init_present_flag;
    p.flags.constrained_intra_pred_flag = e.constrained_intra;
    p.flags.transform_skip_enabled_flag = e.transform_skip;
    p.flags.cu_qp_delta_enabled_flag = e.cu_qp_delta;
    p.diff_cu_qp_delta_depth = (uint8_t)e.diff_cu_qp_delta_depth;
    p.pps_cb_qp_offset = (int8_t)e.cb_qp_offset;
    p.pps_cr_qp_offset = (int8_t)e.cr_qp_offset;
    p.flags.pps_slice_chroma_qp_offsets_present_flag = e.slice_chroma_qp_offsets || p.flags.pps_slice_chroma_qp_offsets_present_flag;
    p.flags.transquant_bypass_enabled_flag = e.transquant_bypass;
    p.flags.tiles_enabled_flag = e.tiles;
    p.flags.entropy_coding_sync_enabled_flag = e.entropy_sync;
    if (!e.tiles) {
        p.num_tile_columns_minus1 = 0;
        p.num_tile_rows_minus1 = 0;
    }
    p.log2_parallel_merge_level_minus2 = (uint8_t)(e.log2_parallel_merge - 2);
    // deblocking: the encoder's defaults, and override allowed so slice headers can carry its per-slice choices
    p.flags.deblocking_filter_control_present_flag = 1;
    p.flags.deblocking_filter_override_enabled_flag = 1;
    p.flags.pps_deblocking_filter_disabled_flag = e.deblocking_disabled;
    p.pps_beta_offset_div2 = (int8_t)e.beta_offset_div2;
    p.pps_tc_offset_div2 = (int8_t)e.tc_offset_div2;
    p.flags.pps_scaling_list_data_present_flag = 0;
    p.pScalingLists = nullptr;
    return memcmp(&o.flags, &p.flags, sizeof(o.flags)) != 0 || o.diff_cu_qp_delta_depth != p.diff_cu_qp_delta_depth ||
           o.pps_cb_qp_offset != p.pps_cb_qp_offset || o.pps_cr_qp_offset != p.pps_cr_qp_offset ||
           o.log2_parallel_merge_level_minus2 != p.log2_parallel_merge_level_minus2 ||
           o.pps_beta_offset_div2 != p.pps_beta_offset_div2 || o.pps_tc_offset_div2 != p.pps_tc_offset_div2;
}

std::string incompatibility(const Sps &es, const Pps &ep, const Sps &as, const Pps &ap)
{
    std::string r;
    auto chk = [&](bool same, const char *what) {
        if (!same)
            r += r.empty() ? what : std::string(", ") + what;
    };
    chk(es.width == as.width && es.height == as.height, "picture size");
    chk(es.chroma_format_idc == as.chroma_format_idc, "chroma format");
    chk(es.bit_depth_luma == as.bit_depth_luma && es.bit_depth_chroma == as.bit_depth_chroma, "bit depth");
    if (es.log2_ctb != as.log2_ctb || es.log2_min_cb != as.log2_min_cb)
        chk(false, ("CTB/CB sizes (encoder " + std::to_string(1 << es.log2_ctb) + "/" + std::to_string(1 << es.log2_min_cb) +
                    ", application " + std::to_string(1 << as.log2_ctb) + "/" + std::to_string(1 << as.log2_min_cb) + ")").c_str());
    chk(es.log2_min_tb == as.log2_min_tb && es.log2_max_tb == as.log2_max_tb, "transform sizes");
    if (es.depth_inter != as.depth_inter || es.depth_intra != as.depth_intra)
        chk(false, ("transform hierarchy depth (encoder " + std::to_string(es.depth_inter) + "/" + std::to_string(es.depth_intra) +
                    ", application " + std::to_string(as.depth_inter) + "/" + std::to_string(as.depth_intra) + ")").c_str());
    chk(es.amp == as.amp, "AMP");
    chk(!es.sao || as.sao, "SAO");
    chk(es.pcm == as.pcm, "PCM");
    chk(!es.tmvp || as.tmvp, "temporal MV prediction");
    chk(es.strong_intra == as.strong_intra, "strong intra smoothing");
    chk(!es.scaling_list && !as.scaling_list, "scaling lists");
    chk(ep.sign_data_hiding == ap.sign_data_hiding, "sign data hiding");
    chk(ep.transform_skip == ap.transform_skip, "transform skip");
    chk(ep.cu_qp_delta == ap.cu_qp_delta && (!ep.cu_qp_delta || ep.diff_cu_qp_delta_depth == ap.diff_cu_qp_delta_depth), "CU QP delta");
    chk(ep.cb_qp_offset == ap.cb_qp_offset && ep.cr_qp_offset == ap.cr_qp_offset, "chroma QP offsets");
    chk(ep.constrained_intra == ap.constrained_intra, "constrained intra prediction");
    chk(ep.transquant_bypass == ap.transquant_bypass, "transquant bypass");
    chk(ep.tiles == ap.tiles && ep.entropy_sync == ap.entropy_sync, "tiles/wavefronts");
    chk(ep.log2_parallel_merge == ap.log2_parallel_merge, "parallel merge level");
    chk(!ep.weighted_pred || ap.weighted_pred, "weighted prediction");
    return r;
}

// ------------------------------------------------------------------------------------------------- slices
bool rewrite_slice(const uint8_t *nal, size_t n, const Sps &es, const Pps &ep, const Sps &as, const Pps &ap, const PicInfo &pic,
                   PocMap &pocs, std::vector<uint8_t> &out, std::string *err)
{
    auto fail = [&](const char *m) {
        if (err)
            *err = m;
        return false;
    };
    if (n < 3)
        return fail("short NAL");
    uint32_t type = (nal[0] >> 1) & 0x3f;
    if (pic.have && pic.idr && type != 19 && type != 20)
        return fail("the application asked for an IDR picture, the backend made another kind");
    uint32_t tid = (nal[1] & 7) ? (nal[1] & 7) - 1u : 0;
    bool irap = type >= 16 && type <= 23;
    bool idr = type == 19 || type == 20;
    std::vector<uint8_t> rb = unescape(nal + 2, n - 2);
    BitReader r(rb.data(), rb.size());

    // --- the backend's slice segment header
    bool first = r.u(1);
    uint32_t no_output_prior = irap ? r.u(1) : 0;
    r.ue();   // pps id
    uint32_t ctbs = ((es.width + (1u << es.log2_ctb) - 1) >> es.log2_ctb) * ((es.height + (1u << es.log2_ctb) - 1) >> es.log2_ctb);
    uint32_t address = 0;
    if (!first) {
        if (ep.dependent_slices && r.u(1))
            return fail("dependent slice segments are not supported");
        address = r.u((int)ceil_log2(ctbs));
    }
    r.u((int)ep.num_extra_bits);
    uint32_t slice_type = r.ue();   // 0 B, 1 P, 2 I
    if (ep.output_flag)
        r.u(1);
    int32_t enc_poc = 0;
    Rps rps;
    bool tmvp_flag = false;
    if (!idr) {
        uint32_t lsb = r.u((int)es.log2_max_poc_lsb);
        uint32_t max_lsb = 1u << es.log2_max_poc_lsb;
        int32_t prev = pocs.enc_prev_tid0;
        int32_t prev_lsb = prev & (int32_t)(max_lsb - 1), prev_msb = prev - prev_lsb;
        int32_t msb = prev_msb;
        if ((int32_t)lsb < prev_lsb && prev_lsb - (int32_t)lsb >= (int32_t)max_lsb / 2)
            msb += (int32_t)max_lsb;
        else if ((int32_t)lsb > prev_lsb && (int32_t)lsb - prev_lsb > (int32_t)max_lsb / 2)
            msb -= (int32_t)max_lsb;
        enc_poc = msb + (int32_t)lsb;
        bool sps_set = r.u(1);
        if (!sps_set) {
            if (!parse_rps(r, (uint32_t)es.rps.size(), (uint32_t)es.rps.size(), es.rps, rps))
                return fail("cannot parse the backend's reference picture set");
        } else {
            uint32_t idx = es.rps.size() > 1 ? r.u((int)ceil_log2((uint32_t)es.rps.size())) : 0;
            if (idx >= es.rps.size())
                return fail("bad reference picture set index");
            rps = es.rps[idx];
        }
        if (es.long_term) {
            uint32_t lt_sps = es.num_long_term_sps ? r.ue() : 0;
            uint32_t lt = r.ue();
            if (lt_sps || lt)
                return fail("long-term references are not supported");
        }
        if (es.tmvp)
            tmvp_flag = r.u(1);
    } else {
        pocs.reset();
    }
    bool sao_luma = false, sao_chroma = false;
    if (es.sao) {
        sao_luma = r.u(1);
        if (es.chroma_format_idc != 0)
            sao_chroma = r.u(1);
    }
    uint32_t nl0 = 0, nl1 = 0;
    bool mod_l0 = false, mod_l1 = false, mvd_l1_zero = false, cabac_init = false, col_l0 = true;
    std::vector<uint32_t> list_l0, list_l1;
    uint32_t col_ref = 0, merge_cand = 0;
    uint32_t num_pic_total = 0;
    for (uint32_t i = 0; i < rps.num_neg; i++)
        num_pic_total += rps.used_neg[i];
    for (uint32_t i = 0; i < rps.num_pos; i++)
        num_pic_total += rps.used_pos[i];
    if (slice_type != 2) {
        nl0 = ep.num_ref_l0;
        nl1 = ep.num_ref_l1;
        if (r.u(1)) {
            nl0 = r.ue() + 1;
            if (slice_type == 0)
                nl1 = r.ue() + 1;
        }
        if (ep.lists_modification && num_pic_total > 1) {
            int bits = (int)ceil_log2(num_pic_total);
            mod_l0 = r.u(1);
            if (mod_l0)
                for (uint32_t i = 0; i < nl0; i++)
                    list_l0.push_back(r.u(bits));
            if (slice_type == 0) {
                mod_l1 = r.u(1);
                if (mod_l1)
                    for (uint32_t i = 0; i < nl1; i++)
                        list_l1.push_back(r.u(bits));
            }
        }
        if (slice_type == 0)
            mvd_l1_zero = r.u(1);
        if (ep.cabac_init_present)
            cabac_init = r.u(1);
        if (tmvp_flag) {
            if (slice_type == 0)
                col_l0 = r.u(1);
            if ((col_l0 && nl0 > 1) || (!col_l0 && nl1 > 1))
                col_ref = r.ue();
        }
        if ((ep.weighted_pred && slice_type == 1) || (ep.weighted_bipred && slice_type == 0))
            return fail("weighted prediction is not supported");
        merge_cand = r.ue();
    }
    int32_t qp = ep.init_qp + r.se();
    int32_t cb = 0, cr = 0;
    if (ep.slice_chroma_qp_offsets) {
        cb = r.se();
        cr = r.se();
    }
    bool dbk_disabled = ep.deblocking_disabled;
    int32_t beta = ep.beta_offset_div2, tc = ep.tc_offset_div2;
    if (ep.deblocking_override && r.u(1)) {
        dbk_disabled = r.u(1);
        if (!dbk_disabled) {
            beta = r.se();
            tc = r.se();
        }
    }
    bool lf_across = ep.loop_filter_across_slices;
    if (ep.loop_filter_across_slices && (sao_luma || sao_chroma || !dbk_disabled))
        lf_across = r.u(1);
    if (ep.tiles || ep.entropy_sync) {
        uint32_t entries = r.ue();
        if (entries)
            return fail("entry points (tiles/wavefronts) are not supported");
    }
    if (ep.header_extension) {
        uint32_t len = r.ue();
        for (uint32_t i = 0; i < len; i++)
            r.u(8);
    }
    // byte_alignment(): a one, then zeros
    if (r.u(1) != 1)
        return fail("bad byte alignment");
    while (!r.aligned())
        r.u(1);
    if (r.err)
        return fail("cannot parse the backend's slice segment header");
    if (tid == 0)
        pocs.enc_prev_tid0 = enc_poc;

    // --- written for the application's parameter sets
    int32_t app_poc = pic.have ? pic.poc : enc_poc;
    pocs.enc_to_app[enc_poc] = app_poc;
    while (pocs.enc_to_app.size() > 32)
        pocs.enc_to_app.erase(pocs.enc_to_app.begin());
    if (!as.sao && (sao_luma || sao_chroma))
        return fail("the backend used SAO, the application's SPS does not enable it");
    if (!as.tmvp && tmvp_flag)
        return fail("the backend used temporal MV prediction, the application's SPS does not enable it");
    if (!ap.cabac_init_present && cabac_init)
        return fail("the backend used cabac_init_flag, the application's PPS does not allow it");

    BitWriter w;
    w.u(nal[0], 8);
    w.u(nal[1], 8);
    w.u(first, 1);
    if (irap)
        w.u(no_output_prior, 1);
    w.ue(ap.id);
    if (!first) {
        if (ap.dependent_slices)
            w.u(0, 1);
        uint32_t actbs = ((as.width + (1u << as.log2_ctb) - 1) >> as.log2_ctb) * ((as.height + (1u << as.log2_ctb) - 1) >> as.log2_ctb);
        w.u(address, (int)ceil_log2(actbs));
    }
    w.u(0, (int)ap.num_extra_bits);
    w.ue(slice_type);
    if (ap.output_flag)
        w.u(1, 1);
    if (!idr) {
        w.u((uint32_t)app_poc & ((1u << as.log2_max_poc_lsb) - 1), (int)as.log2_max_poc_lsb);
        // the backend's references, in the application's POC numbering: an explicit set
        Rps arps = rps;
        for (uint32_t i = 0; i < rps.num_neg; i++) {
            auto it = pocs.enc_to_app.find(enc_poc + rps.delta_neg[i]);
            arps.delta_neg[i] = it != pocs.enc_to_app.end() ? it->second - app_poc : rps.delta_neg[i];
        }
        for (uint32_t i = 0; i < rps.num_pos; i++) {
            auto it = pocs.enc_to_app.find(enc_poc + rps.delta_pos[i]);
            arps.delta_pos[i] = it != pocs.enc_to_app.end() ? it->second - app_poc : rps.delta_pos[i];
        }
        for (uint32_t i = 1; i < arps.num_neg; i++)
            if (arps.delta_neg[i] >= arps.delta_neg[i - 1])
                return fail("reference order changed in the application's POC numbering");
        for (uint32_t i = 0; i < arps.num_neg; i++)
            if (arps.delta_neg[i] >= 0) {
                if (err)
                    *err = "a past reference is not in the past in the application's POC numbering (encoder POC " +
                           std::to_string(enc_poc) + " delta " + std::to_string(rps.delta_neg[i]) + ", application POC " +
                           std::to_string(app_poc) + " -> " + std::to_string(arps.delta_neg[i]) + ")";
                return false;
            }
        w.u(0, 1);   // short_term_ref_pic_set_sps_flag
        write_rps_explicit(w, arps, !as.rps.empty());
        if (as.long_term) {
            if (as.num_long_term_sps)
                w.ue(0);
            w.ue(0);
        }
        if (as.tmvp)
            w.u(tmvp_flag, 1);
    }
    if (as.sao) {
        w.u(sao_luma, 1);
        if (as.chroma_format_idc != 0)
            w.u(sao_chroma, 1);
    }
    if (slice_type != 2) {
        bool ovr = nl0 != ap.num_ref_l0 || (slice_type == 0 && nl1 != ap.num_ref_l1);
        w.u(ovr, 1);
        if (ovr) {
            w.ue(nl0 - 1);
            if (slice_type == 0)
                w.ue(nl1 - 1);
        }
        if (ap.lists_modification && num_pic_total > 1) {
            int bits = (int)ceil_log2(num_pic_total);
            w.u(mod_l0, 1);
            for (uint32_t v : list_l0)
                w.u(v, bits);
            if (slice_type == 0) {
                w.u(mod_l1, 1);
                for (uint32_t v : list_l1)
                    w.u(v, bits);
            }
        } else if (mod_l0 || mod_l1) {
            return fail("the backend modified reference lists, the application's PPS does not allow it");
        }
        if (slice_type == 0)
            w.u(mvd_l1_zero, 1);
        if (ap.cabac_init_present)
            w.u(cabac_init, 1);
        if (tmvp_flag) {
            if (slice_type == 0)
                w.u(col_l0, 1);
            if ((col_l0 && nl0 > 1) || (!col_l0 && nl1 > 1))
                w.ue(col_ref);
        }
        w.ue(merge_cand);
    }
    w.se(qp - ap.init_qp);
    if (ap.slice_chroma_qp_offsets) {
        w.se(cb);
        w.se(cr);
    } else if (cb || cr) {
        return fail("the backend used slice chroma QP offsets, the application's PPS does not allow them");
    }
    bool same_dbk = dbk_disabled == ap.deblocking_disabled && (dbk_disabled || (beta == ap.beta_offset_div2 && tc == ap.tc_offset_div2));
    if (ap.deblocking_override) {
        w.u(!same_dbk, 1);
        if (!same_dbk) {
            w.u(dbk_disabled, 1);
            if (!dbk_disabled) {
                w.se(beta);
                w.se(tc);
            }
        }
    } else if (!same_dbk) {
        return fail("the backend's deblocking differs from the application's PPS, which cannot override it");
    }
    if (ap.loop_filter_across_slices && (sao_luma || sao_chroma || !dbk_disabled))
        w.u(lf_across, 1);
    if (ap.tiles || ap.entropy_sync)
        w.ue(0);
    if (ap.header_extension)
        w.ue(0);
    w.u(1, 1);
    w.align_zero();
    w.b.insert(w.b.end(), rb.begin() + (long)(r.pos / 8), rb.end());
    start_code(out);
    out.push_back(w.b[0]);
    out.push_back(w.b[1]);
    escape_append(out, std::vector<uint8_t>(w.b.begin() + 2, w.b.end()));
    return true;
}

}  // namespace h265
}  // namespace venc
