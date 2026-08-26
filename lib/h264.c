#include "codecmod.h"
#include <libavutil/opt.h>
#include <arpa/inet.h>
#include "loglib.h"


static void h264_decode_frame(decoder_t *dec, frame_q *out) {
	dec->dec_ts = dec->rtp_ts;

	if (dec->packet_buffer->len) {
		ilog(LOG_DEBUG, "Decoding %zu bytes of H.264", dec->packet_buffer->len);

		avc_decoder_input(dec, &STR_GS(dec->packet_buffer), out, false);
	}

	g_string_truncate(dec->packet_buffer, 0);
}


static void nal_hdr(GString *buffer) {
	// libx264 seems to use 4-byte code at the start of the frame and 3-byte ones in the middle
	if (buffer->len == 0)
		g_string_append_len(buffer, "\0\0\0\1", 4);
	else
		g_string_append_len(buffer, "\0\0\1", 3);
}


static bool append_nal(GString *buffer, str *unit) {
	nal_hdr(buffer);

	g_string_append_len(buffer, unit->s, unit->len);

	str_shift(unit, unit->len);

	return true;
}


static bool stap_a(GString *buffer, str *unit) {
	str_shift(unit, 1);

	while (unit->len) {
		uint16_t *lenp = (uint16_t *) unit->s;
		if (str_shift(unit, 2))
			return false;

		uint16_t len = ntohs(*lenp);
		if (len == 0 || len > unit->len)
			return false;

		str nal = STR_LEN(unit->s, len);
		append_nal(buffer, &nal);

		str_shift(unit, nal.len);
	}

	return true;
}


static bool fu_a(GString *buffer, str *unit, uint8_t nal) {
	str_shift(unit, 1);
	char *hdr = unit->s;
	if (str_shift(unit, 1))
		return false;

	if ((*hdr & 0x80)) { // start bit
		nal_hdr(buffer);
		uint8_t frag_nal = nal & 0xe0;
		frag_nal |= *hdr & 0x1f;
		g_string_append_c(buffer, frag_nal);
	}

	g_string_append_len(buffer, unit->s, unit->len);

	str_shift(unit, unit->len);

	return true;
}


static int h264_decoder_input(decoder_t *dec, const str *data, frame_q *out, bool mark) {
	if (!data)
		goto err;

	if (dec->rtp_ts != dec->dec_ts)
		h264_decode_frame(dec, out);

	str frame = *data;

	while (frame.len) {
		uint8_t nal = frame.s[0];
		uint8_t nal_type = nal & 0x1f;

		if (nal_type == 0 || nal_type >= 30)
			goto err;

		bool ok;

		if (nal_type <= 23)
			ok = append_nal(dec->packet_buffer, &frame);
		else if (nal_type == 24)
			ok = stap_a(dec->packet_buffer, &frame);
		else if (nal_type == 28)
			ok = fu_a(dec->packet_buffer, &frame, nal);
		else
			goto err; // unsupported

		if (!ok)
			goto err;
	}

	if (mark)
		h264_decode_frame(dec, out);

	return 0;

err:
	ilog(LOG_ERR | LOG_FLAG_LIMIT, "Invalid H.264 packet");
	return -1;
}


static const char *h264_profile(uint8_t profile_idc, uint8_t profile_iop) {
	if (profile_idc == 'B')
		return "baseline";
	if (profile_idc == 'M')
		return "main";
	if (profile_idc == 'X')
		return "extended";
	if (profile_idc == 0x64)
		return "high";
	if (profile_idc == 0x6e)
		return "high10";
	if (profile_idc == 0x7a)
		return "high422";
	if (profile_idc == 0xf4)
		return "high444";
	// XXX intra profiles?
	// XXX levels?

	ilog(LOG_WARN | LOG_FLAG_LIMIT, "Unrecognised H.264 profile 0x%02x", profile_idc);
	return "baseline";
}


static bool set_enc_options(encoder_t *enc, const str *codec_opts) {
	// defaults
	enc->avc.avcctx->gop_size = 10;

	av_opt_set(enc->avc.avcctx, "tune", "zerolatency", AV_OPT_SEARCH_CHILDREN);

	const char *profile = h264_profile(enc->format_options.h264.profile_idc,
			enc->format_options.h264.profile_iop);
	av_opt_set(enc->avc.avcctx, "profile", profile, AV_OPT_SEARCH_CHILDREN);

	av_opt_set(enc->avc.avcctx, "preset", "ultrafast", AV_OPT_SEARCH_CHILDREN);

	if (enc->format_options.h264.packetization_mode == 0)
		av_opt_set_int(enc->avc.avcctx, "slice-max-size", rtpe_common_config_ptr->video_pkt_size,
				AV_OPT_SEARCH_CHILDREN);
	else if (enc->format_options.h264.packetization_mode == 2)
		return false;

	codeclib_key_value_parse(codec_opts, codeclib_avc_video_enc_options, enc);

	return true;
}


