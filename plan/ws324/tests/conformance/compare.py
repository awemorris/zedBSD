#!/usr/bin/python3 -I
# Compares what conform (the H.265 parse: h265-stream, h265-dpb, h265-std) made of one conformance bitstream with
# three references that do not use the parse:
#   1. the bitstream itself, scanned here for its NAL unit headers (picture boundaries, types, end of sequence),
#      with this script's own reading of 8.1.3 (NoRaslOutputFlag, the pictures a decoder skips), 8.3.1 (POC),
#      7.4.5 (scaling lists), 7.4.7.1 (long-term values), 7.4.7.3 (weights) and 7.4.8 (short-term sets);
#   2. ffmpeg's trace_headers (the syntax element values of every VPS, SPS, PPS and slice segment header);
#   3. ffmpeg's hevc decoder log (-threads 1 -loglevel debug): the POC of each decoded picture and the output order.
# ffmpeg is a measuring stick for data; where it and the specification differ the exceptions table says why.
#   compare.py --name N (--zip Z --member M | --bitstream B) --conform C --trace T.gz --decode D.gz --detail OUT
# Prints one line: "stream <name> <PASS|FAIL> class=<in|out> ..." (counts and the failed checks); the details of
# every mismatch go to --detail.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import gzip
import re
import subprocess
import sys

# The default scaling lists (Table 7-5 for sizeId 0, Table 7-6 for sizeId 1..3; intra matrixId 0..2, inter 3..5).
DEFAULT_4X4 = [16] * 16
DEFAULT_INTRA = [
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18,
    17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
    24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31,
    29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115]
DEFAULT_INTER = [
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
    18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
    28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91]

# The sections of trace_headers this script reads; every other title ends the section being read.
TRACE_TITLES = {
    'Video Parameter Set': 'VPS',
    'Sequence Parameter Set': 'SPS',
    'Picture Parameter Set': 'PPS',
    'Slice Segment Header': 'SH',
}

# The VPS's HRD fields: conform writes the first HRD only, so they compare as a prefix.
HRD_NAMES = {
    'nal_hrd_parameters_present_flag', 'vcl_hrd_parameters_present_flag', 'sub_pic_hrd_params_present_flag',
    'tick_divisor_minus2', 'du_cpb_removal_delay_increment_length_minus1',
    'sub_pic_cpb_params_in_pic_timing_sei_flag', 'dpb_output_delay_du_length_minus1', 'bit_rate_scale',
    'cpb_size_scale', 'cpb_size_du_scale', 'initial_cpb_removal_delay_length_minus1',
    'au_cpb_removal_delay_length_minus1', 'dpb_output_delay_length_minus1', 'fixed_pic_rate_general_flag',
    'fixed_pic_rate_within_cvs_flag', 'elemental_duration_in_tc_minus1', 'low_delay_hrd_flag', 'cpb_cnt_minus1',
    'bit_rate_value_minus1', 'cpb_size_value_minus1', 'cpb_size_du_value_minus1', 'bit_rate_du_value_minus1',
    'cbr_flag'}

# The trace's syntax elements conform does not write (the NAL unit header, the trailing and alignment bits, the
# reserved bits and sub-layer profiles the parse skips, the elements of segments other than the first, and the
# elements whose derived values conform writes instead: scaling lists, list sizes, weights).
NOT_WRITTEN = {
    'forbidden_zero_bit', 'nal_unit_type', 'nuh_layer_id', 'nuh_temporal_id_plus1', 'rbsp_stop_one_bit',
    'rbsp_alignment_zero_bit', 'alignment_bit_equal_to_one', 'alignment_bit_equal_to_zero',
    'general_reserved_zero_7bits', 'general_reserved_zero_35bits', 'general_reserved_zero_43bits',
    'general_reserved_zero_bit', 'general_one_picture_only_constraint_flag', 'general_inbld_flag',
    'general_max_12bit_constraint_flag', 'general_max_10bit_constraint_flag', 'general_max_8bit_constraint_flag',
    'general_max_422chroma_constraint_flag', 'general_max_420chroma_constraint_flag',
    'general_max_monochrome_constraint_flag', 'general_intra_constraint_flag',
    'general_lower_bit_rate_constraint_flag', 'general_max_14bit_constraint_flag', 'reserved_zero_2bits',
    'sub_layer_profile_present_flag', 'sub_layer_level_present_flag', 'sub_layer_profile_space',
    'sub_layer_tier_flag', 'sub_layer_profile_idc', 'sub_layer_profile_compatibility_flag',
    'sub_layer_progressive_source_flag', 'sub_layer_interlaced_source_flag', 'sub_layer_non_packed_constraint_flag',
    'sub_layer_frame_only_constraint_flag', 'sub_layer_reserved_zero_43bits', 'sub_layer_reserved_zero_7bits',
    'sub_layer_reserved_zero_35bits', 'sub_layer_one_picture_only_constraint_flag', 'sub_layer_inbld_flag',
    'sub_layer_reserved_zero_bit', 'sub_layer_level_idc', 'vps_reserved_0xffff_16bits', 'layer_id_included_flag',
    'hrd_layer_set_idx', 'cprms_present_flag', 'vps_extension_data_flag', 'extension_data', 'sps_extension_4bits',
    'pps_extension_4bits', 'sps_extension_data_flag', 'pps_extension_data_flag', 'scaling_list_pred_mode_flag',
    'scaling_list_pred_matrix_id_delta', 'scaling_list_dc_coef_minus8', 'scaling_list_delta_coeff',
    'num_ref_idx_active_override_flag', 'num_ref_idx_l0_active_minus1', 'num_ref_idx_l1_active_minus1',
    'luma_weight_l0_flag', 'luma_weight_l1_flag', 'chroma_weight_l0_flag', 'chroma_weight_l1_flag',
    'delta_luma_weight_l0', 'delta_luma_weight_l1', 'luma_offset_l0', 'luma_offset_l1', 'delta_chroma_weight_l0',
    'delta_chroma_weight_l1', 'chroma_offset_l0', 'chroma_offset_l1', 'entry_point_offset_minus1',
    'slice_segment_header_extension_length', 'slice_segment_header_extension_data_byte'}

