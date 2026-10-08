// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek RTD129x audio CPU (ACPU) RPC
 *
 * The BSP u-boot loads the audio firmware and starts the ACPU on it ("go
 * a"). The firmware brings up the audio hardware, then waits for the
 * system CPU to set audio_rpc_flag in the IPC block before it opens its
 * end of the RPC rings and finishes starting up. On the way it asks the
 * system CPU for memory, over the same RPC, and does not go on until it
 * gets an answer. This driver does what the BSP's rtk_rpc driver does for
 * that: it sets up the rings, answers the firmware's memory requests out
 * of a reserved region, and lets other drivers call into the firmware.
 *
 * The rings live in a 16 KiB area whose layout the firmware hardcodes.
 * Each ring has a record of five words -- buffer, start, end, in, out --
 * in the CPU's byte order, holding ACPU addresses. There are three kinds,
 * and of each a pair per remote CPU, one per direction:
 *   poll  -- user space RPC in the BSP, not used here
 *   intr  -- the firmware's own requests (memory) and their replies
 *   kern  -- kernel calls into the firmware and their replies
 * Pair 0 is the audio firmware, pair 1 the video one. In each pair, ring 0
 * goes system to audio (SA) and ring 1 audio to system (AS).
 *
 * The two CPUs interrupt each other through SB2's CPU_INT register, and
 * mark an interrupt as an RPC one with a bit in the IPC block's
 * vo_int_sync word; the same interrupt also carries display sync events.
 *
 * The firmware prints to the UART the system console is on, and with its
 * debug flag set (as it starts up) it prints "[AO][_AO_if_video_HDMI_mode]
 * HDMI not enabled" every three seconds, about the board's HDMI output.
 * There is no call to change the flag, so once the firmware has set it,
 * it is cleared in the firmware's data, for the one firmware build whose
 * layout is known.
 */
#include <linux/auxiliary_bus.h>
#include <linux/delay.h>
#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "rtd129x-acpu.h"

/* SB2 CPU_INT and CPU_INT_EN */
#define RPC_INT			0x0
#define RPC_INT_EN		0x4
#define RPC_INT_W1		BIT(0)	/* write 1 to the bits given, else 0 */
#define RPC_INT_SA		BIT(1)	/* system to audio */
#define RPC_INT_AS		BIT(3)	/* audio to system */

/* The IPC block (struct rtk_ipc_shm) in the RPC common page; big-endian */
#define IPC			0xc4
#define IPC_AUDIO_RPC_FLAG	(IPC + 0x0c)
#define IPC_VO_INT_SYNC		(IPC + 0x40)
#define SYNC_RPC_NOTIFY		cpu_to_be32(BIT(8))	/* RPC interrupts on */
#define SYNC_RPC_FEEDBACK	cpu_to_be32(BIT(9))	/* this one is RPC */
#define SYNC_VO_NOTIFY		cpu_to_be32(BIT(16))	/* VO interrupts on */
#define SYNC_VO_FEEDBACK	cpu_to_be32(BIT(17))	/* this one is VO */

/* Offsets in the ring area */
#define RING_SIZE		512
#define POLL_RING(i)		(0x0000 + (i) * 0x400)
#define INTR_RING(i)		(0x0200 + (i) * 0x400)
#define POLL_REC(i)		(0x1000 + (i) * 64)
#define INTR_REC(i)		(0x1100 + (i) * 64)
#define KERN_RING(i)		(0x1200 + (i) * 0x200)
#define KERN_REC(i)		(0x1a00 + (i) * 64)
#define NR_RINGS		4	/* SA, AS, and the video CPU's pair */

#define REC_BUF			0
#define REC_START		4
#define REC_END			8
#define REC_IN			12
#define REC_OUT			16

/* RPC_STRUCT, all big-endian */
struct rpc_hdr {
	__be32 program;
	__be32 version;
	__be32 procedure;
	__be32 task;
	__be32 sys_tid;
	__be32 sys_pid;
	__be32 param_size;
	__be32 context;
};

#define PROG_KERNEL		98	/* kernel call; also R_PROGRAM */
#define PROG_REPLY		99
#define PROG_AUDIO_SYSTEM	201	/* the firmware's own program, version 0 */
#define RPC_MAX_WORDS		16

/* R_PROGRAM procedures: the firmware's requests for memory */
#define REMOTE_ALLOC		1
#define REMOTE_FREE		2
#define REMOTE_ALLOC_SECURE	3
/* ...and the address it gets back is in KSEG0 */
#define REMOTE_KSEG0		0x80000000

/* the BSP's; a TV system change takes the firmware a while */
#define CALL_TIMEOUT		msecs_to_jiffies(5000)

/*
 * The debug flag, in bluecore.audio "Audio Version = 166265 (Kylin)",
 * compiled Sep 21 2017: _AO_if_video_HDMI_mode() prints when bit 0 of
 * *ptrDebugFlag is set. Offsets are from the load address, 0x0f900000.
 */
#define FW_PTR_DEBUG_FLAG	0x0035daa4
#define FW_HDMI_MSG		0x003bd230
static const char fw_hdmi_msg[] = "[AO][_AO_if_video_HDMI_mode]HDMI not enabled\n";

static bool fw_debug;
module_param(fw_debug, bool, 0444);
MODULE_PARM_DESC(fw_debug, "Leave the audio firmware's debug output on");

struct remote_alloc {
	struct list_head list;
	phys_addr_t phys;
	size_t size;
};

struct rtd_acpu {
	struct device *dev;
	void __iomem *sb2;
	void __iomem *comm;
	void __iomem *ring;
	u32 ring_acpu;			/* ACPU address of the ring area */

	struct gen_pool *audio;		/* buffers we share with it */
	struct gen_pool *media;		/* what it asks us for */
	struct list_head remote;

	void __iomem *fw;		/* the firmware image and data */
	resource_size_t fw_size;
	bool quiet;

	spinlock_t sa_lock;		/* the SA rings and the interrupt */
	struct work_struct work;

	struct mutex call_lock;
	struct completion call_done;
	u32 call_task;
	u32 call_ret;

	struct mutex rpc_lock;		/* AUDIO_SYSTEM calls, one at a time */
	struct completion rpc_done;
	u32 rpc_context;
	__be32 rpc_res[RPC_MAX_WORDS];
	u32 rpc_nres;

	struct rtd_acpu_adev *pcm;
	struct rtd_acpu_adev *vo;
};

static void __iomem *ring_ptr(struct rtd_acpu *acpu, u32 addr)
{
	return acpu->ring + (addr - acpu->ring_acpu);
}

static u32 rec_get(struct rtd_acpu *acpu, u32 rec, u32 field)
{
	return readl(acpu->ring + rec + field);
}

static u32 ring_used(struct rtd_acpu *acpu, u32 rec)
{
	u32 start = rec_get(acpu, rec, REC_START);
	u32 size = rec_get(acpu, rec, REC_END) - start;

	return (rec_get(acpu, rec, REC_IN) - rec_get(acpu, rec, REC_OUT) +
		size) % size;
}

/* Words go in and out at a 4-byte granule, as the BSP does it */
static int ring_read(struct rtd_acpu *acpu, u32 rec, void *buf, u32 len)
{
	u32 start = rec_get(acpu, rec, REC_START);
	u32 end = rec_get(acpu, rec, REC_END);
	u32 out = rec_get(acpu, rec, REC_OUT);
	u32 tail = end - out;

	if (ring_used(acpu, rec) < len)
		return -EAGAIN;
	if (tail >= len) {
		memcpy_fromio(buf, ring_ptr(acpu, out), len);
		out += ALIGN(len, 4);
	} else {
		memcpy_fromio(buf, ring_ptr(acpu, out), tail);
		memcpy_fromio(buf + tail, ring_ptr(acpu, start), len - tail);
		out = start + ALIGN(len - tail, 4);
	}
	if (out >= end)
		out = start + (out - end);
	writel(out, acpu->ring + rec + REC_OUT);
	return 0;
}

static int ring_write(struct rtd_acpu *acpu, u32 rec, const void *buf, u32 len)
{
	u32 start = rec_get(acpu, rec, REC_START);
	u32 end = rec_get(acpu, rec, REC_END);
	u32 in = rec_get(acpu, rec, REC_IN);
	u32 tail = end - in;

	if (len > end - start - ring_used(acpu, rec) - 1)
		return -ENOSPC;
	if (tail >= len) {
		memcpy_toio(ring_ptr(acpu, in), buf, len);
		in += ALIGN(len, 4);
	} else {
		memcpy_toio(ring_ptr(acpu, in), buf, tail);
		memcpy_toio(ring_ptr(acpu, start), buf + tail, len - tail);
		in = start + ALIGN(len - tail, 4);
	}
	if (in >= end)
		in = start + (in - end);
	/* the data before the pointer that hands it over */
	wmb();
	writel(in, acpu->ring + rec + REC_IN);
	return 0;
}

static void ring_init(struct rtd_acpu *acpu, u32 rec, u32 ring, u32 size)
{
	u32 buf = acpu->ring_acpu + ring;

	writel(buf, acpu->ring + rec + REC_BUF);
	writel(buf, acpu->ring + rec + REC_START);
	writel(buf + size, acpu->ring + rec + REC_END);
	writel(buf, acpu->ring + rec + REC_IN);
	writel(buf, acpu->ring + rec + REC_OUT);
}

/* As rpc_send_interrupt(): flag it as RPC, then raise it */
static void acpu_kick(struct rtd_acpu *acpu)
{
	u32 sync = readl(acpu->comm + IPC_VO_INT_SYNC);

	if (sync & SYNC_RPC_NOTIFY)
		writel(sync | SYNC_RPC_FEEDBACK, acpu->comm + IPC_VO_INT_SYNC);
	writel(RPC_INT_SA | RPC_INT_W1, acpu->sb2 + RPC_INT);
}

static int acpu_send(struct rtd_acpu *acpu, u32 rec, const void *msg, u32 len)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&acpu->sa_lock, flags);
	ret = ring_write(acpu, rec, msg, len);
	if (!ret)
		acpu_kick(acpu);
	spin_unlock_irqrestore(&acpu->sa_lock, flags);
	return ret;
}