static const codec_type_t codec_type_h264 = {
	.def_init = avc_def_init,
	.decoder_init = avc_video_decoder_init,
	.decoder_input = h264_decoder_input,
	.decoder_close = avc_decoder_close,
	.encoder_init = avc_encoder_init,
	.encoder_input = avc_video_encoder_input,
	.encoder_close = avc_encoder_close,
};


static bool packetize_nals(str_q *q, const uint8_t *pkt, size_t len) {
	// libx264 produces an initial 4-byte start code \0\0\0\1 + NAL code byte + NAL unit data
	if (len < 6) {
		ilog(LOG_ERR | LOG_FLAG_LIMIT, "Short H.264 frame");
		return false;
	}

	str p = STR_LEN(pkt, len);

	// expect a long (4-byte) start code at the beginning
	if (memcmp(pkt, "\0\0\0\1", 4) == 0)
		str_shift(&p, 4);
	else {
		ilog(LOG_ERR | LOG_FLAG_LIMIT, "H.264 frame does not start with a NAL unit");
		return false;
	}

	while (p.len > 0) {
		uint8_t nal = p.s[0];

		// subsequent start codes are 3-byte
		char *next = memmem(p.s, p.len, "\0\0\1", 3);

		str unit = STR_NULL;

		if (!next) // use up remainder
			str_shift_ret(&p, p.len, &unit);
		else {
			str_shift_ret(&p, next - p.s, &unit);
			str_shift(&p, 3);
		}

		if ((nal & 0x80))
			ilog(LOG_WARN | LOG_FLAG_LIMIT, "Forbidden NAL bit is set, ignoring unit");
		else if (unit.len) // can't have empty NALs
			t_queue_push_tail(q, str_slice_dup(&unit));
	}

	return true;
}


static int nal_more(encoder_t *enc) {
	return enc->nals.length ? 1 : 0;
}


static int nal_fragment(encoder_t *enc, str *output, str *first) {
	if (!enc->avc.nal_frag && first->len <= rtpe_common_config_ptr->video_pkt_size)
		return -1; // not a fragment

	size_t part = first->len - enc->avc.nal_frag;
	if (part > rtpe_common_config_ptr->video_pkt_size)
		part = rtpe_common_config_ptr->video_pkt_size;

	uint8_t nal = first->s[0];

	output->s[0] = 28; // FU-A
	output->s[0] |= nal & 0x60; // NRI

	output->s[1] = nal & 0x1f; // fragmented NAL type

	if (!enc->avc.nal_frag) {
		output->s[1] |= 0x80; // start bit
		enc->avc.nal_frag++;
		part--;
	}

	memcpy(output->s + 2, first->s + enc->avc.nal_frag, part);
	enc->avc.nal_frag += part;

	if (enc->avc.nal_frag == first->len) {
		output->s[1] |= 0x40; // end bit
		t_queue_pop_head(&enc->nals);
		g_free(first);
		enc->avc.nal_frag = 0;
	}

	output->len = part + 2;

	return nal_more(enc);
}


static int nal_aggregate(encoder_t *enc, str *output, str_list *head) {
	if (!head->next)
		return -1;
	if (head->data->len + head->next->data->len > rtpe_common_config_ptr->video_pkt_size)
		return -1;

	output->s[0] = 24; // STAP-A
	uint8_t nri = 0;
	output->len = 1;

	while (head && output->len + head->data->len <= rtpe_common_config_ptr->video_pkt_size + 2) {
		str *nal = head->data;

		uint16_t *l = (uint16_t *) &output->s[output->len];
		*l = htons(nal->len);
		output->len += 2;

		uint8_t n = nal->s[0] & 0x60;
		if (n > nri) {
			output->s[0] = (output->s[0] & 0x9f) | n;
			nri = n;
		}

		memcpy(&output->s[output->len], nal->s, nal->len);
		output->len += nal->len;

		g_free(nal);
		t_queue_pop_head(&enc->nals);

		head = enc->nals.head;
	}

	return nal_more(enc);
}



