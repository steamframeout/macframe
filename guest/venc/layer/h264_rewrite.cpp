// H.264 parameter sets and slice-header rewriting (see h264_rewrite.h).
#include "h264_rewrite.h"

#include <string.h>

namespace venc {

// ------------------------------------------------------------------------------------------------- bits
uint32_t BitReader::u(int bits)
{
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        if (pos >= n * 8) {
            err = true;
            return 0;
        }
        v = (v << 1) | ((d[pos >> 3] >> (7 - (pos & 7))) & 1);
        pos++;
    }
    return v;
}
uint32_t BitReader::ue()
{
    int zeros = 0;
    while (u(1) == 0) {
        if (err || ++zeros > 31) {
            err = true;
            return 0;
        }
    }
    if (zeros == 0)
        return 0;
    return ((1u << zeros) - 1) + u(zeros);
}
int32_t BitReader::se()
{
    uint32_t k = ue();
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

void BitWriter::u(uint32_t v, int bits)
{
    for (int i = bits - 1; i >= 0; i--) {
        acc = (acc << 1) | ((v >> i) & 1);
        if (++nacc == 8) {
            b.push_back((uint8_t)acc);
            acc = 0;
            nacc = 0;
        }
    }
}
void BitWriter::ue(uint32_t v)
{
    uint64_t x = (uint64_t)v + 1;
    int len = 0;
    while ((x >> len) > 1)
        len++;
    u(0, len);
    u((uint32_t)x, len + 1);
}
void BitWriter::se(int32_t v) { ue(v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * (int64_t)v)); }
void BitWriter::align_zero()
{
    while (nacc)
        u(0, 1);
}
void BitWriter::align_one()
{
    while (nacc)
        u(1, 1);
}
void BitWriter::trailing()
{
    u(1, 1);
    align_zero();
}
void BitWriter::bits_from(BitReader &r, size_t count)
{
    while (count >= 16) {
        u(r.u(16), 16);
        count -= 16;
    }
    if (count)
        u(r.u((int)count), (int)count);
}

std::vector<uint8_t> unescape(const uint8_t *nal, size_t n)
{
    std::vector<uint8_t> r;
    r.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        r.push_back(nal[i]);
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    return r;
}
void escape_append(std::vector<uint8_t> &out, const std::vector<uint8_t> &rbsp)
{
    int zeros = 0;
    for (uint8_t c : rbsp) {
        if (zeros >= 2 && c <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }
}

static void start_code(std::vector<uint8_t> &out)
{
    static const uint8_t sc[4] = {0, 0, 0, 1};
    out.insert(out.end(), sc, sc + 4);
}

// ------------------------------------------------------------------------------------------------- parsing
static bool skip_scaling_list(BitReader &r, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; j++) {
        if (next != 0) {
            int delta = r.se();
            next = (last + delta + 256) % 256;
        }
        last = next == 0 ? last : next;
    }
    return !r.err;
}

bool parse_sps(const uint8_t *nal, size_t n, Sps &s)
{
    if (n < 4 || (nal[0] & 0x1f) != 7)
        return false;
    std::vector<uint8_t> rb = unescape(nal + 1, n - 1);
    BitReader r(rb.data(), rb.size());
    s = Sps{};
    s.profile_idc = r.u(8);
    r.u(8);   // constraint flags + reserved
    r.u(8);   // level
    s.id = r.ue();
    if (s.profile_idc == 100 || s.profile_idc == 110 || s.profile_idc == 122 || s.profile_idc == 244 || s.profile_idc == 44 ||
        s.profile_idc == 83 || s.profile_idc == 86 || s.profile_idc == 118 || s.profile_idc == 128 || s.profile_idc == 138 ||
        s.profile_idc == 139 || s.profile_idc == 134 || s.profile_idc == 135) {
        s.chroma_format_idc = r.ue();
        if (s.chroma_format_idc == 3)
            s.separate_colour_plane = r.u(1);
        s.bit_depth_luma = 8 + r.ue();
        s.bit_depth_chroma = 8 + r.ue();
        s.qpprime_bypass = r.u(1);
        s.scaling_matrix = r.u(1);
        if (s.scaling_matrix)
            for (int i = 0; i < (s.chroma_format_idc != 3 ? 8 : 12); i++)
                if (r.u(1))
                    skip_scaling_list(r, i < 6 ? 16 : 64);
    }
    s.log2_max_frame_num = 4 + r.ue();
    s.poc_type = r.ue();
    if (s.poc_type == 0) {
        s.log2_max_poc_lsb = 4 + r.ue();
    } else if (s.poc_type == 1) {
        s.delta_pic_order_always_zero = r.u(1);
        r.se();
        r.se();
        uint32_t k = r.ue();
        for (uint32_t i = 0; i < k && !r.err; i++)
            r.se();
    }
    s.max_num_ref_frames = r.ue();
    r.u(1);  // gaps
    s.width_mbs = r.ue() + 1;
    s.height_map_units = r.ue() + 1;
    s.frame_mbs_only = r.u(1);
    if (!s.frame_mbs_only)
        s.mb_adaptive = r.u(1);
    s.direct_8x8 = r.u(1);
    return !r.err;
}

