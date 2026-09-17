/* Check that touching the vb2 queue before VIDIOC_REQBUFS returns -EINVAL rather than oopsing */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static void try_ioctl(int fd, const char *name, unsigned long req, void *arg)
{
	int r;
	errno = 0;
	r = ioctl(fd, req, arg);
	printf("  %-22s ret=%2d errno=%d (%s)\n", name, r, errno,
	       r < 0 ? strerror(errno) : "-");
}

int main(void)
{
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct v4l2_buffer b;
	int fd = open("/dev/video0", O_RDWR);

	if (fd < 0) { perror("open"); return 1; }
	memset(&b, 0, sizeof(b));
	b.type = t;
	b.memory = V4L2_MEMORY_MMAP;

	printf("Operating on the queue without REQBUFS:\n");
	try_ioctl(fd, "VIDIOC_STREAMOFF", VIDIOC_STREAMOFF, &t);
	try_ioctl(fd, "VIDIOC_STREAMON",  VIDIOC_STREAMON,  &t);
	try_ioctl(fd, "VIDIOC_QBUF",      VIDIOC_QBUF,      &b);
	try_ioctl(fd, "VIDIOC_DQBUF",     VIDIOC_DQBUF,     &b);
	try_ioctl(fd, "VIDIOC_QUERYBUF",  VIDIOC_QUERYBUF,  &b);
	close(fd);
	printf("All calls returned to userspace, no oops\n");
	return 0;
}
