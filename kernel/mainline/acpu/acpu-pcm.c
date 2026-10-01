// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek RTD129x HDMI receiver audio, through the audio CPU
 *
 * The HDMI receiver hands its audio to the audio input (AI) block, which
 * belongs to the audio firmware. Capture is the BSP's sequence
 * (sound/arm/snd-realtek.c, snd_card_capture_prepare_LPCM()): create an AI
 * agent, give it ring buffers, tell it to deliver interleaved little-endian
 * PCM for ALSA, configure the input and run it. The firmware then writes
 * into the LPCM ring and moves its write pointer; a timer here copies whole
 * periods into the ALSA buffer and moves the read pointer.
 *
 * With no audio from the source -- none playing, or a DVI one -- the
 * firmware writes nothing. The stream then carries on with silence, in
 * real time, so that a reader such as Janus sees a quiet input rather than
 * a stalled one.
 *
 * The AI agent's default source is the HDMI receiver; the BSP selects I2S
 * with an extra call that is not made here.
 */
#include <linux/auxiliary_bus.h>
#include <linux/hrtimer.h>
#include <linux/module.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "rtd129x-acpu.h"

/* enum AUDIO_IO_PIN */
#define PIN_BASE_BS_OUT		3
#define PIN_PCM_OUT		5

#define AGENT_AUDIO_IN		8	/* enum AUDIO_MODULE_TYPE */
#define INFO_AI_CONNECT_ALSA	18	/* enum AUDIO_ENUM_PRIVAETINFO */
#define AIO_DESTROY_AI_FLOW	2	/* enum AUDIO_ENUM_AIO_PRIVAETINFO */
#define ALSA_FORMAT_S16LE	1	/* AUDIO_ALSA_FORMAT_16BITS_LE_LPCM */

#define AI_RING_SIZE		SZ_32K	/* per channel, the AI's own output */
#define LPCM_RING_SIZE		SZ_32K	/* what we read */
#define CHANNELS		2

/* RINGBUFFER_HEADER, big-endian, 64 bytes */
struct ring_hdr {
	__be32 magic;
	__be32 begin;
	__be32 size;
	__be32 buffer_id;
	__be32 write;
	__be32 num_read;
	__be32 reserved[2];
	__be32 read[4];
	__be32 file[4];
};

/* What the firmware reads and writes, in one block of the audio heap */
struct ai_shared {
	struct ring_hdr ai[CHANNELS];
	struct ring_hdr lpcm;
};

struct hdmi_audio {
	struct rtd_acpu *acpu;
	struct snd_card *card;
	struct snd_pcm_substream *substream;

	struct rtd_acpu_buf param;	/* call arguments */
	struct rtd_acpu_buf shared;	/* struct ai_shared */
	struct rtd_acpu_buf ai[CHANNELS];
	struct rtd_acpu_buf lpcm;
	u32 agent;
	bool running;			/* the AI flow is set up */
	bool active;			/* between trigger start and stop */

	struct hrtimer timer;
	ktime_t period;
	u32 lpcm_read;			/* our copy of lpcm.read[0], LE */
	snd_pcm_uframes_t hw_ptr;
	ktime_t last;			/* when the last period went up */
	bool silent;			/* making up periods of silence */
};

/* How long the firmware may go quiet before we fill in */
#define SILENCE_AFTER_MS	100

static u32 param_addr(struct hdmi_audio *ha, size_t offset)
{
	return rtd_acpu_addr(ha->param.phys + offset);
}

static __be32 *param_word(struct hdmi_audio *ha, size_t offset)
{
	return ha->param.vaddr + offset;
}

/* Check both the call's HRESULT and, when given, the result's */
static int ha_call(struct hdmi_audio *ha, u32 cmd, size_t arg, size_t res,
		   bool check_res)
{
	u32 ret;
	int err;

	/* the firmware reads it uncached; make sure it has all landed */
	wmb();
	err = rtd_acpu_call(ha->acpu, cmd, param_addr(ha, arg),
			    param_addr(ha, res), &ret);
	if (err)
		return err;
	if (ret != RTD_ACPU_S_OK ||
	    (check_res && be32_to_cpu(*param_word(ha, res)) != RTD_ACPU_S_OK)) {
		dev_err(ha->card->dev, "command %u: %#x/%#x\n", cmd, ret,
			be32_to_cpu(*param_word(ha, res)));
		return -EIO;
	}
	return 0;
}

static int ha_check_ready(struct hdmi_audio *ha)
{
	/* RPC_DEFAULT_INPUT_T: info, retval { result, data }, ret */
	memset(ha->param.vaddr, 0, 64);
	return ha_call(ha, RTD_ACPU_CHECK_READY, 0, 4, false);
}