# Exceptions: stream -> {check: reason}.  A listed check of a listed stream is reported as waived, not failed.
# Every entry is checked against the stream's txt and the specification (phase.md, the exceptions table).
EXCEPTIONS = {
    'TSUNEQBD_A_MAIN10_Technicolor_2': {
        'structure': 'ffmpeg 7.1 does not read an SPS whose luma and chroma bit depths differ (10 and 9), so it '
                     'gives no trace; the range values were checked by hand against the zip txt and the HM .trc',
        'ffmpeg-decode': 'ffmpeg decodes no picture of it (the same SPS)',
        'ffmpeg-output': 'ffmpeg outputs no picture of it (the same SPS)',
    },
}

# The prefix ffmpeg puts on each trace line (run.sh strips it; a line read after a carriage return still has it).
TRACE_PREFIX_RE = re.compile(r'^.*\[trace_headers @ 0x[0-9a-f]+\] ')
FIELD_RE = re.compile(r'^(\d+)\s+(\S+)\s+[01]*\s*= (-?\d+)$')
POC_RE = re.compile(r'(Decoded|Output) frame with POC (-?\d+)/(-?\d+)\.')


def base(name):
    """The name without its indices."""
    return name.split('[', 1)[0]


class Section:
    """One parameter set or slice segment header: (position, name, value) records in order."""

    def __init__(self, kind):
        self.kind = kind
        self.records = []
        self.values = {}

    def add(self, position, name, value):
        self.records.append((position, name, value))
        self.values.setdefault(name, value)

    def get(self, name, default=None):
        return self.values.get(name, default)

    def groups(self):
        out = {}
        for _, name, value in self.records:
            out.setdefault(name, []).append(value)
        return out


def open_text(path):
    if path.endswith('.gz'):
        return gzip.open(path, 'rt', errors='replace')
    return open(path, 'r', errors='replace')


def read_trace(path, unread):
    """The sections of trace_headers after the extradata (the packets), in stream order.  A line that looks like a
    field but does not read as one (another output joined to it) goes to unread: the trace is then not trusted.
    ffmpeg's own statistics line (frame=..., at the end even with -nostats) is skipped."""
    sections = []
    current = None
    packets = False
    with open_text(path) as handle:
        for line in handle:
            line = TRACE_PREFIX_RE.sub('', line.rstrip('\n'))
            if line.startswith('['):
                continue
            if line.startswith('Packet:'):
                packets = True
                current = None
                continue
            if not packets:
                continue
            match = FIELD_RE.match(line)
            if match:
                if current is not None:
                    current.add(int(match.group(1)), match.group(2), int(match.group(3)))
                continue
            if line.startswith('frame=') and '[' not in line:
                continue
            if re.match(r'^\d', line) or 'frame=' in line:
                unread.append(line)
                continue
            kind = TRACE_TITLES.get(line.strip())
            current = None
            if kind is not None:
                current = Section(kind)
                sections.append(current)
    return sections


def read_decode(path):
    """The POCs ffmpeg decoded (decoding order) and output (output order) after its "Stream mapping:" line (the
    stream information probe before it decodes with another decoder, which can sit at the same address).  A debug
    line can land at the end of another line without its "[hevc @ ...]" prefix, so the pattern is searched."""
    decoded = []
    output = []
    started = False
    with open_text(path) as handle:
        for line in handle:
            if line.startswith('Stream mapping:'):
                started = True
                decoded = []
                output = []
                continue
            match = POC_RE.search(line)
            if not match or match.group(2) != '0':
                continue
            if match.group(1) == 'Decoded':
                decoded.append(int(match.group(3)))
            else:
                output.append(int(match.group(3)))
    if not started:
        decoded.append('no Stream mapping line')
    return decoded, output