static void acpu_reply(struct rtd_acpu *acpu, u32 rec, struct rpc_hdr *req,
		       u32 value)
{
	struct {
		struct rpc_hdr hdr;
		__be32 task;
		__be32 value;
	} msg = {
		.hdr.program = cpu_to_be32(PROG_REPLY),
		.hdr.version = cpu_to_be32(PROG_REPLY),
		.hdr.param_size = cpu_to_be32(8),
		.hdr.context = req->context,
		.task = req->task,
		.value = cpu_to_be32(value),
	};

	if (acpu_send(acpu, rec, &msg, sizeof(msg)))
		dev_err(acpu->dev, "no room for a reply\n");
}

/* The firmware's memory requests, out of the media region */
static u32 acpu_remote_alloc(struct rtd_acpu *acpu, u32 size, bool page_align)
{
	struct genpool_data_align align = {
		.align = page_align ? PAGE_SIZE : SZ_16K,
	};
	struct remote_alloc *ra;
	unsigned long phys;

	size = PAGE_ALIGN(size);
	ra = kzalloc(sizeof(*ra), GFP_KERNEL);
	if (!ra)
		return 0;
	phys = gen_pool_alloc_algo(acpu->media, size, gen_pool_first_fit_align,
				   &align);
	if (!phys) {
		dev_err(acpu->dev, "out of memory for the firmware (%u)\n", size);
		kfree(ra);
		return 0;
	}
	ra->phys = phys;
	ra->size = size;
	list_add(&ra->list, &acpu->remote);
	dev_dbg(acpu->dev, "remote alloc %#x at %#lx\n", size, phys);
	return phys + REMOTE_KSEG0;
}

