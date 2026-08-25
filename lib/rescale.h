#ifndef _RESCALE_H_
#define _RESCALE_H_


#include "codeclib.h"
#include <libavutil/frame.h>


AVFrame *rescale_frame(rescale_t *rescale, AVFrame *frame, const format_t *to_format);
void rescale_shutdown(rescale_t *rescale);


#endif
