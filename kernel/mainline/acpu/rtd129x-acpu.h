/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Realtek RTD129x audio CPU: the RPC used to talk to its firmware
 * (bluecore.audio), and the parts of the firmware's interface the HDMI
 * receiver's audio capture uses. Layouts and numbers are from Realtek's BSP
 * (drivers/soc/realtek/common/rpc, sound/arm/snd-realtek*), which talks to
 * the same firmware.
 *
 * The firmware is big-endian MIPS: everything it reads in a message or in
 * a ring buffer header is big-endian. It addresses memory through KSEG1,
 * so a physical address is passed to it as phys | 0xa0000000.
 */
#ifndef _RTD129X_ACPU_H
#define _RTD129X_ACPU_H

#include <linux/auxiliary_bus.h>
#include <linux/types.h>

struct rtd_acpu;

/* Memory both CPUs can reach: the audio heap, mapped uncached. */
struct rtd_acpu_buf {
	void *vaddr;
	phys_addr_t phys;
	size_t size;
};

static inline u32 rtd_acpu_addr(phys_addr_t phys)
{
	return (u32)phys | 0xa0000000;
}

/* The firmware's success code, in the reply and in its result structures */
#define RTD_ACPU_S_OK		0x10000000

/* "Kernel RPC" commands (ENUM_AUDIO_KERNEL_RPC_CMD in the BSP) */
enum {
	RTD_ACPU_CREATE_AGENT	= 0,
	RTD_ACPU_INIT_RINGBUF	= 1,
	RTD_ACPU_PRIVATEINFO	= 2,
	RTD_ACPU_RUN		= 3,
	RTD_ACPU_PAUSE		= 4,
	RTD_ACPU_STOP		= 19,
	RTD_ACPU_CHECK_READY	= 20,
	RTD_ACPU_ADC0_CONFIG	= 23,
	RTD_ACPU_AIO_PRIVATEINFO = 37,
};

int rtd_acpu_call(struct rtd_acpu *acpu, u32 cmd, u32 param, u32 result,
		  u32 *ret);
int rtd_acpu_rpc(struct rtd_acpu *acpu, u32 procedure, const __be32 *args,
		 u32 nargs, __be32 *res, u32 nres);
void rtd_acpu_vo_kick(struct rtd_acpu *acpu);
phys_addr_t rtd_acpu_media_alloc(struct rtd_acpu *acpu, size_t size);
void rtd_acpu_media_free(struct rtd_acpu *acpu, phys_addr_t phys, size_t size);
int rtd_acpu_alloc(struct rtd_acpu *acpu, size_t size, struct rtd_acpu_buf *buf);
void rtd_acpu_free(struct rtd_acpu *acpu, struct rtd_acpu_buf *buf);

/* The auxiliary device the core creates for the HDMI audio capture */
struct rtd_acpu_adev {
	struct auxiliary_device adev;
	struct rtd_acpu *acpu;
};

#define RTD_ACPU_PCM_NAME	"pcm"
#define RTD_ACPU_VO_NAME	"vo"	/* the video output */

#endif