static void acpu_remote_free(struct rtd_acpu *acpu, u32 addr)
{
	phys_addr_t phys = addr - REMOTE_KSEG0;
	struct remote_alloc *ra;

	list_for_each_entry(ra, &acpu->remote, list)
		if (ra->phys == phys) {
			gen_pool_free(acpu->media, ra->phys, ra->size);
			list_del(&ra->list);
			kfree(ra);
			return;
		}
	dev_warn(acpu->dev, "remote free of unknown %#x\n", addr);
}

/* Turn the firmware's debug output off; see the top of the file */
static void acpu_quiet(struct rtd_acpu *acpu)
{
	char msg[sizeof(fw_hdmi_msg)];
	u32 ptr, off;

	if (acpu->quiet || fw_debug)
		return;
	acpu->quiet = true;

	memcpy_fromio(msg, acpu->fw + FW_HDMI_MSG, sizeof(msg));
	if (memcmp(msg, fw_hdmi_msg, sizeof(msg))) {
		dev_info(acpu->dev, "unknown firmware build, debug output left on\n");
		return;
	}
	ptr = be32_to_cpu(readl(acpu->fw + FW_PTR_DEBUG_FLAG));
	off = (ptr & 0x1fffffff) - 0x0f900000;
	if (!ptr || off >= acpu->fw_size - 4) {
		dev_info(acpu->dev, "debug flag not set up (%#x)\n", ptr);
		return;
	}
	writel(readl(acpu->fw + off) & ~cpu_to_be32(BIT(0)), acpu->fw + off);
	dev_dbg(acpu->dev, "firmware debug output off\n");
}