class Conform:
    """What conform wrote."""

    def __init__(self):
        self.pictures = []
        self.skips_before = []
        self.trailing = {}
        self.outputs = []
        self.counts = {}
        self.dpb = []
        self.std = []
        self.errors = []


def read_conform(path):
    conform = Conform()
    pending = {}
    current = None
    picture = None
    with open(path, 'r', errors='replace') as handle:
        for line in handle:
            line = line.rstrip('\n')
            if line.startswith('@'):
                parts = line.split()
                tag = parts[0]
                current = None
                if tag == '@PIC':
                    keys = ['index', 'nal', 'tid', 'poc', 'pic_output', 'no_rasl', 'first', 'after_eos',
                            'noprior', 'irap', 'idr', 'vps', 'sps', 'pps', 'generation', 'slices']
                    picture = dict(zip(keys, [int(x) for x in parts[1:]]))
                    picture['sets'] = []
                    picture['sh'] = None
                    conform.pictures.append(picture)
                    conform.skips_before.append(pending)
                    pending = {}
                elif tag == '@SKIP':
                    pending[parts[1]] = pending.get(parts[1], 0) + int(parts[2])
                elif tag in ('@VPS', '@SPS', '@PPS'):
                    current = Section(tag[1:])
                    current.id = int(parts[1])
                    picture['sets'].append(current)
                elif tag == '@SH':
                    current = Section('SH')
                    picture['sh'] = current
                elif tag == '@OUT':
                    conform.outputs.append(int(parts[1]))
                elif tag == '@COUNT':
                    conform.counts[parts[1]] = parts[2] if len(parts) > 2 else ''
                elif tag == '@DPB':
                    conform.dpb.append(line)
                elif tag == '@STD':
                    conform.std.append(line)
                elif tag == '@ERROR':
                    conform.errors.append(line)
                continue
            if current is None:
                continue
            name, _, value = line.partition(' ')
            if name.startswith('='):
                current.values.setdefault(name, value)
                current.records.append((None, name, value))
            else:
                current.add(None, name, int(value))
    conform.trailing = pending
    return conform


def scan_nal_units(data):
    """Each NAL unit's (offset, type, layer, temporal id, first_slice_segment_in_pic_flag or None)."""
    units = []
    position = 0
    length = len(data)
    starts = []
    while True:
        found = data.find(b'\x00\x00\x01', position)
        if found < 0 or found + 3 >= length:
            break
        starts.append(found + 3)
        position = found + 3
    for start in starts:
        if start + 1 >= length:
            continue
        first_byte = data[start]
        second = data[start + 1]
        kind = (first_byte >> 1) & 0x3F
        layer = ((first_byte & 1) << 5) | (second >> 3)
        tid = (second & 7) - 1
        flag = None
        if kind < 32 and start + 2 < length:
            flag = data[start + 2] >> 7
        units.append((start, kind, layer, tid, flag))
    return units


def is_reserved_vcl(kind):
    return 10 <= kind <= 15 or 22 <= kind <= 31


def expected_pictures(units):
    """The pictures of the bitstream in decoding order, with what 8.1.3 says a decoder does with each."""
    pictures = []
    started = False
    first_pending = True
    eos_pending = False
    irap_no_rasl = False
    current = None
    between = []
    for _, kind, layer, tid, flag in units:
        if kind in (36, 37) and layer == 0:
            eos_pending = True
            continue
        if kind >= 32 and layer == 0:
            between.append(kind)
            continue
        if layer != 0:
            continue
        if flag == 0:
            if current is not None:
                current['slices'] += 1
                for other in between:
                    current['interleaved'][other] = current['interleaved'].get(other, 0) + 1
            between = []
            continue
        between = []
        current = {'nal': kind, 'tid': tid, 'slices': 1, 'skip': None, 'interleaved': {}}
        pictures.append(current)
        if is_reserved_vcl(kind):
            current['skip'] = 'reserved_type'
            continue
        irap = 16 <= kind <= 21
        if not started and not irap:
            current['skip'] = 'before_irap'
            continue
        if irap:
            irap_no_rasl = kind <= 20 or first_pending or eos_pending
        if kind in (8, 9) and irap_no_rasl:
            current['skip'] = 'rasl_skipped'
            continue
        current['irap'] = int(irap)
        current['idr'] = int(kind in (19, 20))
        current['no_rasl'] = int(irap and irap_no_rasl)
        current['first'] = int(first_pending)
        current['after_eos'] = int(eos_pending)
        started = True
        first_pending = False
        eos_pending = False
    return pictures


