#include "codecmod.h"
#include "loglib.h"


static void vp8_decode_frame(decoder_t *dec, frame_q *out) {
	dec->dec_ts = dec->rtp_ts;

	if (dec->packet_buffer->len) {
		ilog(LOG_DEBUG, "Decoding %zu bytes of VP8", dec->packet_buffer->len);

		avc_decoder_input(dec, &STR_GS(dec->packet_buffer), out, false);
	}

	g_string_truncate(dec->packet_buffer, 0);
}


static int vp8_decoder_input(decoder_t *dec, const str *data, frame_q *out, bool mark) {
	if (!data)
		goto err;

	if (dec->rtp_ts != dec->dec_ts)
		vp8_decode_frame(dec, out);

	str frame = *data;
	str b;

	if (str_shift_ret(&frame, 1, &b))
		goto err;
	uint8_t xrnsr_pid = b.s[0];

	if ((xrnsr_pid & 0x80)) { // X bit
		if (str_shift_ret(&frame, 1, &b))
			goto err;
		uint8_t x = b.s[0];

		if ((x & 0x80)) { // I bit
			if (str_shift_ret(&frame, 1, &b))
				goto err;

			uint8_t m_pid = b.s[0];
			if ((m_pid & 0x80)) { // M bit
				if (str_shift_ret(&frame, 1, &b))
					goto err;
				// XXX 2nd byte PID
			}
		}

		if ((x & 0x40)) { // L bit
			if (!str_shift_ret(&frame, 1, &b))
				goto err;
		}

		if ((x & 0x30)) { // T or K bit
			if (!str_shift_ret(&frame, 1, &b))
				goto err;
		}
	}

	if ((xrnsr_pid & 0x10) && (xrnsr_pid & 0x7) == 0) {
		// start of first partition: new frame
		if (dec->packet_buffer->len != 0)
			ilog(LOG_WARN | LOG_FLAG_LIMIT, "New VP8 frame while decode buffer is not empty, "
					"discarding %zu bytes", dec->packet_buffer->len);

		g_string_truncate(dec->packet_buffer, 0);

		if (frame.len < 3)
			goto err;
		//uint8_t size0_h_ver_p = frame.s[0];
	}

	g_string_append_len(dec->packet_buffer, frame.s, frame.len);

	if (mark)
		vp8_decode_frame(dec, out);

	return 0;

err:
	ilog(LOG_ERR | LOG_FLAG_LIMIT, "Invalid VP8 packet");
	return -1;
}


static bool set_enc_options(encoder_t *enc, const str *codec_opts) {
	// defaults
	enc->avc.avcctx->gop_size = 10;

	codeclib_set_av_opt_int(enc, "lag-in-frames", 0);
	codeclib_set_av_opt_int(enc, "cpu-used", 16);
	codeclib_set_av_opt_int(enc, "deadline", 5000);
	codeclib_set_av_opt_int(enc, "threads", 8);

	codeclib_key_value_parse(codec_opts, codeclib_avc_video_enc_options, enc);

	return true;
}


static const codec_type_t codec_type_vp8 = {
	.def_init = avc_def_init,
	.decoder_init = avc_video_decoder_init,
	.decoder_input = vp8_decoder_input,
	.decoder_close = avc_decoder_close,
	.encoder_init = avc_encoder_init,
	.encoder_input = avc_video_encoder_input,
	.encoder_close = avc_encoder_close,
};


static int packetizer_vp8_fn(AVPacket *pkt, str *output, size_t num_bytes, encoder_t *enc,
		int64_t *__restrict pts, int64_t *__restrict duration, bool *mark)
{
	size_t out_len = 0;
	GString *buf = enc->sample_buffer;

	output->s[0] = 0x80; // xrnsr_pid
	out_len++;

	if (pkt) {
		if (pkt->size < 3)
			return -1;

		// new frame, move into buffer
		g_string_truncate(buf, 0);
		g_string_append_len(buf, (const char *) pkt->data, pkt->size);
		ilog(LOG_DEBUG, "Packetising %u bytes of VP8", pkt->size);

		enc->packet_pts = pkt->pts;
		enc->frame_idx++;

		output->s[0] |= 0x10; // S bit

		if ((pkt->data[0] & 0x01)) // P bit
			output->s[0] |= 0x20; // N bit
	}

	output->s[1] = 0x80; // iltk_rsv
	output->s[2] = 0x80 | ((enc->frame_idx >> 8) & 0x7f); // M + PID
	output->s[3] = enc->frame_idx & 0xff; // PID
	out_len += 3;

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

	return 0;
}

packetizer_t packetizer_vp8 = {
	.init = packetizer_buffered_init,
	.fn = packetizer_vp8_fn,
	.destroy = packetizer_buffered_destroy,
};


static const codec_def_t vp8 = {
	.rtpname = "VP8",
	.avcodec_id = AV_CODEC_ID_VP8,
	.default_width = 1280,
	.default_height = 720,
	.avcodec_name_enc = "libvpx",
	.avcodec_name_dec = "libvpx",
	.default_bitrate = 2000000,
	.packetizer = &packetizer_vp8,
	.media_type = MT_VIDEO,
	.codec_type = &codec_type_vp8,
	.set_enc_options = set_enc_options,
	.format_cmp = format_cmp_ignore,
	.default_fmtp = "max-fr:30;max-fs:1800",
};


__attribute__((constructor))
static void init(void) {
	codeclib_register_codec(&vp8);
}