static void acpu_skip(struct rtd_acpu *acpu, u32 rec, u32 len)
{
	u8 buf[64];

	while (len) {
		u32 n = min_t(u32, len, sizeof(buf));

		if (ring_read(acpu, rec, buf, n))
			return;
		len -= n;
	}
}

/*
 * A reply to one of our AUDIO_SYSTEM calls: the caller's task ID, then the
 * result in XDR, i.e. big-endian words.
 */
static void acpu_rpc_reply(struct rtd_acpu *acpu, struct rpc_hdr *hdr, u32 size)
{
	__be32 buf[RPC_MAX_WORDS + 1];
	u32 n = min_t(u32, ALIGN(size, 4), sizeof(buf));

	if (ring_read(acpu, INTR_REC(1), buf, n))
		return;
	acpu_skip(acpu, INTR_REC(1), ALIGN(size, 4) - n);
	if (!acpu->rpc_context ||
	    be32_to_cpu(hdr->context) != acpu->rpc_context) {
		dev_warn(acpu->dev, "stray reply, context %#x\n",
			 be32_to_cpu(hdr->context));
		return;
	}
	dev_dbg(acpu->dev, "reply, %u bytes: %*ph\n", size, n, buf);
	acpu->rpc_nres = n >= 4 ? n / 4 - 1 : 0;
	memcpy(acpu->rpc_res, buf + 1, acpu->rpc_nres * 4);
	acpu->rpc_context = 0;
	complete(&acpu->rpc_done);
}

/* The firmware's own requests, on the intr AS ring */
static void acpu_do_intr(struct rtd_acpu *acpu)
{
	struct rpc_hdr hdr;
	__be32 arg;
	u32 value;

	while (!ring_read(acpu, INTR_REC(1), &hdr, sizeof(hdr))) {
		u32 size = be32_to_cpu(hdr.param_size);

		if (be32_to_cpu(hdr.program) == PROG_REPLY) {
			acpu_rpc_reply(acpu, &hdr, size);
			continue;
		}
		if (be32_to_cpu(hdr.program) != PROG_KERNEL ||
		    size < sizeof(arg)) {
			dev_dbg(acpu->dev, "dropped program %u procedure %u\n",
				be32_to_cpu(hdr.program),
				be32_to_cpu(hdr.procedure));
			acpu_skip(acpu, INTR_REC(1), ALIGN(size, 4));
			continue;
		}
		if (ring_read(acpu, INTR_REC(1), &arg, sizeof(arg)))
			return;
		acpu_skip(acpu, INTR_REC(1), ALIGN(size, 4) - sizeof(arg));

		switch (be32_to_cpu(hdr.procedure)) {
		case REMOTE_ALLOC:
		case REMOTE_ALLOC_SECURE:
			/* context bit 0: page alignment is enough */
			value = acpu_remote_alloc(acpu, be32_to_cpu(arg),
						  be32_to_cpu(hdr.context) & 1);
			break;
		case REMOTE_FREE:
			acpu_remote_free(acpu, be32_to_cpu(arg));
			value = 0;
			break;
		default:
			dev_warn(acpu->dev, "unknown request %u\n",
				 be32_to_cpu(hdr.procedure));
			value = 0;
		}
		hdr.context &= ~cpu_to_be32(3);
		acpu_reply(acpu, INTR_REC(0), &hdr, value);
		/* the firmware has set its debug flag by its first request */
		acpu_quiet(acpu);
	}
}

