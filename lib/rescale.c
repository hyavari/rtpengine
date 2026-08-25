#include "rescale.h"
#include <glib.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/pixdesc.h>
#include <inttypes.h>
#include <libavutil/frame.h>
#include "loglib.h"
#include "codeclib.h"


static const char *rescale_init(rescale_t *rescale, const format_t *to_format, const format_t *from_format)
{
	avfilter_graph_free(&rescale->filter);

	rescale->filter = avfilter_graph_alloc();
	if (!rescale->filter)
		return "failed to alloc filter graph";
	rescale->filter->nb_threads = 4;
	rescale->filter->thread_type = 0;

	rescale->to_format = *to_format;
	rescale->from_format = *from_format;

	format_t aspect_format = *to_format;

	const AVFilter *filter = avfilter_get_by_name("buffer");
	if (!filter)
		return "buffer filter not available";

	char opts[64];
	snprintf(opts, sizeof(opts), "width=%d:height=%d:pix_fmt=%s:time_base=1/1",
			from_format->width, from_format->height,
			av_get_pix_fmt_name(from_format->pix_fmt));
	if (avfilter_graph_create_filter(&rescale->src, filter, NULL,
				opts, NULL, rescale->filter))
		return "failed to configure buffer filter";

	AVFilterContext *link = rescale->src;

	if (from_format->height != to_format->height || from_format->width != to_format->width) {
		filter = avfilter_get_by_name("scale");
		if (!filter)
			return "scale filter not available";

		// maintain aspect ratio
		int asp_height = to_format->width * from_format->height / from_format->width;
		int diff = asp_height - to_format->height;
		if (diff > 0) {
			// height too large: use requested height and reduce width to fit
			aspect_format.height = to_format->height;
			aspect_format.width = to_format->height * from_format->height / from_format->width;
		}
		else if (diff < -3) {
			// height too small: use aspect-adjusted height
			aspect_format.height = asp_height;
		}
		// else: close enough: use exact requested height

		snprintf(opts, sizeof(opts), "w=%d:h=%d", aspect_format.width, aspect_format.height);
		if (avfilter_graph_create_filter(&rescale->scale, filter, NULL,
					opts, NULL, rescale->filter))
			return "failed to configure scale filter";

		if (avfilter_link(link, 0, rescale->scale, 0))
			return "failed to link scale filter";
		link = rescale->scale;
	}

	rescale->aspect_format = aspect_format;

	if (memcmp(&aspect_format, to_format, sizeof(aspect_format))) {
		filter = avfilter_get_by_name("pad");
		if (!filter)
			return "pad filter not available";

		snprintf(opts, sizeof(opts), "w=%d:h=%d:x=%d:y=%d",
				to_format->width, to_format->height,
				(to_format->width - aspect_format.width) / 2,
				(to_format->height - aspect_format.height) / 2);
		if (avfilter_graph_create_filter(&rescale->pad, filter, NULL,
					opts, NULL, rescale->filter))
			return "failed to configure pad filter";

		if (avfilter_link(link, 0, rescale->pad, 0))
			return "failed to link pad filter";
		link = rescale->pad;
	}

	if (to_format->pix_fmt != from_format->format) {
		filter = avfilter_get_by_name("format");
		if (!filter)
			return "format filter not available";

		snprintf(opts, sizeof(opts), "pix_fmts=%s", av_get_pix_fmt_name(to_format->pix_fmt));
		if (avfilter_graph_create_filter(&rescale->format, filter, NULL,
					opts, NULL, rescale->filter))
			return "failed to configure scale filter";

		if (avfilter_link(link, 0, rescale->format, 0))
			return "failed to link format filter";
		link = rescale->format;
	}

	filter = avfilter_get_by_name("buffersink");
	if (!filter)
		return "buffersink filter not available";

	if (avfilter_graph_create_filter(&rescale->sink, filter, NULL,
				NULL, NULL, rescale->filter))
		return "failed to configure buffersink filter";

	if (avfilter_link(link, 0, rescale->sink, 0))
		return "failed to link buffersink filter";
	link = rescale->sink;

	if (avfilter_graph_config(rescale->filter, NULL))
		return "failed to config filter";

	return NULL;
}


AVFrame *rescale_frame(rescale_t *rescale, AVFrame *frame, const format_t *to_format) {
	const char *err;
	int errcode = 0;

	if (frame->format != to_format->pix_fmt)
		goto resample;
	if (frame->width != to_format->width)
		goto resample;
	if (frame->height != to_format->height)
		goto resample;

	return frame;

resample:;

	format_t from_format = {
		.pix_fmt = frame->format,
		.width = frame->width,
		.height = frame->height,
	};

	if (G_UNLIKELY(!rescale->filter
				|| memcmp(&rescale->to_format, to_format, sizeof(*to_format))
				|| memcmp(&rescale->from_format, &from_format, sizeof(from_format))))
	{
		err = rescale_init(rescale, to_format, &from_format);
		if (err)
			goto err;
	}

	int ret = av_buffersrc_add_frame(rescale->src, frame);
	err = "failed to add frame to filter";
	if (ret)
		goto err;

	AVFrame *swr_frame = av_frame_alloc();
	err = "failed to alloc rescaling frame";
	if (!swr_frame)
		goto err;

	ret = av_buffersink_get_frame(rescale->sink, swr_frame);
	err = "failed to retrieve frame from filter";
	if (ret)
		goto err;

	return swr_frame;

err:
	if (errcode)
		ilog(LOG_ERR | LOG_FLAG_LIMIT, "Error rescaling: %s (%s)", err, av_error(errcode));
	else
		ilog(LOG_ERR | LOG_FLAG_LIMIT, "Error rescaling: %s", err);
	rescale_shutdown(rescale);
	return NULL;
}


void rescale_shutdown(rescale_t *rescale) {
	if (rescale->src)
		avfilter_free(rescale->src);
	rescale->src = NULL;

	if (rescale->pad)
		avfilter_free(rescale->pad);
	rescale->pad = NULL;

	if (rescale->scale)
		avfilter_free(rescale->scale);
	rescale->scale = NULL;

	if (rescale->format)
		avfilter_free(rescale->format);
	rescale->format = NULL;

	if (rescale->sink)
		avfilter_free(rescale->sink);
	rescale->sink = NULL;

	avfilter_graph_free(&rescale->filter);
	rescale->to_format.pix_fmt = -1;
	rescale->from_format.pix_fmt = -1;
}