static int ha_create_agent(struct hdmi_audio *ha)
{
	/* AUDIO_RPC_INSTANCE { instanceID, type }, RPCRES_LONG { result, data } */
	memset(ha->param.vaddr, 0, 64);
	*param_word(ha, 0) = cpu_to_be32(-1);
	*param_word(ha, 4) = cpu_to_be32(AGENT_AUDIO_IN);
	if (ha_call(ha, RTD_ACPU_CREATE_AGENT, 0, 8, true))
		return -EIO;
	ha->agent = be32_to_cpu(*param_word(ha, 12));
	return 0;
}

static void ring_hdr_init(struct ring_hdr *h, phys_addr_t begin, u32 size)
{
	memset(h, 0, sizeof(*h));
	h->begin = cpu_to_be32(begin);
	h->size = cpu_to_be32(size);
	h->write = h->begin;
	h->read[0] = h->begin;
	h->num_read = cpu_to_be32(1);
}

/* INIT_RINGBUF: RPCRES_LONG ret, HRESULT res, AUDIO_RPC_RINGBUFFER_HEADER */
static int ha_init_ring(struct hdmi_audio *ha, u32 pin, struct ring_hdr *hdrs,
			int count)
{
	struct ai_shared *sh = ha->shared.vaddr;
	int i;

	memset(ha->param.vaddr, 0, 64);
	*param_word(ha, 12) = cpu_to_be32(ha->agent);
	*param_word(ha, 16) = cpu_to_be32(pin);
	for (i = 0; i < count; i++)
		*param_word(ha, 20 + 4 * i) = cpu_to_be32(ha->shared.phys +
						((void *)&hdrs[i] - (void *)sh));
	*param_word(ha, 52) = cpu_to_be32(-1);		/* readIdx */
	*param_word(ha, 56) = cpu_to_be32(count);	/* listSize */
	return ha_call(ha, RTD_ACPU_INIT_RINGBUF, 12, 0, true);
}

static int ha_agent_cmd(struct hdmi_audio *ha, u32 cmd)
{
	/* RPC_TOAGENT_T: inst_id, retval { result, data }, res */
	memset(ha->param.vaddr, 0, 64);
	*param_word(ha, 0) = cpu_to_be32(ha->agent);
	return ha_call(ha, cmd, 0, 4, cmd == RTD_ACPU_RUN);
}

static int ha_setup(struct hdmi_audio *ha, unsigned int rate)
{
	struct ai_shared *sh = ha->shared.vaddr;
	int i, ret;

	for (i = 0; i < CHANNELS; i++)
		ring_hdr_init(&sh->ai[i], ha->ai[i].phys, AI_RING_SIZE);
	ring_hdr_init(&sh->lpcm, ha->lpcm.phys, LPCM_RING_SIZE);
	ha->lpcm_read = ha->lpcm.phys;

	ret = ha_init_ring(ha, PIN_PCM_OUT, sh->ai, CHANNELS) ?:
	      ha_init_ring(ha, PIN_BASE_BS_OUT, &sh->lpcm, 1);
	if (ret)
		return ret;

	/* AUDIO_RPC_PRIVATEINFO_PARAMETERS, then the return value at 72 */
	memset(ha->param.vaddr, 0, 160);
	*param_word(ha, 0) = cpu_to_be32(ha->agent);
	*param_word(ha, 4) = cpu_to_be32(INFO_AI_CONNECT_ALSA);
	*param_word(ha, 8) = cpu_to_be32(ALSA_FORMAT_S16LE);
	ret = ha_call(ha, RTD_ACPU_PRIVATEINFO, 0, 72, false);
	if (ret)
		return ret;

	/*
	 * AUDIO_CONFIG_ADC: instanceID; AUDIO_GENERAL_CONFIG { char
	 * interface_en, channel_in, count_down_rec_en; int cyc };
	 * AUDIO_SAMPLE_INFO { sampling_rate, PCM_bitnum }. The BSP does not
	 * look at the result of this one.
	 */
	memset(ha->param.vaddr, 0, 64);
	*param_word(ha, 0) = cpu_to_be32(ha->agent);
	*(u8 *)(ha->param.vaddr + 4) = 1;
	*(u8 *)(ha->param.vaddr + 5) = 3;
	*param_word(ha, 12) = cpu_to_be32(rate);
	*param_word(ha, 16) = cpu_to_be32(24);
	wmb();
	ret = rtd_acpu_call(ha->acpu, RTD_ACPU_ADC0_CONFIG, param_addr(ha, 0),
			    param_addr(ha, 20), NULL);
	if (ret)
		return ret;

	return ha_agent_cmd(ha, RTD_ACPU_PAUSE) ?:
	       ha_agent_cmd(ha, RTD_ACPU_RUN);
}