/* Replies to our calls, and the firmware's calls into the kernel */
static void acpu_do_kern(struct rtd_acpu *acpu)
{
	struct rpc_hdr hdr;
	__be32 arg[3];

	while (!ring_read(acpu, KERN_REC(1), &hdr, sizeof(hdr))) {
		if (hdr.task) {
			/* nothing is registered for these in this port */
			if (ring_read(acpu, KERN_REC(1), arg, sizeof(arg)))
				return;
			dev_dbg(acpu->dev, "firmware called %u (%#x, %#x)\n",
				be32_to_cpu(arg[0]), be32_to_cpu(arg[1]),
				be32_to_cpu(arg[2]));
			acpu_reply(acpu, KERN_REC(0), &hdr, 0);
			continue;
		}
		if (ring_read(acpu, KERN_REC(1), arg, 2 * sizeof(arg[0])))
			return;
		if (be32_to_cpu(arg[0]) != acpu->call_task) {
			dev_warn(acpu->dev, "stray reply for %u\n",
				 be32_to_cpu(arg[0]));
			continue;
		}
		acpu->call_ret = be32_to_cpu(arg[1]);
		complete(&acpu->call_done);
	}
}

static void acpu_work(struct work_struct *work)
{
	struct rtd_acpu *acpu = container_of(work, struct rtd_acpu, work);

	acpu_do_intr(acpu);
	acpu_do_kern(acpu);
}

static irqreturn_t acpu_irq(int irq, void *data)
{
	struct rtd_acpu *acpu = data;
	u32 sync;

	if (!(readl(acpu->sb2 + RPC_INT) & RPC_INT_AS))
		return IRQ_NONE;
	/* writing the bit with RPC_INT_W1 clear clears it */
	writel(RPC_INT_AS, acpu->sb2 + RPC_INT);

	sync = readl(acpu->comm + IPC_VO_INT_SYNC);
	if (sync & SYNC_RPC_FEEDBACK)
		writel(sync & ~SYNC_RPC_FEEDBACK, acpu->comm + IPC_VO_INT_SYNC);
	/* the rings are cheap to look at, so look whatever the flag says */
	queue_work(system_highpri_wq, &acpu->work);
	return IRQ_HANDLED;
}

/**
 * rtd_acpu_call() - call into the audio firmware
 * @acpu: the audio CPU
 * @cmd: RTD_ACPU_* command
 * @param: ACPU address of the command's argument structure
 * @result: ACPU address of where the command writes its result
 * @ret: the HRESULT the call returns, RTD_ACPU_S_OK on success
 */
int rtd_acpu_call(struct rtd_acpu *acpu, u32 cmd, u32 param, u32 result,
		  u32 *ret)
{
	struct {
		struct rpc_hdr hdr;
		__be32 arg[3];
	} msg = {
		.hdr.program = cpu_to_be32(PROG_KERNEL),
		.hdr.version = cpu_to_be32(PROG_KERNEL),
		.hdr.param_size = cpu_to_be32(sizeof(msg.arg)),
		.arg = { cpu_to_be32(cmd), cpu_to_be32(param),
			 cpu_to_be32(result) },
	};
	int err;

	mutex_lock(&acpu->call_lock);
	acpu->call_task = task_pid_nr(current);
	msg.hdr.task = cpu_to_be32(acpu->call_task);
	reinit_completion(&acpu->call_done);
	err = acpu_send(acpu, KERN_REC(0), &msg, sizeof(msg));
	if (!err && !wait_for_completion_timeout(&acpu->call_done,
						 CALL_TIMEOUT))
		err = -ETIMEDOUT;
	if (!err && ret)
		*ret = acpu->call_ret;
	acpu->call_task = 0;
	mutex_unlock(&acpu->call_lock);
	if (err)
		dev_err(acpu->dev, "call %u failed: %d\n", cmd, err);
	return err;
}
EXPORT_SYMBOL_GPL(rtd_acpu_call);

