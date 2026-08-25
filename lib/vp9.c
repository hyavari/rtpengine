#include "codecmod.h"
#include "loglib.h"


static void vp9_decode_frame(decoder_t *dec, frame_q *out) {
	dec->dec_ts = dec->rtp_ts;

	if (dec->packet_buffer->len) {
		ilog(LOG_DEBUG, "Decoding %zu bytes of VP9", dec->packet_buffer->len);

		avc_decoder_input(dec, &STR_GS(dec->packet_buffer), out, false);
	}

	g_string_truncate(dec->packet_buffer, 0);
}


static int vp9_decoder_input(decoder_t *dec, const str *data, frame_q *out, bool mark) {
	if (!data)
		goto err;

	if (dec->rtp_ts != dec->dec_ts)
		vp9_decode_frame(dec, out);

	str frame = *data;
	str b;

	if (str_shift_ret(&frame, 1, &b))
		goto err;
	uint8_t iplfbevz = b.s[0];

	if ((iplfbevz & 0x80)) { // I bit
		if (str_shift_ret(&frame, 1, &b))
			goto err;

		uint8_t m_pid = b.s[0];
		if ((m_pid & 0x80)) { // M bit
			if (str_shift_ret(&frame, 1, &b))
				goto err;
			// XXX 2nd byte PID
		}
	}

	if ((iplfbevz & 0x20)) { // L bit
		if (str_shift_ret(&frame, 1, &b))
			goto err;
		//uint8_t tid_u_sid_d = b.s[0];

		if (!(iplfbevz & 0x10)) {
			// non-flexible mode

			if (str_shift_ret(&frame, 1, &b))
				goto err;
			//uint8_t tl0picidx = b.s[0];
		}
	}

	if ((iplfbevz & 0x50) == 0x50) { // both P and F
		if (str_shift_ret(&frame, 1, &b))
			goto err;
		uint8_t p_diff_n0 = b.s[0];

		if ((p_diff_n0 & 0x01)) {
			if (str_shift_ret(&frame, 1, &b))
				goto err;
			uint8_t p_diff_n1 = b.s[0];

			if ((p_diff_n1 & 0x01)) {
				if (str_shift_ret(&frame, 1, &b))
					goto err;
				uint8_t p_diff_n2 = b.s[0];

				if ((p_diff_n2 & 0x01))
					goto err; // only max 3 allowed
			}
		}
	}

	if ((iplfbevz & 0x02)) { // V bit
		if (str_shift_ret(&frame, 1, &b))
			goto err;
		uint8_t n_s_yg_RRR = b.s[0];

		if ((n_s_yg_RRR & 0x10)) { // Y bit
			uint8_t n_s = (n_s_yg_RRR >> 5) + 1;

			for (uint8_t i = 0; i < n_s; i++) {
				if (str_shift_ret(&frame, 4, &b))
					goto err;
			}
		}

		if ((n_s_yg_RRR & 0x08)) { // G bit
			if (str_shift_ret(&frame, 1, &b))
				goto err;
			uint8_t n_g = b.s[0];

			for (uint8_t i = 0; i < n_g; i++) {
				if (str_shift_ret(&frame, 1, &b))
					goto err;
				uint8_t tid_u_r_RR = b.s[0];

				uint8_t r = (tid_u_r_RR >> 2) & 0x3;
				for (uint8_t j = 0; j < r; j++) {
					if (str_shift_ret(&frame, 1, &b))
						goto err;
					//uint8_t p_diff = b.s[0];
				}
			}
		}
	}

	g_string_append_len(dec->packet_buffer, frame.s, frame.len);

	if (mark)
		vp9_decode_frame(dec, out);

	return 0;

err:
	ilog(LOG_ERR | LOG_FLAG_LIMIT, "Invalid VP9 packet");
	return -1;
}


static bool set_enc_options(encoder_t *enc, const str *codec_opts) {
	// defaults
	enc->avc.avcctx->gop_size = 10;

	codeclib_set_av_opt_int(enc, "lag-in-frames", 0);
	codeclib_set_av_opt_int(enc, "cpu-used", 8);
	codeclib_set_av_opt_int(enc, "deadline", 5000);
	codeclib_set_av_opt_int(enc, "threads", 8);

	codeclib_key_value_parse(codec_opts, codeclib_avc_video_enc_options, enc);

	return true;
}


static const codec_type_t codec_type_vp9 = {
	.def_init = avc_def_init,
	.decoder_init = avc_video_decoder_init,
	.decoder_input = vp9_decoder_input,
	.decoder_close = avc_decoder_close,
	.encoder_init = avc_encoder_init,
	.encoder_input = avc_video_encoder_input,
	.encoder_close = avc_encoder_close,
};


static int packetizer_vp9_fn(AVPacket *pkt, str *output, size_t num_bytes, encoder_t *enc,
		int64_t *__restrict pts, int64_t *__restrict duration, bool *mark)
{
	size_t out_len = 0;
	GString *buf = enc->sample_buffer;

	output->s[0] = 0x80; // iplfbevz
	out_len++;

	if (pkt) {
		if (pkt->size < 3)
			return -1;

		// new frame, move into buffer
		g_string_truncate(buf, 0);
		g_string_append_len(buf, (const char *) pkt->data, pkt->size);
		ilog(LOG_DEBUG, "Packetising %u bytes of VP9", pkt->size);

		enc->packet_pts = pkt->pts;
		enc->frame_idx++;

		output->s[0] |= 0x08; // B bit
	}

	output->s[1] = 0x80 | ((enc->frame_idx >> 8) & 0x7f); // M + PID
	output->s[2] = enc->frame_idx  & 0xff; // PID
	out_len += 2;

	size_t len = MIN(rtpe_common_config_ptr->video_pkt_size, output->len);
	len = MIN(len, buf->len);

	memcpy(output->s + out_len, buf->str, len);
	out_len += len;

	*pts = enc->packet_pts;

	g_string_erase(buf, 0, len);

	output->len = out_len;

	*mark = false;

	if (buf->len)
		return 1;

	*mark = true;
	output->s[0] |= 0x04; // E bit

	return 0;
}

packetizer_t packetizer_vp9 = {
	.init = packetizer_buffered_init,
	.fn = packetizer_vp9_fn,
	.destroy = packetizer_buffered_destroy,
};


static const codec_def_t vp9 = {
	.rtpname = "VP9",
	.avcodec_id = AV_CODEC_ID_VP9,
	.default_width = 1280,
	.default_height = 720,
	.avcodec_name_enc = "libvpx-vp9",
	.avcodec_name_dec = "libvpx-vp9",
	.default_bitrate = 2000000,
	.packetizer = &packetizer_vp9,
	.media_type = MT_VIDEO,
	.codec_type = &codec_type_vp9,
	.set_enc_options = set_enc_options,
	.format_cmp = format_cmp_ignore,
	.default_fmtp = "max-fr:30;max-fs:1800;profile-id=0",
};


__attribute__((constructor))
static void init(void) {
	codeclib_register_codec(&vp9);
}
