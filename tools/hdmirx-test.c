/*
 * hdmirx-test -- capture rtk_hdmirx frames following the order Android's tv_input HAL uses.
 *
 * Why it exists: the driver does not implement VIDIOC_G_FMT, and both v4l2-ctl
 * and ustreamer call G_FMT before S_FMT and give up on S_FMT when it fails.
 * That leaves mipi_top.pitch / v_output_len at 0 and the MIPI wrapper never
 * produces a frame.
 *
 * This program issues S_FMT directly (exactly as the HAL does) to confirm the
 * capture path itself works.
 *
 *   usage: hdmirx-test [frames] [outfile]
 * Resolution and pixel format come from the hdmirx_video_info sysfs attribute.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/videodev2.h>

#define NBUF 4

static int sysfs_int(const char *path, const char *key)
{
	FILE *f = fopen(path, "r");
	char line[128];
	int val = -1;
	size_t klen = strlen(key);

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, klen) && line[klen] == ':')
			val = atoi(line + klen + 1);
	}
	fclose(f);
	return val;
}

static int sysfs_str(const char *path, const char *key, char *out, size_t n)
{
	FILE *f = fopen(path, "r");
	char line[128];
	size_t klen = strlen(key);
	int found = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, klen) && line[klen] == ':') {
			char *v = line + klen + 1;
			char *nl = strchr(v, '\n');
			if (nl)
				*nl = 0;
			snprintf(out, n, "%s", v);
			found = 1;
		}
	}
	fclose(f);
	return found ? 0 : -1;
}

/* Find the video node carrying the hdmirx_video_info attribute (the driver falls
   back to video0 when it cannot get minor 250) */
static int find_dev(char *devpath, size_t dn, char *infopath, size_t in)
{
	int i;
	for (i = 0; i < 64; i++) {
		char p[128];
		snprintf(p, sizeof(p),
			 "/sys/class/video4linux/video%d/hdmirx_video_info", i);
		if (access(p, R_OK) == 0) {
			snprintf(devpath, dn, "/dev/video%d", i);
			snprintf(infopath, in, "%s", p);
			return 0;
		}
	}
	return -1;
}

int main(int argc, char **argv)
{
	int frames = (argc > 1) ? atoi(argv[1]) : 20;
	const char *outfn = (argc > 2) ? argv[2] : "/tmp/hdmirx.raw";
	char devpath[64], infopath[128], status[32];
	int fd, out, i, n;
	int width, height, fps;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	void *bufs[NBUF];
	size_t lens[NBUF];
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct timeval t0, t1;

	if (find_dev(devpath, sizeof(devpath), infopath, sizeof(infopath))) {
		fprintf(stderr, "no rtk_hdmirx device found\n");
		return 1;
	}

	if (sysfs_str(infopath, "Status", status, sizeof(status)) ||
	    strcmp(status, "Ready")) {
		fprintf(stderr, "signal not ready (Status=%s)\n", status);
		return 1;
	}
	width  = sysfs_int(infopath, "Width");
	height = sysfs_int(infopath, "Height");
	fps    = sysfs_int(infopath, "Fps");
	printf("device %s  detected %dx%d @%dfps\n", devpath, width, height, fps);

	if (width <= 0 || height <= 0) {
		fprintf(stderr, "sysfs reported an invalid geometry\n");
		return 1;
	}

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/*
	 * Clear any leftover streaming state. The order matters:
	 *
	 * This driver only calls vb2_queue_init() inside VIDIOC_REQBUFS. Issuing
	 * VIDIOC_STREAMOFF before that makes vb2_streamoff() operate on a queue
	 * that was never initialised -- dev comes from kzalloc, so
	 * q->done_wq.task_list is all zeros and __vb2_queue_cancel()'s
	 * wake_up_all() NULL-derefs:
	 *
	 *   Unable to handle kernel NULL pointer dereference at 00000000
	 *   PC is at __wake_up_common+0x38/0xa0
	 *   [<...>] __vb2_queue_cancel+0x7c/0x188
	 *   [<...>] vb2_core_streamoff+0x54/0xb8
	 *   [<...>] v4l2_hdmi_do_ioctl+0x600/0x6f8
	 *
	 * So issue REQBUFS(count=0) first to get the queue initialised. Only when
	 * that reports -EBUSY (meaning it really is still streaming) is STREAMOFF
	 * needed, and by then the queue is initialised.
	 */
	{
		struct v4l2_requestbuffers z;
		memset(&z, 0, sizeof(z));
		z.count = 0;
		z.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		z.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd, VIDIOC_REQBUFS, &z) < 0 && errno == EBUSY) {
			printf("queue still streaming, issuing STREAMOFF first\n");
			ioctl(fd, VIDIOC_STREAMOFF, &type);
			memset(&z, 0, sizeof(z));
			z.count = 0;
			z.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			z.memory = V4L2_MEMORY_MMAP;
			ioctl(fd, VIDIOC_REQBUFS, &z);
		}
	}

	/* S_FMT directly, with no preceding G_FMT -- exactly what the HAL does */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV16;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		return 1;
	}
	printf("S_FMT succeeded\n");

	memset(&req, 0, sizeof(req));
	req.count = NBUF;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS");
		return 1;
	}
	printf("REQBUFS returned %u buffers\n", req.count);

	for (i = 0; i < (int)req.count && i < NBUF; i++) {
		struct v4l2_buffer b;
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
			perror("VIDIOC_QUERYBUF");
			return 1;
		}
		lens[i] = b.length;
		bufs[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, b.m.offset);
		if (bufs[i] == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
			perror("VIDIOC_QBUF");
			return 1;
		}
	}
	printf("each buffer is %zu bytes\n", lens[0]);

	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON");
		return 1;
	}
	printf("STREAMON, capturing %d frames...\n", frames);

	out = open(outfn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	gettimeofday(&t0, NULL);

	for (n = 0; n < frames; n++) {
		struct v4l2_buffer b;
		fd_set fds;
		struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };

		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
			fprintf(stderr, "frame %d timed out: MIPI produced nothing\n", n);
			break;
		}

		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) {
			perror("VIDIOC_DQBUF");
			break;
		}
		if (out >= 0)
			write(out, bufs[b.index], b.bytesused ? b.bytesused : lens[b.index]);
		if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
			perror("VIDIOC_QBUF");
			break;
		}
	}

	gettimeofday(&t1, NULL);
	ioctl(fd, VIDIOC_STREAMOFF, &type);
	if (out >= 0)
		close(out);
	close(fd);

	{
		double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
		printf("captured %d frames in %.2f s (%.1f fps) -> %s\n",
		       n, sec, n > 0 ? n / sec : 0.0, outfn);
	}
	return n > 0 ? 0 : 1;
}