/**
 * rtd_acpu_vo_kick() - tell the video output there is a new command in its
 * ring, as the BSP's dc2vo_send_interrupt()
 * @acpu: the audio CPU
 */
void rtd_acpu_vo_kick(struct rtd_acpu *acpu)
{
	unsigned long flags;
	u32 sync;

	spin_lock_irqsave(&acpu->sa_lock, flags);
	sync = readl(acpu->comm + IPC_VO_INT_SYNC);
	if (sync & SYNC_VO_NOTIFY) {
		writel(sync | SYNC_VO_FEEDBACK, acpu->comm + IPC_VO_INT_SYNC);
		writel(RPC_INT_SA | RPC_INT_W1, acpu->sb2 + RPC_INT);
	}
	spin_unlock_irqrestore(&acpu->sa_lock, flags);
}
EXPORT_SYMBOL_GPL(rtd_acpu_vo_kick);

/**
 * rtd_acpu_rpc() - call one of the firmware's AUDIO_SYSTEM procedures
 * @acpu: the audio CPU
 * @procedure: the procedure number (AudioRPC_System.h in Realtek's SDK)
 * @args: the arguments, XDR-encoded: one big-endian word per member
 * @nargs: number of words in @args
 * @res: where the result goes, XDR-encoded as well
 * @nres: number of words @res has room for
 *
 * These go over the intr rings, as Realtek's user-space RPC library sends
 * them; the video output procedures are among them on this SoC.
 *
 * Return: the number of result words received, or a negative error.
 */
int rtd_acpu_rpc(struct rtd_acpu *acpu, u32 procedure, const __be32 *args,
		 u32 nargs, __be32 *res, u32 nres)
{
	static u32 context;
	struct {
		struct rpc_hdr hdr;
		__be32 arg[RPC_MAX_WORDS];
	} msg = {
		.hdr.program = cpu_to_be32(PROG_AUDIO_SYSTEM),
		.hdr.procedure = cpu_to_be32(procedure),
		.hdr.param_size = cpu_to_be32(nargs * 4),
	};
	int err;

	if (nargs > RPC_MAX_WORDS)
		return -EINVAL;
	memcpy(msg.arg, args, nargs * 4);

	mutex_lock(&acpu->rpc_lock);
	msg.hdr.task = cpu_to_be32(task_pid_nr(current));
	msg.hdr.sys_tid = msg.hdr.task;
	/* never 0, which means "no call in flight" */
	context = (context + 1) & 0x7fffffff ?: 1;
	msg.hdr.context = cpu_to_be32(context);
	reinit_completion(&acpu->rpc_done);
	WRITE_ONCE(acpu->rpc_context, context);
	err = acpu_send(acpu, INTR_REC(0), &msg, sizeof(msg.hdr) + nargs * 4);
	if (!err && !wait_for_completion_timeout(&acpu->rpc_done,
						 CALL_TIMEOUT))
		err = -ETIMEDOUT;
	WRITE_ONCE(acpu->rpc_context, 0);
	if (!err) {
		err = min(acpu->rpc_nres, nres);
		memcpy(res, acpu->rpc_res, err * 4);
	}
	mutex_unlock(&acpu->rpc_lock);
	if (err < 0)
		dev_err(acpu->dev, "procedure %u failed: %d\n", procedure, err);
	return err;
}
EXPORT_SYMBOL_GPL(rtd_acpu_rpc);

/*
 * Memory out of the region the firmware gets its own buffers from, which
 * it can reach (below 512 MiB). Physical; the caller maps it.
 */