static void ha_destroy(struct hdmi_audio *ha)
{
	/* AUDIO_RPC_AIO_PRIVATEINFO_PARAMETERS, then the result at 72 */
	memset(ha->param.vaddr, 0, 160);
	*param_word(ha, 0) = cpu_to_be32(ha->agent);
	*param_word(ha, 4) = cpu_to_be32(AIO_DESTROY_AI_FLOW);
	wmb();
	rtd_acpu_call(ha->acpu, RTD_ACPU_AIO_PRIVATEINFO, param_addr(ha, 0),
		      param_addr(ha, 72), NULL);
}

static void ha_free_rings(struct hdmi_audio *ha)
{
	int i;

	for (i = 0; i < CHANNELS; i++)
		rtd_acpu_free(ha->acpu, &ha->ai[i]);
	rtd_acpu_free(ha->acpu, &ha->lpcm);
	rtd_acpu_free(ha->acpu, &ha->shared);
}

static int ha_alloc_rings(struct hdmi_audio *ha)
{
	int i, ret;

	ret = rtd_acpu_alloc(ha->acpu, sizeof(struct ai_shared), &ha->shared) ?:
	      rtd_acpu_alloc(ha->acpu, LPCM_RING_SIZE, &ha->lpcm);
	for (i = 0; !ret && i < CHANNELS; i++)
		ret = rtd_acpu_alloc(ha->acpu, AI_RING_SIZE, &ha->ai[i]);
	if (ret)
		ha_free_rings(ha);
	return ret;
}

static void ha_put_period(struct hdmi_audio *ha, const void *src, u32 src_off)
{
	struct snd_pcm_runtime *rt = ha->substream->runtime;
	u32 period = frames_to_bytes(rt, rt->period_size);
	u32 buffer = frames_to_bytes(rt, rt->buffer_size);
	u32 dst = frames_to_bytes(rt, ha->hw_ptr);
	u32 done = 0;

	while (done < period) {
		u32 n = min(period - done, buffer - dst);

		if (src) {
			n = min(n, LPCM_RING_SIZE - src_off);
			memcpy(rt->dma_area + dst, src + src_off, n);
			src_off = (src_off + n) % LPCM_RING_SIZE;
		} else {
			memset(rt->dma_area + dst, 0, n);
		}
		done += n;
		dst = (dst + n) % buffer;
	}
	ha->hw_ptr = (ha->hw_ptr + rt->period_size) % rt->buffer_size;
}

/* Move whole periods from the LPCM ring, or silence, to the ALSA buffer */
static enum hrtimer_restart ha_timer(struct hrtimer *t)
{
	struct hdmi_audio *ha = container_of(t, struct hdmi_audio, timer);
	struct snd_pcm_runtime *rt = ha->substream->runtime;
	struct ai_shared *sh = ha->shared.vaddr;
	u32 period = frames_to_bytes(rt, rt->period_size);
	ktime_t period_ns = ns_to_ktime(div_u64((u64)rt->period_size *
						NSEC_PER_SEC, rt->rate));
	u32 write = be32_to_cpu(READ_ONCE(sh->lpcm.write));
	u32 avail = (write - ha->lpcm_read + LPCM_RING_SIZE) % LPCM_RING_SIZE;
	ktime_t now = ktime_get();
	bool elapsed = false;

	if (!READ_ONCE(ha->active))
		return HRTIMER_NORESTART;

	if (avail >= period) {
		ha->silent = false;
		while (avail >= period) {
			ha_put_period(ha, ha->lpcm.vaddr,
				      ha->lpcm_read - ha->lpcm.phys);
			ha->lpcm_read = ha->lpcm.phys +
				(ha->lpcm_read - ha->lpcm.phys + period) %
				LPCM_RING_SIZE;
			avail -= period;
			elapsed = true;
		}
		WRITE_ONCE(sh->lpcm.read[0], cpu_to_be32(ha->lpcm_read));
		ha->last = now;
	} else if (ha->silent ||
		   ktime_ms_delta(now, ha->last) > SILENCE_AFTER_MS) {
		if (!ha->silent)
			ha->last = now;
		ha->silent = true;
		while (ktime_compare(ktime_sub(now, ha->last), period_ns) >= 0) {
			ha_put_period(ha, NULL, 0);
			ha->last = ktime_add(ha->last, period_ns);
			elapsed = true;
		}
	}
	if (elapsed)
		snd_pcm_period_elapsed(ha->substream);
	hrtimer_forward_now(t, ha->period);
	return HRTIMER_RESTART;
}

static const struct snd_pcm_hardware ha_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
	.rate_min = 44100,
	.rate_max = 48000,
	.channels_min = CHANNELS,
	.channels_max = CHANNELS,
	.buffer_bytes_max = SZ_64K,
	.period_bytes_min = SZ_1K,
	/* a period has to fit the LPCM ring with room to spare */
	.period_bytes_max = SZ_8K,
	.periods_min = 2,
	.periods_max = 32,
};