class Checker:
    """Counts checks and collects the details of mismatches."""

    def __init__(self, name):
        self.name = name
        self.counts = {}
        self.failed = {}
        self.details = []
        self.values = 0

    def check(self, kind, ok, text):
        self.counts[kind] = self.counts.get(kind, 0) + 1
        if not ok:
            self.failed[kind] = self.failed.get(kind, 0) + 1
            if len(self.details) < 400:
                self.details.append('%s: %s' % (kind, text))


def compare_records(checker, kind, where, ours, theirs, prefix=frozenset()):
    """Each syntax element conform wrote against the same element's occurrences in the trace."""
    their_groups = theirs.groups()
    for name, values in ours.groups().items():
        if name.startswith('='):
            continue
        other = their_groups.get(name, [])
        if base(name) in prefix:
            other = other[:len(values)]
        checker.values += len(values)
        checker.check(kind, values == other, '%s %s: conform %s, trace %s' % (where, name, values[:8], other[:8]))
    our_groups = ours.groups()
    for name in their_groups:
        if name not in our_groups and base(name) not in NOT_WRITTEN:
            checker.check(kind, False, '%s %s: in the trace, conform wrote none' % (where, name))


def derive_scaling(section, enabled_default):
    """ScalingList and the DC values (7.3.4, 7.4.5) from a section's scaling_list_data() syntax."""
    lists = {}
    dcs = {}
    for size_id in range(4):
        for matrix_id in range(0, 6, 3 if size_id == 3 else 1):
            count = 16 if size_id == 0 else 64
            mode = section.get('scaling_list_pred_mode_flag[%d][%d]' % (size_id, matrix_id))
            if enabled_default:
                mode = 0
                delta = 0
            elif mode == 0:
                delta = section.get('scaling_list_pred_matrix_id_delta[%d][%d]' % (size_id, matrix_id), 0)
            if mode == 0:
                if delta == 0:
                    if size_id == 0:
                        lists[(size_id, matrix_id)] = list(DEFAULT_4X4)
                    else:
                        lists[(size_id, matrix_id)] = list(DEFAULT_INTRA if matrix_id < 3 else DEFAULT_INTER)
                    dcs[(size_id, matrix_id)] = 16
                else:
                    ref = matrix_id - delta * (3 if size_id == 3 else 1)
                    lists[(size_id, matrix_id)] = list(lists.get((size_id, ref), []))
                    dcs[(size_id, matrix_id)] = dcs.get((size_id, ref), 16)
                continue
            value = 8
            if size_id > 1:
                value = section.get('scaling_list_dc_coef_minus8[%d][%d]' % (size_id - 2, matrix_id), 0) + 8
                dcs[(size_id, matrix_id)] = value
            entries = []
            for i in range(count):
                delta_coef = section.get('scaling_list_delta_coeff[%d][%d][%d]' % (size_id, matrix_id, i), 0)
                value = (value + delta_coef + 256) % 256
                entries.append(value)
            lists[(size_id, matrix_id)] = entries
    out = {}
    for (size_id, matrix_id), entries in lists.items():
        out['=ScalingList[%d][%d]' % (size_id, matrix_id)] = ' '.join(str(x) for x in entries)
        if size_id >= 2:
            out['=ScalingDC[%d][%d]' % (size_id, matrix_id)] = str(dcs[(size_id, matrix_id)])
    return out


class Cursor:
    """Reads a section's records in order (for the st_ref_pic_set() structure)."""

    def __init__(self, records, index):
        self.records = records
        self.index = index

    def take(self, name):
        if self.index >= len(self.records) or self.records[self.index][1] != name:
            got = self.records[self.index][1] if self.index < len(self.records) else 'end'
            raise ValueError('expected %s, found %s' % (name, got))
        value = self.records[self.index][2]
        self.index += 1
        return value

    def peek(self):
        if self.index >= len(self.records):
            return None
        return self.records[self.index][1]