phys_addr_t rtd_acpu_media_alloc(struct rtd_acpu *acpu, size_t size)
{
	return gen_pool_alloc(acpu->media, PAGE_ALIGN(size));
}
EXPORT_SYMBOL_GPL(rtd_acpu_media_alloc);

void rtd_acpu_media_free(struct rtd_acpu *acpu, phys_addr_t phys, size_t size)
{
	gen_pool_free(acpu->media, phys, PAGE_ALIGN(size));
}
EXPORT_SYMBOL_GPL(rtd_acpu_media_free);

int rtd_acpu_alloc(struct rtd_acpu *acpu, size_t size, struct rtd_acpu_buf *buf)
{
	dma_addr_t phys;

	buf->size = ALIGN(size, 64);
	buf->vaddr = gen_pool_dma_zalloc(acpu->audio, buf->size, &phys);
	if (!buf->vaddr)
		return -ENOMEM;
	buf->phys = phys;
	return 0;
}
EXPORT_SYMBOL_GPL(rtd_acpu_alloc);

void rtd_acpu_free(struct rtd_acpu *acpu, struct rtd_acpu_buf *buf)
{
	if (buf->vaddr)
		gen_pool_free(acpu->audio, (unsigned long)buf->vaddr, buf->size);
	buf->vaddr = NULL;
}
EXPORT_SYMBOL_GPL(rtd_acpu_free);

static int acpu_region(struct rtd_acpu *acpu, const char *name,
		       struct resource *res)
{
	struct device_node *np = acpu->dev->of_node, *rmem;
	int idx, ret;

	idx = of_property_match_string(np, "memory-region-names", name);
	if (idx < 0)
		return dev_err_probe(acpu->dev, idx, "no %s region\n", name);
	rmem = of_parse_phandle(np, "memory-region", idx);
	if (!rmem)
		return -ENODEV;
	ret = of_address_to_resource(rmem, 0, res);
	of_node_put(rmem);
	return ret;
}

/* As rtk_rpc_probe(): rings, then interrupts on, then the flag */
static void acpu_start(struct rtd_acpu *acpu)
{
	int i;

	for (i = 0; i < NR_RINGS; i++) {
		ring_init(acpu, POLL_REC(i), POLL_RING(i), RING_SIZE);
		ring_init(acpu, INTR_REC(i), INTR_RING(i), RING_SIZE);
		ring_init(acpu, KERN_REC(i), KERN_RING(i), RING_SIZE);
	}
	writel(readl(acpu->comm + IPC_VO_INT_SYNC) | SYNC_RPC_NOTIFY,
	       acpu->comm + IPC_VO_INT_SYNC);
	acpu_kick(acpu);
	writel(0xffffffff, acpu->comm + IPC_AUDIO_RPC_FLAG);
}

static void acpu_unregister_adev(void *data)
{
	struct rtd_acpu_adev *adev = data;

	auxiliary_device_delete(&adev->adev);
	auxiliary_device_uninit(&adev->adev);
}

static void acpu_release_adev(struct device *dev)
{
	kfree(container_of(dev, struct rtd_acpu_adev, adev.dev));
}

static int acpu_add_adev(struct rtd_acpu *acpu, const char *name,
			 struct rtd_acpu_adev **slot)
{
	struct rtd_acpu_adev *adev;
	int ret;

	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;
	adev->acpu = acpu;
	adev->adev.name = name;
	adev->adev.dev.parent = acpu->dev;
	adev->adev.dev.release = acpu_release_adev;
	ret = auxiliary_device_init(&adev->adev);
	if (ret) {
		kfree(adev);
		return ret;
	}
	ret = auxiliary_device_add(&adev->adev);
	if (ret) {
		auxiliary_device_uninit(&adev->adev);
		return ret;
	}
	*slot = adev;
	return devm_add_action_or_reset(acpu->dev, acpu_unregister_adev, adev);
}

