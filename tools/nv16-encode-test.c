/*
 * Call ustreamer's CPU JPEG encoder directly to turn one raw frame into a JPEG.
 * Used to verify the newly added semi-planar (NV12/NV21/NV16) path.
 *
 *   nv16-encode-test <raw-file> <width> <height> <NV16|NV12|NV21> <out.jpg>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/videodev2.h>

#include "libs/types.h"
#include "libs/frame.h"
#include "ustreamer/encoders/cpu/encoder.h"

int main(int argc, char **argv) {
	if (argc != 6) {
		fprintf(stderr, "usage: %s <raw> <w> <h> <NV16|NV12|NV21> <out.jpg>\n", argv[0]);
		return 1;
	}
	const uint w = atoi(argv[2]);
	const uint h = atoi(argv[3]);

	uint fmt;
	if (!strcmp(argv[4], "NV16")) fmt = V4L2_PIX_FMT_NV16;
	else if (!strcmp(argv[4], "NV12")) fmt = V4L2_PIX_FMT_NV12;
	else if (!strcmp(argv[4], "NV21")) fmt = V4L2_PIX_FMT_NV21;
	else { fprintf(stderr, "unknown format %s\n", argv[4]); return 1; }

	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror("fopen"); return 1; }
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	us_frame_s *src = us_frame_init();
	us_frame_realloc_data(src, size);
	if (fread(src->data, 1, size, f) != (size_t)size) { perror("fread"); return 1; }
	fclose(f);

	src->used = size;
	src->width = w;
	src->height = h;
	src->format = fmt;
	src->stride = w;

	printf("in : %s %ux%u %s  %ld bytes (%.2f bytes/pixel)\n",
	       argv[1], w, h, argv[4], size, (double)size / (w * h));

	us_frame_s *dest = us_frame_init();
	us_cpu_encoder_compress(src, dest, 80);
	printf("out: %s  %zu bytes JPEG\n", argv[5], dest->used);

	FILE *o = fopen(argv[5], "wb");
	fwrite(dest->data, 1, dest->used, o);
	fclose(o);

	us_frame_destroy(src);
	us_frame_destroy(dest);
	return 0;
}