bool parse_pps(const uint8_t *nal, size_t n, const Sps &sps, Pps &p)
{
    if (n < 2 || (nal[0] & 0x1f) != 8)
        return false;
    std::vector<uint8_t> rb = unescape(nal + 1, n - 1);
    BitReader r(rb.data(), rb.size());
    p = Pps{};
    p.id = r.ue();
    p.sps_id = r.ue();
    p.cabac = r.u(1);
    p.bottom_field_pic_order = r.u(1);
    p.num_slice_groups = r.ue() + 1;
    if (p.num_slice_groups > 1)
        return false;   // FMO: never produced by the backends
    p.num_ref_l0 = r.ue() + 1;
    p.num_ref_l1 = r.ue() + 1;
    p.weighted_pred = r.u(1);
    p.weighted_bipred_idc = r.u(2);
    p.pic_init_qp = 26 + r.se();
    r.se();  // pic_init_qs
    p.chroma_qp_offset = r.se();
    p.deblocking_control = r.u(1);
    p.constrained_intra = r.u(1);
    p.redundant_pic_cnt = r.u(1);
    p.second_chroma_qp_offset = p.chroma_qp_offset;
    // more_rbsp_data(): anything before the rbsp stop bit
    size_t last = rb.size() * 8;
    while (last > 0 && !((rb[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1))
        last--;
    if (last > 0 && r.pos < last - 1) {
        p.transform_8x8 = r.u(1);
        p.scaling_matrix = r.u(1);
        if (p.scaling_matrix)
            for (int i = 0; i < 6 + (sps.chroma_format_idc != 3 ? 2 : 6) * (int)p.transform_8x8; i++)
                if (r.u(1))
                    skip_scaling_list(r, i < 6 ? 16 : 64);
        p.second_chroma_qp_offset = r.se();
    }
    return !r.err;
}

static uint32_t level_number(StdVideoH264LevelIdc l)
{
    static const uint32_t t[] = {10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62};
    return (uint32_t)l < sizeof(t) / sizeof(t[0]) ? t[l] : 41;
}

Sps sps_from_std(const StdVideoH264SequenceParameterSet &s)
{
    Sps o;
    o.id = s.seq_parameter_set_id;
    o.profile_idc = s.profile_idc;
    o.chroma_format_idc = s.chroma_format_idc;
    o.bit_depth_luma = 8 + s.bit_depth_luma_minus8;
    o.bit_depth_chroma = 8 + s.bit_depth_chroma_minus8;
    o.separate_colour_plane = s.flags.separate_colour_plane_flag;
    o.scaling_matrix = s.flags.seq_scaling_matrix_present_flag;
    o.qpprime_bypass = s.flags.qpprime_y_zero_transform_bypass_flag;
    o.log2_max_frame_num = 4 + s.log2_max_frame_num_minus4;
    o.poc_type = s.pic_order_cnt_type;
    o.log2_max_poc_lsb = 4 + s.log2_max_pic_order_cnt_lsb_minus4;
    o.delta_pic_order_always_zero = s.flags.delta_pic_order_always_zero_flag;
    o.max_num_ref_frames = s.max_num_ref_frames;
    o.width_mbs = s.pic_width_in_mbs_minus1 + 1;
    o.height_map_units = s.pic_height_in_map_units_minus1 + 1;
    o.frame_mbs_only = s.flags.frame_mbs_only_flag;
    o.mb_adaptive = s.flags.mb_adaptive_frame_field_flag;
    o.direct_8x8 = s.flags.direct_8x8_inference_flag;
    return o;
}
Pps pps_from_std(const StdVideoH264PictureParameterSet &p)
{
    Pps o;
    o.id = p.pic_parameter_set_id;
    o.sps_id = p.seq_parameter_set_id;
    o.cabac = p.flags.entropy_coding_mode_flag;
    o.bottom_field_pic_order = p.flags.bottom_field_pic_order_in_frame_present_flag;
    o.num_ref_l0 = p.num_ref_idx_l0_default_active_minus1 + 1;
    o.num_ref_l1 = p.num_ref_idx_l1_default_active_minus1 + 1;
    o.weighted_pred = p.flags.weighted_pred_flag;
    o.weighted_bipred_idc = p.weighted_bipred_idc;
    o.pic_init_qp = 26 + p.pic_init_qp_minus26;
    o.chroma_qp_offset = p.chroma_qp_index_offset;
    o.second_chroma_qp_offset = p.second_chroma_qp_index_offset;
    o.deblocking_control = p.flags.deblocking_filter_control_present_flag;
    o.constrained_intra = p.flags.constrained_intra_pred_flag;
    o.redundant_pic_cnt = p.flags.redundant_pic_cnt_present_flag;
    o.transform_8x8 = p.flags.transform_8x8_mode_flag;
    o.scaling_matrix = p.flags.pic_scaling_matrix_present_flag;
    return o;
}

// ------------------------------------------------------------------------------------------------- writing
static void write_scaling_list(BitWriter &w, const uint8_t *list, int size, bool use_default)
{
    int last = 8;
    if (use_default) {
        w.se(-8);   // nextScale 0 at j = 0: use the default matrix
        return;
    }
    for (int j = 0; j < size; j++) {
        int delta = (int)list[j] - last;
        if (delta > 127)
            delta -= 256;
        if (delta < -128)
            delta += 256;
        w.se(delta);
        last = list[j];
    }
}
static void write_scaling_lists(BitWriter &w, const StdVideoH264ScalingLists *sl, int count)
{
    for (int i = 0; i < count; i++) {
        bool present = sl && (sl->scaling_list_present_mask >> i) & 1;
        w.u(present, 1);
        if (!present)
            continue;
        bool def = (sl->use_default_scaling_matrix_mask >> i) & 1;
        if (i < 6)
            write_scaling_list(w, sl->ScalingList4x4[i], 16, def);
        else
            write_scaling_list(w, sl->ScalingList8x8[i - 6], 64, def);
    }
}

static void write_hrd(BitWriter &w, const StdVideoH264HrdParameters &h)
{
    w.ue(h.cpb_cnt_minus1);
    w.u(h.bit_rate_scale, 4);
    w.u(h.cpb_size_scale, 4);
    for (uint32_t i = 0; i <= h.cpb_cnt_minus1 && i < STD_VIDEO_H264_CPB_CNT_LIST_SIZE; i++) {
        w.ue(h.bit_rate_value_minus1[i]);
        w.ue(h.cpb_size_value_minus1[i]);
        w.u(h.cbr_flag[i], 1);
    }
    w.u(h.initial_cpb_removal_delay_length_minus1, 5);
    w.u(h.cpb_removal_delay_length_minus1, 5);
    w.u(h.dpb_output_delay_length_minus1, 5);
    w.u(h.time_offset_length, 5);
}

void write_sps(std::vector<uint8_t> &out, const StdVideoH264SequenceParameterSet &s)
{
    BitWriter w;
    w.u(0x67, 8);  // nal_ref_idc 3, type 7
    w.u(s.profile_idc, 8);
    w.u(s.flags.constraint_set0_flag, 1);
    w.u(s.flags.constraint_set1_flag, 1);
    w.u(s.flags.constraint_set2_flag, 1);
    w.u(s.flags.constraint_set3_flag, 1);
    w.u(s.flags.constraint_set4_flag, 1);
    w.u(s.flags.constraint_set5_flag, 1);
    w.u(0, 2);
    w.u(level_number(s.level_idc), 8);
    w.ue(s.seq_parameter_set_id);
    uint32_t p = s.profile_idc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 || p == 138 ||
        p == 139 || p == 134 || p == 135) {
        w.ue(s.chroma_format_idc);
        if (s.chroma_format_idc == 3)
            w.u(s.flags.separate_colour_plane_flag, 1);
        w.ue(s.bit_depth_luma_minus8);
        w.ue(s.bit_depth_chroma_minus8);
        w.u(s.flags.qpprime_y_zero_transform_bypass_flag, 1);
        w.u(s.flags.seq_scaling_matrix_present_flag, 1);
        if (s.flags.seq_scaling_matrix_present_flag)
            write_scaling_lists(w, s.pScalingLists, s.chroma_format_idc != 3 ? 8 : 12);
    }
    w.ue(s.log2_max_frame_num_minus4);
    w.ue(s.pic_order_cnt_type);
    if (s.pic_order_cnt_type == 0) {
        w.ue(s.log2_max_pic_order_cnt_lsb_minus4);
    } else if (s.pic_order_cnt_type == 1) {
        w.u(s.flags.delta_pic_order_always_zero_flag, 1);
        w.se(s.offset_for_non_ref_pic);
        w.se(s.offset_for_top_to_bottom_field);
        w.ue(s.num_ref_frames_in_pic_order_cnt_cycle);
        for (uint32_t i = 0; i < s.num_ref_frames_in_pic_order_cnt_cycle; i++)
            w.se(s.pOffsetForRefFrame ? s.pOffsetForRefFrame[i] : 0);
    }
    w.ue(s.max_num_ref_frames);
    w.u(s.flags.gaps_in_frame_num_value_allowed_flag, 1);
    w.ue(s.pic_width_in_mbs_minus1);
    w.ue(s.pic_height_in_map_units_minus1);
    w.u(s.flags.frame_mbs_only_flag, 1);
    if (!s.flags.frame_mbs_only_flag)
        w.u(s.flags.mb_adaptive_frame_field_flag, 1);
    w.u(s.flags.direct_8x8_inference_flag, 1);
    w.u(s.flags.frame_cropping_flag, 1);
    if (s.flags.frame_cropping_flag) {
        w.ue(s.frame_crop_left_offset);
        w.ue(s.frame_crop_right_offset);
        w.ue(s.frame_crop_top_offset);
        w.ue(s.frame_crop_bottom_offset);
    }
    const StdVideoH264SequenceParameterSetVui *v = s.flags.vui_parameters_present_flag ? s.pSequenceParameterSetVui : nullptr;
    w.u(v != nullptr, 1);
    if (v) {
        w.u(v->flags.aspect_ratio_info_present_flag, 1);
        if (v->flags.aspect_ratio_info_present_flag) {
            w.u(v->aspect_ratio_idc, 8);
            if (v->aspect_ratio_idc == STD_VIDEO_H264_ASPECT_RATIO_IDC_EXTENDED_SAR) {
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
            w.u(v->flags.color_description_present_flag, 1);
            if (v->flags.color_description_present_flag) {
                w.u(v->colour_primaries, 8);
                w.u(v->transfer_characteristics, 8);
                w.u(v->matrix_coefficients, 8);
            }
        }
        w.u(v->flags.chroma_loc_info_present_flag, 1);
        if (v->flags.chroma_loc_info_present_flag) {
            w.ue(v->chroma_sample_loc_type_top_field);
            w.ue(v->chroma_sample_loc_type_bottom_field);
        }
        w.u(v->flags.timing_info_present_flag, 1);
        if (v->flags.timing_info_present_flag) {
            w.u(v->num_units_in_tick, 32);
            w.u(v->time_scale, 32);
            w.u(v->flags.fixed_frame_rate_flag, 1);
        }
        bool nal_hrd = v->flags.nal_hrd_parameters_present_flag && v->pHrdParameters;
        bool vcl_hrd = v->flags.vcl_hrd_parameters_present_flag && v->pHrdParameters;
        w.u(nal_hrd, 1);
        if (nal_hrd)
            write_hrd(w, *v->pHrdParameters);
        w.u(vcl_hrd, 1);
        if (vcl_hrd)
            write_hrd(w, *v->pHrdParameters);
        if (nal_hrd || vcl_hrd)
            w.u(0, 1);  // low_delay_hrd_flag
        w.u(0, 1);      // pic_struct_present_flag
        w.u(v->flags.bitstream_restriction_flag, 1);
        if (v->flags.bitstream_restriction_flag) {
            w.u(1, 1);   // motion_vectors_over_pic_boundaries_flag
            w.ue(2);     // max_bytes_per_pic_denom
            w.ue(1);     // max_bits_per_mb_denom
            w.ue(16);    // log2_max_mv_length_horizontal
            w.ue(16);    // log2_max_mv_length_vertical
            w.ue(v->max_num_reorder_frames);
            w.ue(v->max_dec_frame_buffering);
        }
    }
    w.trailing();
    start_code(out);
    out.push_back(w.b[0]);
    escape_append(out, std::vector<uint8_t>(w.b.begin() + 1, w.b.end()));
}

void write_pps(std::vector<uint8_t> &out, const StdVideoH264PictureParameterSet &p, const StdVideoH264SequenceParameterSet &sps)
{
    BitWriter w;
    w.u(0x68, 8);
    w.ue(p.pic_parameter_set_id);
    w.ue(p.seq_parameter_set_id);
    w.u(p.flags.entropy_coding_mode_flag, 1);
    w.u(p.flags.bottom_field_pic_order_in_frame_present_flag, 1);
    w.ue(0);  // num_slice_groups_minus1
    w.ue(p.num_ref_idx_l0_default_active_minus1);
    w.ue(p.num_ref_idx_l1_default_active_minus1);
    w.u(p.flags.weighted_pred_flag, 1);
    w.u(p.weighted_bipred_idc, 2);
    w.se(p.pic_init_qp_minus26);
    w.se(p.pic_init_qs_minus26);
    w.se(p.chroma_qp_index_offset);
    w.u(p.flags.deblocking_filter_control_present_flag, 1);
    w.u(p.flags.constrained_intra_pred_flag, 1);
    w.u(p.flags.redundant_pic_cnt_present_flag, 1);
    if (p.flags.transform_8x8_mode_flag || p.flags.pic_scaling_matrix_present_flag ||
        p.second_chroma_qp_index_offset != p.chroma_qp_index_offset) {
        w.u(p.flags.transform_8x8_mode_flag, 1);
        w.u(p.flags.pic_scaling_matrix_present_flag, 1);
        if (p.flags.pic_scaling_matrix_present_flag)
            write_scaling_lists(w, p.pScalingLists, 6 + (sps.chroma_format_idc != 3 ? 2 : 6) * p.flags.transform_8x8_mode_flag);
        w.se(p.second_chroma_qp_index_offset);
    }
    w.trailing();
    start_code(out);
    out.push_back(w.b[0]);
    escape_append(out, std::vector<uint8_t>(w.b.begin() + 1, w.b.end()));
}

std::string incompatibility(const Sps &es, const Pps &ep, const Sps &as, const Pps &ap)
{
    std::string r;
    auto chk = [&](bool same, const char *what) {
        if (!same)
            r += r.empty() ? what : std::string(", ") + what;
    };
    chk(es.width_mbs == as.width_mbs && es.height_map_units == as.height_map_units, "picture size");
    chk(es.chroma_format_idc == as.chroma_format_idc, "chroma format");
    chk(es.bit_depth_luma == as.bit_depth_luma && es.bit_depth_chroma == as.bit_depth_chroma, "bit depth");
    chk(es.frame_mbs_only == as.frame_mbs_only && es.mb_adaptive == as.mb_adaptive, "frame/field coding");
    chk(es.qpprime_bypass == as.qpprime_bypass, "lossless bypass");
    chk(!es.scaling_matrix && !as.scaling_matrix && !ep.scaling_matrix && !ap.scaling_matrix, "scaling matrices");
    chk(ep.cabac == ap.cabac, "entropy coding (CABAC/CAVLC)");
    chk(ep.transform_8x8 == ap.transform_8x8, "8x8 transform");
    chk(ep.chroma_qp_offset == ap.chroma_qp_offset && ep.second_chroma_qp_offset == ap.second_chroma_qp_offset, "chroma QP offsets");
    chk(ep.constrained_intra == ap.constrained_intra, "constrained intra prediction");
    chk(!ep.weighted_pred || ap.weighted_pred, "weighted prediction");
    return r;
}

// ------------------------------------------------------------------------------------------------- slices
bool rewrite_slice(const uint8_t *nal, size_t n, const Sps &es, const Pps &ep, const Sps &as, const Pps &ap, const PicInfo &pic,
                   std::vector<uint8_t> &out, std::string *err)
{
    auto fail = [&](const char *m) {
        if (err)
            *err = m;
        return false;
    };
    if (n < 2)
        return fail("short NAL");
    uint8_t hdr = nal[0];
    uint32_t type = hdr & 0x1f, ref_idc = (hdr >> 5) & 3;
    bool idr = type == 5;
    std::vector<uint8_t> rb = unescape(nal + 1, n - 1);
    BitReader r(rb.data(), rb.size());

    // --- parse the backend's slice header
    uint32_t first_mb = r.ue();
    uint32_t slice_type = r.ue();
    uint32_t st = slice_type % 5;   // 0 P, 1 B, 2 I, 3 SP, 4 SI
    if (st == 3 || st == 4)
        return fail("SP/SI slices are not supported");
    r.ue();  // pps id
    if (es.separate_colour_plane)
        r.u(2);
    uint32_t frame_num = r.u((int)es.log2_max_frame_num);
    if (!es.frame_mbs_only && r.u(1))
        return fail("field pictures are not supported");
    uint32_t idr_pic_id = idr ? r.ue() : 0;
    uint32_t poc_lsb = 0;
    if (es.poc_type == 0) {
        poc_lsb = r.u((int)es.log2_max_poc_lsb);
        if (ep.bottom_field_pic_order)
            r.se();
    } else if (es.poc_type == 1 && !es.delta_pic_order_always_zero) {
        r.se();
        if (ep.bottom_field_pic_order)
            r.se();
    }
    if (ep.redundant_pic_cnt)
        r.ue();
    uint32_t direct_spatial = 0;
    if (st == 1)
        direct_spatial = r.u(1);
    uint32_t nl0 = ep.num_ref_l0, nl1 = ep.num_ref_l1;
    if (st == 0 || st == 1) {
        if (r.u(1)) {
            nl0 = r.ue() + 1;
            if (st == 1)
                nl1 = r.ue() + 1;
        }
    }
    struct Mod { uint32_t idc, val; };
    std::vector<Mod> mods[2];
    bool mod_flag[2] = {false, false};
    for (int l = 0; l < (st == 1 ? 2 : (st == 0 ? 1 : 0)); l++) {
        mod_flag[l] = r.u(1);
        if (mod_flag[l]) {
            for (;;) {
                uint32_t idc = r.ue();
                if (idc == 3 || r.err)
                    break;
                mods[l].push_back({idc, r.ue()});
            }
        }
    }
    size_t pwt_start = r.pos, pwt_end = r.pos;
    bool enc_pwt = (ep.weighted_pred && st == 0) || (ep.weighted_bipred_idc == 1 && st == 1);
    if (enc_pwt) {
        r.ue();
        if (es.chroma_format_idc != 0)
            r.ue();
        for (int l = 0; l < (st == 1 ? 2 : 1); l++)
            for (uint32_t i = 0; i < (l ? nl1 : nl0); i++) {
                if (r.u(1)) {
                    r.se();
                    r.se();
                }
                if (es.chroma_format_idc != 0 && r.u(1))
                    for (int j = 0; j < 4; j++)
                        r.se();
            }
        pwt_end = r.pos;
    }
    size_t drpm_start = r.pos, drpm_end = r.pos;
    if (ref_idc) {
        if (idr) {
            r.u(2);
        } else if (r.u(1)) {
            for (;;) {
                uint32_t op = r.ue();
                if (op == 0 || r.err)
                    break;
                if (op == 1 || op == 3)
                    r.ue();
                if (op == 2)
                    r.ue();
                if (op == 3 || op == 6)
                    r.ue();
                if (op == 4)
                    r.ue();
            }
        }
        drpm_end = r.pos;
    }
    uint32_t cabac_init = 0;
    if (ep.cabac && st != 2)
        cabac_init = r.ue();
    int32_t qp = ep.pic_init_qp + r.se();
    uint32_t dbk_idc = 0;
    int32_t alpha = 0, beta = 0;
    if (ep.deblocking_control) {
        dbk_idc = r.ue();
        if (dbk_idc != 1) {
            alpha = r.se();
            beta = r.se();
        }
    }
    if (r.err)
        return fail("cannot parse the backend's slice header");

    // --- write it for the application's parameter sets
    BitWriter w;
    w.u(hdr, 8);
    w.ue(first_mb);
    w.ue(slice_type);
    w.ue(ap.id);
    if (as.separate_colour_plane)
        w.u(0, 2);
    uint32_t fn = pic.have ? pic.frame_num : frame_num;
    w.u(fn & ((1u << as.log2_max_frame_num) - 1), (int)as.log2_max_frame_num);
    if (!as.frame_mbs_only)
        w.u(0, 1);
    if (idr)
        w.ue(pic.have ? pic.idr_pic_id : idr_pic_id);
    if (as.poc_type == 0) {
        uint32_t poc = pic.have ? (uint32_t)pic.poc : (es.poc_type == 0 ? poc_lsb : 2 * frame_num);
        w.u(poc & ((1u << as.log2_max_poc_lsb) - 1), (int)as.log2_max_poc_lsb);
        if (ap.bottom_field_pic_order)
            w.se(0);
    } else if (as.poc_type == 1 && !as.delta_pic_order_always_zero) {
        w.se(0);
        if (ap.bottom_field_pic_order)
            w.se(0);
    }
    if (ap.redundant_pic_cnt)
        w.ue(0);
    if (st == 1)
        w.u(direct_spatial, 1);
    if (st == 0 || st == 1) {
        bool ovr = nl0 != ap.num_ref_l0 || (st == 1 && nl1 != ap.num_ref_l1);
        w.u(ovr, 1);
        if (ovr) {
            w.ue(nl0 - 1);
            if (st == 1)
                w.ue(nl1 - 1);
        }
    }
    for (int l = 0; l < (st == 1 ? 2 : (st == 0 ? 1 : 0)); l++) {
        w.u(mod_flag[l], 1);
        if (mod_flag[l]) {
            for (auto &m : mods[l]) {
                w.ue(m.idc);
                w.ue(m.val);
            }
            w.ue(3);
        }
    }
    bool app_pwt = (ap.weighted_pred && st == 0) || (ap.weighted_bipred_idc == 1 && st == 1);
    if (app_pwt) {
        if (enc_pwt) {
            BitReader c = r;
            c.pos = pwt_start;
            w.bits_from(c, pwt_end - pwt_start);
        } else {   // explicit weights, all default: decodes as unweighted
            w.ue(0);
            if (as.chroma_format_idc != 0)
                w.ue(0);
            for (int l = 0; l < (st == 1 ? 2 : 1); l++)
                for (uint32_t i = 0; i < (l ? nl1 : nl0); i++) {
                    w.u(0, 1);
                    if (as.chroma_format_idc != 0)
                        w.u(0, 1);
                }
        }
    } else if (enc_pwt) {
        return fail("the backend used weighted prediction, the application's PPS does not allow it");
    }
    if (ref_idc) {
        BitReader c = r;
        c.pos = drpm_start;
        w.bits_from(c, drpm_end - drpm_start);
    }
    if (ap.cabac && st != 2)
        w.ue(cabac_init);
    w.se(qp - ap.pic_init_qp);
    if (ap.deblocking_control) {
        w.ue(dbk_idc);
        if (dbk_idc != 1) {
            w.se(alpha);
            w.se(beta);
        }
    } else if (dbk_idc != 0 || alpha != 0 || beta != 0) {
        return fail("the backend changed deblocking, the application's PPS cannot express it");
    }

    // --- slice data
    if (ep.cabac != ap.cabac)
        return fail("entropy coding differs");
    if (ap.cabac) {
        while (!r.aligned())
            r.u(1);  // cabac_alignment_one_bit
        w.align_one();
        w.b.insert(w.b.end(), rb.begin() + (long)(r.pos / 8), rb.end());
    } else {
        // copy up to the rbsp stop bit, then terminate again
        size_t last = rb.size() * 8;
        while (last > r.pos && !((rb[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1))
            last--;
        if (last <= r.pos)
            return fail("no rbsp stop bit");
        w.bits_from(r, last - 1 - r.pos);
        w.trailing();
    }
    start_code(out);
    out.push_back(w.b[0]);
    escape_append(out, std::vector<uint8_t>(w.b.begin() + 1, w.b.end()));
    return true;
}

}  // namespace venc