def derive_st_rps(cursor, index, num_sets, sets):
    """One st_ref_pic_set( index ) (7.3.7) and its derivation (7.4.8): ([(delta, used)] S0, S1)."""
    inter = 0
    if index != 0:
        inter = cursor.take('inter_ref_pic_set_prediction_flag')
    if inter:
        delta_idx_minus1 = 0
        if index == num_sets:
            delta_idx_minus1 = cursor.take('delta_idx_minus1')
        sign = cursor.take('delta_rps_sign')
        absolute = cursor.take('abs_delta_rps_minus1') + 1
        delta_rps = (1 - 2 * sign) * absolute
        ref_s0, ref_s1 = sets[index - (delta_idx_minus1 + 1)]
        num_negative = len(ref_s0)
        num_delta = len(ref_s0) + len(ref_s1)
        used = []
        use_delta = []
        for j in range(num_delta + 1):
            flag = cursor.take('used_by_curr_pic_flag[%d]' % j)
            used.append(flag)
            use_delta.append(1 if flag else cursor.take('use_delta_flag[%d]' % j))
        s0 = []
        s1 = []
        for j in range(len(ref_s1) - 1, -1, -1):
            d = ref_s1[j][0] + delta_rps
            if d < 0 and use_delta[num_negative + j]:
                s0.append((d, used[num_negative + j]))
        if delta_rps < 0 and use_delta[num_delta]:
            s0.append((delta_rps, used[num_delta]))
        for j in range(num_negative):
            d = ref_s0[j][0] + delta_rps
            if d < 0 and use_delta[j]:
                s0.append((d, used[j]))
        for j in range(num_negative - 1, -1, -1):
            d = ref_s0[j][0] + delta_rps
            if d > 0 and use_delta[j]:
                s1.append((d, used[j]))
        if delta_rps > 0 and use_delta[num_delta]:
            s1.append((delta_rps, used[num_delta]))
        for j in range(len(ref_s1)):
            d = ref_s1[j][0] + delta_rps
            if d > 0 and use_delta[num_negative + j]:
                s1.append((d, used[num_negative + j]))
        return s0, s1
    negative = cursor.take('num_negative_pics')
    positive = cursor.take('num_positive_pics')
    s0 = []
    s1 = []
    poc = 0
    for i in range(negative):
        poc -= cursor.take('delta_poc_s0_minus1[%d]' % i) + 1
        s0.append((poc, cursor.take('used_by_curr_pic_s0_flag[%d]' % i)))
    poc = 0
    for i in range(positive):
        poc += cursor.take('delta_poc_s1_minus1[%d]' % i) + 1
        s1.append((poc, cursor.take('used_by_curr_pic_s1_flag[%d]' % i)))
    return s0, s1


def st_text(rps):
    s0, s1 = rps
    parts = ['%d' % len(s0), '%d' % len(s1)] + ['%d:%d' % (d, u) for d, u in s0 + s1]
    return ' '.join(parts)


def sps_sets(section):
    """The SPS's derived short-term sets, from its trace records."""
    names = [name for _, name, _ in section.records]
    sets = []
    if 'num_short_term_ref_pic_sets' not in names:
        return sets
    start = names.index('num_short_term_ref_pic_sets')
    count = section.records[start][2]
    cursor = Cursor(section.records, start + 1)
    for i in range(count):
        sets.append(derive_st_rps(cursor, i, count, sets))
    return sets


def derive_ps(section, kind):
    """The derived values of a parameter set from its trace records."""
    out = {}
    if kind == 'SPS':
        if section.get('scaling_list_enabled_flag'):
            out.update(derive_scaling(section, not section.get('sps_scaling_list_data_present_flag')))
        for i, rps in enumerate(sps_sets(section)):
            out['=StRps[%d]' % i] = st_text(rps)
    if kind == 'PPS' and section.get('pps_scaling_list_data_present_flag'):
        out.update(derive_scaling(section, False))
    return out