static int acpu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource comm, ring, audio, media, fw;
	struct rtd_acpu *acpu;
	void *audio_virt;
	int irq, ret;

	acpu = devm_kzalloc(dev, sizeof(*acpu), GFP_KERNEL);
	if (!acpu)
		return -ENOMEM;
	acpu->dev = dev;
	INIT_LIST_HEAD(&acpu->remote);
	spin_lock_init(&acpu->sa_lock);
	INIT_WORK(&acpu->work, acpu_work);
	mutex_init(&acpu->call_lock);
	init_completion(&acpu->call_done);
	mutex_init(&acpu->rpc_lock);
	init_completion(&acpu->rpc_done);

	/* SB2 belongs to a syscon as a whole, so do not claim the range */
	acpu->sb2 = devm_ioremap(dev, platform_get_resource(pdev,
				 IORESOURCE_MEM, 0)->start, 0xc);
	if (!acpu->sb2)
		return -ENOMEM;

	ret = acpu_region(acpu, "comm", &comm) ?:
	      acpu_region(acpu, "ring", &ring) ?:
	      acpu_region(acpu, "audio", &audio) ?:
	      acpu_region(acpu, "media", &media) ?:
	      acpu_region(acpu, "firmware", &fw);
	if (ret)
		return ret;
	acpu->comm = devm_ioremap(dev, comm.start, resource_size(&comm));
	acpu->ring = devm_ioremap(dev, ring.start, resource_size(&ring));
	audio_virt = devm_memremap(dev, audio.start, resource_size(&audio),
				   MEMREMAP_WC);
	if (!acpu->comm || !acpu->ring || IS_ERR(audio_virt))
		return -ENOMEM;
	acpu->ring_acpu = rtd_acpu_addr(ring.start);
	acpu->fw_size = resource_size(&fw);
	acpu->fw = devm_ioremap(dev, fw.start, acpu->fw_size);
	if (!acpu->fw)
		return -ENOMEM;
	/* the firmware is linked to run here; its pointers say so */
	if (fw.start != 0x0f900000)
		return dev_err_probe(dev, -EINVAL, "firmware not at 0x0f900000\n");

	acpu->audio = devm_gen_pool_create(dev, 6, -1, "acpu-audio");
	acpu->media = devm_gen_pool_create(dev, PAGE_SHIFT, -1, "acpu-media");
	if (IS_ERR(acpu->audio) || IS_ERR(acpu->media))
		return -ENOMEM;
	ret = gen_pool_add_virt(acpu->audio, (unsigned long)audio_virt,
				audio.start, resource_size(&audio), -1) ?:
	      gen_pool_add(acpu->media, media.start, resource_size(&media), -1);
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, acpu_irq, 0, dev_name(dev), acpu);
	if (ret)
		return ret;

	/*
	 * The firmware enables its RPC interrupts once it has opened the
	 * rings. If it already has, it is running from an earlier start, and
	 * new rings would lose track of whatever it has in flight.
	 */
	if (readl(acpu->sb2 + RPC_INT_EN) & RPC_INT_SA) {
		dev_warn(dev, "audio firmware already running\n");
		acpu_quiet(acpu);
	} else if (be32_to_cpu(readl(acpu->comm)) != 0x16803001) {
		/* u-boot's "go a" writes this magic for the firmware */
		return dev_err_probe(dev, -ENODEV, "audio firmware not started\n");
	} else {
		acpu_start(acpu);
	}

	platform_set_drvdata(pdev, acpu);
	return acpu_add_adev(acpu, RTD_ACPU_PCM_NAME, &acpu->pcm) ?:
	       acpu_add_adev(acpu, RTD_ACPU_VO_NAME, &acpu->vo);
}

static const struct of_device_id acpu_of_match[] = {
	{ .compatible = "realtek,rtd1295-acpu" },
	{ }
};
MODULE_DEVICE_TABLE(of, acpu_of_match);

static struct platform_driver acpu_driver = {
	.probe = acpu_probe,
	.driver = {
		.name = "rtd129x-acpu",
		.of_match_table = acpu_of_match,
		/*
		 * The firmware keeps the memory it got from us; a new probe
		 * would hand it out again.
		 */
		.suppress_bind_attrs = true,
	},
};

static int __init acpu_init(void)
{
	return platform_driver_register(&acpu_driver);
}
module_init(acpu_init);

MODULE_DESCRIPTION("Realtek RTD129x audio CPU RPC");
MODULE_LICENSE("GPL");