static int packetizer_h264_fn(AVPacket *pkt, str *output, size_t num_bytes, encoder_t *enc,
		int64_t *__restrict pts, int64_t *__restrict duration, bool *mark)
{
	if (pkt) {
		if (pkt->size < 1)
			return -1;

		// new NALs, queue should be empty
		if (enc->nals.length) {
			ilog(LOG_WARN, "Old H.264 NAL queue not empty!");
			t_queue_clear_full(&enc->nals, str_slice_free);
		}

		ilog(LOG_DEBUG, "Packetising %u bytes of H.264", pkt->size);

		enc->packet_pts = pkt->pts;

		if (!packetize_nals(&enc->nals, pkt->data, pkt->size))
			return -1;
	}

	if (!enc->nals.length) {
		ilog(LOG_ERR | LOG_FLAG_LIMIT, "Zero output NAL units");
		return -1;
	}

	*pts = enc->packet_pts;

	str_list *head = enc->nals.head;
	str *first = head->data;

	// is this a fragment, or do we need to fragment it?
	int ret = nal_fragment(enc, output, first);
	if (ret >= 0)
		goto out;

	// can we aggregate at least two NALs?
	ret = nal_aggregate(enc, output, head);
	if (ret >= 0)
		goto out;

	// single NAL
	memcpy(output->s, first->s, first->len);
	output->len = first->len;

	t_queue_pop_head(&enc->nals);
	g_free(first);

	ret = nal_more(enc);

out:
	*mark = ret == 0 ? true : false;
	return ret;
}


packetizer_t packetizer_h264 = {
	.fn = packetizer_h264_fn,
};


static uint8_t dehex_digit(char p) {
	if (p >= '0' && p <= '9')
		return p - '0';
	if (p >= 'a' && p <= 'f')
		return p - 'a';
	if (p >= 'A' && p <= 'F')
		return p - 'A';
	return -1;
}


static uint8_t dehex(const char *p) {
	return (dehex_digit(p[0]) << 4) | dehex_digit(p[1]);
}


static void h264_fmtp_parse_cb(str *key, str *token, void *data) {
	union codec_format_options *opts = data;

	if (str_eq(key, "packetization-mode"))
		opts->h264.packetization_mode = str_to_i(token, 0);
	if (str_eq(key, "level-asymmetry-allowed"))
		opts->h264.level_asymmetry_allowed = str_to_i(token, 0);
	else if (str_eq(key, "profile-level-id") && token->len == 6) {
		opts->h264.profile_idc = dehex(&token->s[0]);
		opts->h264.profile_iop = dehex(&token->s[2]);
		opts->h264.level_idc = dehex(&token->s[4]);
	}
}


static bool h264_fmtp_parse(struct rtp_codec_format *f, const str *fmtp) {
	codeclib_key_value_parse(fmtp, h264_fmtp_parse_cb, f);
	return true;
}


static int h264_format_cmp(const struct rtp_payload_type *A, const struct rtp_payload_type *B) {
	// params must have been parsed successfully
	if (!A->format.fmtp_parsed || !B->format.fmtp_parsed)
		return -1;

	__auto_type a = &A->format.parsed.h264;
	__auto_type b = &B->format.parsed.h264;

	if (a->packetization_mode != b->packetization_mode)
		return -1;

	// ignore profile indication for now
	if (0 && (!a->level_asymmetry_allowed || !b->level_asymmetry_allowed)) {
		if (a->profile_idc != b->profile_idc)
			return -1;
		if (a->profile_iop != b->profile_iop)
			return -1;
		// for baseline, main, and extended, also match constraint_set3_flag
		if (a->profile_idc == 'B' || a->profile_idc == 'M' || a->profile_idc == 'X') {
			if ((a->profile_iop & 0x10) != (b->profile_iop & 0x10))
				return -1;
		}
	}

	return 0;
}


static bool h264_format_supported(const struct rtp_payload_type *pt) {
	if (!pt->format.fmtp_parsed)
		return false;
	if (pt->format.parsed.h264.packetization_mode == 2)
		return false;
	return true;
}


static const codec_def_t h264 = {
	.rtpname = "H264",
	.avcodec_id = AV_CODEC_ID_H264,
	.default_width = 1280,
	.default_height = 720,
	.avcodec_name_enc = "libx264",
	.avcodec_name_dec = "h264",
	.default_bitrate = 2000000,
	.packetizer = &packetizer_h264,
	.media_type = MT_VIDEO,
	.codec_type = &codec_type_h264,
	.set_enc_options = set_enc_options,
	.format_cmp = h264_format_cmp,
	.default_fmtp = "packetization-mode=1;max-fs=8160;level-asymmetry-allowed=1;profile-level-id=42001f",
	.format_parse = h264_fmtp_parse,
	.format_supported = h264_format_supported,
};


__attribute__((constructor))
static void init(void) {
	codeclib_register_codec(&h264);
}