def derive_slice(section, sps, pps):
    """The derived values of a slice segment header from its trace records and its SPS and PPS."""
    out = {}
    kind = section.get('nal_unit_type')
    names = [name for _, name, _ in section.records]
    sets = sps_sets(sps)
    num_sets = sps.get('num_short_term_ref_pic_sets', 0)
    rps = ([], [])
    if kind not in (19, 20):
        if section.get('short_term_ref_pic_set_sps_flag'):
            rps = sets[section.get('short_term_ref_pic_set_idx', 0)]
        else:
            start = names.index('short_term_ref_pic_set_sps_flag') + 1
            cursor = Cursor(section.records, start)
            rps = derive_st_rps(cursor, num_sets, num_sets, sets)
            following = section.records[cursor.index][0] if cursor.index < len(section.records) else None
            out['=StRpsBits'] = str(following - section.records[start][0]) if following is not None else '?'
        out['=StRpsSlice[0]'] = st_text(rps)
    total = sum(u for _, u in rps[0] + rps[1])

    # The long-term pictures (7.4.7.1).
    count = section.get('num_long_term_sps', 0) + section.get('num_long_term_pics', 0)
    from_sps = section.get('num_long_term_sps', 0)
    cycle = 0
    for i in range(count):
        if i < from_sps:
            idx = section.get('lt_idx_sps[%d]' % i, 0)
            lsb = sps.get('lt_ref_pic_poc_lsb_sps[%d]' % idx)
            used = sps.get('used_by_curr_pic_lt_sps_flag[%d]' % idx)
        else:
            lsb = section.get('poc_lsb_lt[%d]' % i)
            used = section.get('used_by_curr_pic_lt_flag[%d]' % i)
        delta = section.get('delta_poc_msb_cycle_lt[%d]' % i, 0)
        cycle = delta if i in (0, from_sps) else delta + cycle
        out['=PocLsbLt[%d]' % i] = str(lsb)
        out['=UsedByCurrPicLt[%d]' % i] = str(used)
        out['=DeltaPocMsbCycleLt[%d]' % i] = str(cycle)
        total += used

    # The list sizes and the weights (7.4.7.1, 7.4.7.3).
    slice_type = section.get('slice_type')
    lists = []
    if slice_type in (0, 1):
        lists = [0] if slice_type == 1 else [0, 1]
        out['=NumPicTotalCurr'] = str(total)
    sizes = {}
    for x in lists:
        if section.get('num_ref_idx_active_override_flag'):
            sizes[x] = section.get('num_ref_idx_l%d_active_minus1' % x) + 1
        else:
            sizes[x] = pps.get('num_ref_idx_l%d_default_active_minus1' % x) + 1
        out['=NumRefIdxActive[%d]' % x] = str(sizes[x])
    if 'luma_log2_weight_denom' in section.values:
        denom = section.get('luma_log2_weight_denom')
        chroma_denom = denom + section.get('delta_chroma_log2_weight_denom', 0)
        chroma = sps.get('chroma_format_idc') != 0 and not sps.get('separate_colour_plane_flag', 0)
        for x in lists:
            for i in range(sizes[x]):
                if section.get('luma_weight_l%d_flag[%d]' % (x, i)):
                    weight = (1 << denom) + section.get('delta_luma_weight_l%d[%d]' % (x, i))
                    offset = section.get('luma_offset_l%d[%d]' % (x, i))
                else:
                    weight = 1 << denom
                    offset = 0
                out['=LumaWeightL%d[%d]' % (x, i)] = str(weight)
                out['=LumaOffsetL%d[%d]' % (x, i)] = str(offset)
                if not chroma:
                    continue
                for j in range(2):
                    if section.get('chroma_weight_l%d_flag[%d]' % (x, i)):
                        weight = (1 << chroma_denom) + section.get('delta_chroma_weight_l%d[%d][%d]' % (x, i, j))
                        raw = section.get('chroma_offset_l%d[%d][%d]' % (x, i, j))
                        offset = max(-128, min(127, 128 + raw - ((128 * weight) >> chroma_denom)))
                    else:
                        weight = 1 << chroma_denom
                        offset = 0
                    out['=ChromaWeightL%d[%d][%d]' % (x, i, j)] = str(weight)
                    out['=ChromaOffsetL%d[%d][%d]' % (x, i, j)] = str(offset)

    # Where slice_segment_data() starts: the byte after byte_alignment(), in the RBSP after the 2-byte header.
    last = section.records[-1][0]
    out['=DataOffset'] = str((last + 1) // 8 - 2)
    return out


def compare_derived(checker, kind, where, ours, expected):
    for name, value in expected.items():
        mine = ours.values.get(name)
        checker.values += 1
        checker.check(kind, mine == value, '%s %s: conform %s, derived %s' % (where, name, mine, value))
    for name in ours.values:
        if name.startswith('=') and name not in expected:
            checker.check(kind, False, '%s %s: conform wrote it, the trace has no such value' % (where, name))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--name', required=True)
    parser.add_argument('--zip')
    parser.add_argument('--member')
    parser.add_argument('--bitstream')
    parser.add_argument('--conform', required=True)
    parser.add_argument('--trace', required=True)
    parser.add_argument('--decode', required=True)
    parser.add_argument('--detail', required=True)
    args = parser.parse_args()

    checker = Checker(args.name)
    waived = EXCEPTIONS.get(args.name, {})
    if args.bitstream is not None:
        with open(args.bitstream, 'rb') as handle:
            data = handle.read()
    else:
        data = subprocess.run(['unzip', '-p', args.zip, args.member], stdout=subprocess.PIPE, timeout=300,
                              check=True).stdout
    units = scan_nal_units(data)
    expected = expected_pictures(units)
    conform = read_conform(args.conform)
    unread = []
    sections = read_trace(args.trace, unread)
    checker.check('structure', not unread, 'trace lines that do not read: %d, first %r' % (
        len(unread), unread[:1]))
    decoded, output = read_decode(args.decode)

    # The trace's pictures: each first slice segment header of the base layer, with the parameter sets then current.
    trace_pictures = []
    current_sets = {'VPS': {}, 'SPS': {}, 'PPS': {}}
    id_names = {'VPS': 'vps_video_parameter_set_id', 'SPS': 'sps_seq_parameter_set_id',
                'PPS': 'pps_pic_parameter_set_id'}
    for section in sections:
        if section.get('nuh_layer_id', 0) != 0:
            continue
        if section.kind in id_names:
            current_sets[section.kind][section.get(id_names[section.kind])] = section
        elif section.get('first_slice_segment_in_pic_flag') == 1:
            trace_pictures.append({'sh': section, 'sets': {k: dict(v) for k, v in current_sets.items()}})

    # The bitstream's pictures that the trace has (every VCL type but the reserved ones) get their trace.
    parsed = [p for p in expected if not is_reserved_vcl(p['nal'])]
    checker.check('structure', len(parsed) == len(trace_pictures),
                  'pictures: bitstream %d (not reserved), trace %d' % (len(parsed), len(trace_pictures)))
    for picture, traced in zip(parsed, trace_pictures):
        picture['trace'] = traced
        checker.check('structure', traced['sh'].get('nal_unit_type') == picture['nal'],
                      'nal type: bitstream %d, trace %s' % (picture['nal'], traced['sh'].get('nal_unit_type')))

    # The pictures returned and the ones skipped between them (8.1.3), against conform.
    returned = [p for p in expected if p['skip'] is None]
    gaps = []
    gap = {}
    for picture in expected:
        if picture['skip'] is None:
            gaps.append(gap)
            gap = {}
        else:
            gap[picture['skip']] = gap.get(picture['skip'], 0) + 1
    picture_skips = ('before_irap', 'rasl_skipped', 'reserved_type', 'broken_pictures')
    conform_gaps = [{k: v for k, v in g.items() if k in picture_skips} for g in conform.skips_before]
    trailing = {k: v for k, v in conform.trailing.items() if k in picture_skips}
    checker.check('pictures', len(returned) == len(conform.pictures),
                  'returned: expected %d, conform %d' % (len(returned), len(conform.pictures)))
    for i, (mine, theirs) in enumerate(zip(conform_gaps, gaps)):
        checker.check('pictures', mine == theirs, 'skipped before picture %d: expected %s, conform %s' % (i, theirs, mine))
    checker.check('pictures', trailing == gap, 'skipped at the end: expected %s, conform %s' % (gap, trailing))

    # Each returned picture: its flags, POC (8.3.1), first slice segment header and parameter sets.
    prev_tid0 = 0
    class_out = False
    for mine, theirs in zip(conform.pictures, returned):
        where = 'picture %d' % mine['index']
        for key in ('nal', 'tid', 'irap', 'idr', 'no_rasl', 'first', 'after_eos', 'slices'):
            checker.check('pictures', mine[key] == theirs[key],
                          '%s %s: expected %s, conform %s' % (where, key, theirs[key], mine[key]))
        traced = theirs.get('trace')
        if traced is None:
            continue
        sh = traced['sh']
        pps = traced['sets']['PPS'].get(sh.get('slice_pic_parameter_set_id'))
        sps = traced['sets']['SPS'].get(pps.get('pps_seq_parameter_set_id')) if pps is not None else None
        if pps is None or sps is None:
            checker.check('structure', False, '%s: the trace has no PPS or SPS for it' % where)
            continue
        if sps.get('chroma_format_idc') != 1 or sps.get('bit_depth_luma_minus8') != 0 or \
                sps.get('bit_depth_chroma_minus8') != 0:
            class_out = True

        # NoOutputOfPriorPicsFlag (C.5.2.2), PicOutputFlag (8.1.3) and the POC (8.3.1).
        noprior = 0
        if mine['irap'] and mine['no_rasl'] and not mine['first']:
            noprior = 1 if theirs['nal'] == 21 else sh.get('no_output_of_prior_pics_flag', 0)
        checker.check('pictures', mine['noprior'] == noprior, '%s noprior: expected %d, conform %d' %
                      (where, noprior, mine['noprior']))
        checker.check('pictures', mine['pic_output'] == sh.get('pic_output_flag', 1),
                      '%s pic_output: conform %d' % (where, mine['pic_output']))
        lsb_bits = sps.get('log2_max_pic_order_cnt_lsb_minus4') + 4
        lsb = sh.get('slice_pic_order_cnt_lsb', 0)
        if mine['irap'] and mine['no_rasl']:
            msb = 0
        else:
            prev_lsb = prev_tid0 & ((1 << lsb_bits) - 1)
            prev_msb = prev_tid0 - prev_lsb
            if lsb < prev_lsb and prev_lsb - lsb >= (1 << (lsb_bits - 1)):
                msb = prev_msb + (1 << lsb_bits)
            elif lsb > prev_lsb and lsb - prev_lsb > (1 << (lsb_bits - 1)):
                msb = prev_msb - (1 << lsb_bits)
            else:
                msb = prev_msb
        poc = msb + lsb
        checker.check('poc', mine['poc'] == poc, '%s poc: 8.3.1 %d, conform %d' % (where, poc, mine['poc']))
        if theirs['tid'] == 0 and theirs['nal'] not in (6, 7, 8, 9) and not (theirs['nal'] <= 14 and theirs['nal'] % 2 == 0):
            prev_tid0 = poc

        # The parameter sets conform dumped at this picture, against the trace's ones then current.
        for ours in mine['sets']:
            other = traced['sets'][ours.kind].get(ours.id)
            if other is None:
                checker.check('fields', False, '%s %s %d: not in the trace' % (where, ours.kind, ours.id))
                continue
            label = '%s %s %d' % (where, ours.kind, ours.id)
            compare_records(checker, 'fields', label, ours, other, HRD_NAMES if ours.kind == 'VPS' else frozenset())
            try:
                derived = derive_ps(other, ours.kind)
            except (ValueError, IndexError, KeyError) as error:
                checker.check('derived', False, '%s: cannot derive: %s' % (label, error))
                continue
            compare_derived(checker, 'derived', label, ours, derived)

        # The first slice segment header.
        if mine['sh'] is None:
            checker.check('fields', False, '%s: conform wrote no header' % where)
            continue
        compare_records(checker, 'fields', where + ' SH', mine['sh'], sh)
        try:
            derived = derive_slice(sh, sps, pps)
        except (ValueError, IndexError, KeyError, TypeError) as error:
            checker.check('derived', False, '%s SH: cannot derive: %s' % (where, error))
            continue
        compare_derived(checker, 'derived', where + ' SH', mine['sh'], derived)

    # ffmpeg's decoded POCs (decoding order) and output order.
    pocs = [p['poc'] for p in conform.pictures]
    checker.check('ffmpeg-decode', pocs == decoded, 'decoded POCs: ffmpeg %d %s, conform %d %s' %
                  (len(decoded), decoded[:40], len(pocs), pocs[:40]))
    checker.check('ffmpeg-output', conform.outputs == output, 'output POCs: ffmpeg %d %s, conform %d %s' %
                  (len(output), output[:60], len(conform.outputs), conform.outputs[:60]))
    if conform.outputs != output:
        for i, (a, b) in enumerate(zip(conform.outputs, output)):
            if a != b:
                checker.details.append('ffmpeg-output: first difference at output %d: conform %d, ffmpeg %d' % (i, a, b))
                break

    # conform's own counts: nothing broken, refused, missing or failed.
    for key in ('broken_pictures', 'refused_sets', 'missing', 'evicted', 'dpb_errors', 'std_failures'):
        checker.check('counts', conform.counts.get(key) == '0', 'count %s = %s' % (key, conform.counts.get(key)))
    checker.check('counts', not conform.errors, 'errors %s' % conform.errors[:4])
    for line in conform.std:
        if 'profile-mapped' not in line:
            checker.details.append('std: ' + line)
    for line in conform.dpb:
        checker.details.append('dpb: ' + line)

    # The NAL units found between two slice segments of one picture (7.4.2.4.4: they do not end it).
    interleaved = {}
    for picture in expected:
        for kind, count in picture['interleaved'].items():
            interleaved[kind] = interleaved.get(kind, 0) + count
    pictures_interleaved = sum(1 for p in expected if p['interleaved'] and p['skip'] is None)

    # The class: out of the Main range when an SPS conform read is not 8-bit 4:2:0.
    for picture in conform.pictures:
        for ours in picture['sets']:
            if ours.kind == 'SPS' and (ours.get('chroma_format_idc') != 1 or ours.get('bit_depth_luma_minus8') != 0
                                       or ours.get('bit_depth_chroma_minus8') != 0):
                class_out = True

    # The verdict: every check passed, or failed only where the exceptions table waives it.
    failed = [k for k in checker.failed if k not in waived]
    status = 'FAIL' if failed else 'PASS'
    summary = 'stream %s %s class=%s pictures=%d outputs=%d' % (
        args.name, status, 'out' if class_out else 'in', len(conform.pictures), len(conform.outputs))
    if interleaved:
        summary += ' interleaved=%d:%s' % (pictures_interleaved, ','.join(
            'nut%d:%d' % (k, v) for k, v in sorted(interleaved.items())))
    summary += ' values=%d checks=' % checker.values + ','.join('%s:%d' % (k, v) for k, v in sorted(checker.counts.items()))
    if checker.failed:
        summary += ' failed=' + ','.join('%s:%d' % (k, v) for k, v in sorted(checker.failed.items()))
        summary += ' first=[%s]' % checker.details[0][:300] if checker.details else ''
    waived_hit = [k for k in checker.failed if k in waived]
    if waived_hit:
        summary += ' waived=' + ','.join(sorted(waived_hit))
    with open(args.detail, 'w') as handle:
        handle.write(summary + '\n')
        for kind in waived_hit:
            handle.write('waived %s: %s\n' % (kind, waived[kind]))
        for line in checker.details:
            handle.write(line + '\n')
    print(summary)
    return 0


if __name__ == '__main__':
    sys.exit(main())