static int ha_open(struct snd_pcm_substream *ss)
{
	struct hdmi_audio *ha = snd_pcm_substream_chip(ss);
	int ret;

	ss->runtime->hw = ha_hw;
	ret = ha_check_ready(ha) ?: ha_create_agent(ha);
	if (ret)
		return ret;
	ret = ha_alloc_rings(ha);
	if (ret)
		return ret;
	ha->substream = ss;
	ha->running = false;
	return 0;
}

static int ha_close(struct snd_pcm_substream *ss)
{
	struct hdmi_audio *ha = snd_pcm_substream_chip(ss);

	hrtimer_cancel(&ha->timer);
	ha_destroy(ha);
	ha_free_rings(ha);
	ha->substream = NULL;
	return 0;
}

static int ha_prepare(struct snd_pcm_substream *ss)
{
	struct hdmi_audio *ha = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	int ret;

	ha->hw_ptr = 0;
	ha->period = ns_to_ktime(div_u64((u64)rt->period_size * NSEC_PER_SEC,
					 rt->rate) / 2);
	if (ha->running)
		return 0;
	ret = ha_setup(ha, rt->rate);
	if (ret)
		return ret;
	ha->running = true;
	return 0;
}

static int ha_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct hdmi_audio *ha = snd_pcm_substream_chip(ss);
	struct ai_shared *sh = ha->shared.vaddr;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* start from what arrives now, not from what piled up */
		ha->lpcm_read = be32_to_cpu(READ_ONCE(sh->lpcm.write));
		WRITE_ONCE(sh->lpcm.read[0], cpu_to_be32(ha->lpcm_read));
		ha->last = ktime_get();
		ha->silent = false;
		WRITE_ONCE(ha->active, true);
		hrtimer_start(&ha->timer, ha->period, HRTIMER_MODE_REL_SOFT);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		/* the timer may be what is calling us, through an xrun */
		WRITE_ONCE(ha->active, false);
		hrtimer_try_to_cancel(&ha->timer);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t ha_pointer(struct snd_pcm_substream *ss)
{
	struct hdmi_audio *ha = snd_pcm_substream_chip(ss);

	return ha->hw_ptr;
}

static const struct snd_pcm_ops ha_ops = {
	.open = ha_open,
	.close = ha_close,
	.prepare = ha_prepare,
	.trigger = ha_trigger,
	.pointer = ha_pointer,
};

static void ha_free_param(void *data)
{
	struct hdmi_audio *ha = data;

	rtd_acpu_free(ha->acpu, &ha->param);
}

static int ha_probe(struct auxiliary_device *adev,
		    const struct auxiliary_device_id *id)
{
	struct rtd_acpu_adev *radev = container_of(adev, struct rtd_acpu_adev,
						   adev);
	struct device *dev = &adev->dev;
	struct hdmi_audio *ha;
	struct snd_card *card;
	struct snd_pcm *pcm;
	int ret;

	ha = devm_kzalloc(dev, sizeof(*ha), GFP_KERNEL);
	if (!ha)
		return -ENOMEM;
	ha->acpu = radev->acpu;
	hrtimer_setup(&ha->timer, ha_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
	ret = rtd_acpu_alloc(ha->acpu, SZ_4K, &ha->param);
	if (ret)
		return ret;
	/*
	 * Before the card, so that it is freed after it: a stream still open
	 * at removal calls into the firmware when it closes.
	 */
	ret = devm_add_action_or_reset(dev, ha_free_param, ha);
	if (ret)
		return ret;

	ret = snd_devm_card_new(dev, SNDRV_DEFAULT_IDX1, "hdmirx", THIS_MODULE,
				0, &card);
	if (ret)
		return ret;
	ha->card = card;

	strscpy(card->driver, "rtd129x-hdmirx", sizeof(card->driver));
	strscpy(card->shortname, "HDMI-RX", sizeof(card->shortname));
	strscpy(card->longname, "RTD129x HDMI receiver audio",
		sizeof(card->longname));

	ret = snd_pcm_new(card, "HDMI-RX", 0, 0, 1, &pcm);
	if (ret)
		return ret;
	pcm->private_data = ha;
	strscpy(pcm->name, "HDMI-RX", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &ha_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
				       0, SZ_64K);

	return snd_card_register(card);
}

static const struct auxiliary_device_id ha_ids[] = {
	{ .name = "rtd129x_acpu." RTD_ACPU_PCM_NAME },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, ha_ids);

static struct auxiliary_driver ha_driver = {
	.probe = ha_probe,
	.id_table = ha_ids,
};
module_auxiliary_driver(ha_driver);

MODULE_DESCRIPTION("Realtek RTD129x HDMI receiver audio capture");
MODULE_LICENSE("GPL");
