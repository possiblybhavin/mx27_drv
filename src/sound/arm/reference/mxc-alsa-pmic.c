/*
 * Copyright 2004-2007 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

 /*!
  * @defgroup SOUND_DRV MXC Sound Driver for ALSA
  */

 /*!
  * @file       mxc-alsa-pmic.c
  * @brief      this fle       mxc-alsa-pmic.c
  * @brief      this file implements the mxc sound driver interface for ALSA.
  *             The mxc sound driver supports mono/stereo recording (there are
  *             some limitations due to hardware), mono/stereo playback and
  *             audio mixing.
  *             Recording supports 8000 khz and 16000 khz sample rate.
  *             Playback supports 8000, 11025, 16000, 22050, 24000, 32000,
  *             44100, 48000 and 96000 Hz for mono and stereo.
  *             This file also handles the software mixer and abstraction APIs
  *             that control the volume,balance,mono-adder,input and output
  *             devices for PMIC.
  *             These mixer controls shall be accessible thru alsa as well as
  *             OSS emulation modes
  *
  * @ingroup SOUND_DRV
  */

#include <sound/driver.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/ioctl.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/soundcard.h>

#ifdef CONFIG_PM
#include <linux/pm.h>
#endif				/* CONFIG_PM */

#include <asm/arch/dma.h>
#include <asm/mach-types.h>

#include <ssi/ssi.h>
#include <ssi/registers.h>
#include <dam/dam.h>
#include <asm/arch/pmic_external.h>
#include <asm/arch/pmic_audio.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include "mxc-alsa-pmic.h"
#include "mxc-alsa-common.h"
#include <linux/fs.h>
/*
 * PMIC driver buffer policy.
 * Customize here if the sound is not correct
 */
#define MAX_BUFFER_SIZE  			(32*1024)
#define DMA_BUF_SIZE				(8*1024)
#define MIN_PERIOD_SIZE				64
#define MIN_PERIOD				2
#define MAX_PERIOD				255

#define AUD_MUX_CONF 				0x0031010
#define MASK_2_TS				0xfffffffc
#define MASK_1_TS				0xfffffffd
#define MASK_1_TS_STDAC				0xfffffffe
#define MASK_1_TS_REC				0xfffffffe
#define SOUND_CARD_NAME				"MXC"

/*!
 * These defines enable DMA chaining for playback
 * and capture respectively.
 */
#define MXC_SOUND_PLAYBACK_CHAIN_DMA_EN 1
#define MXC_SOUND_CAPTURE_CHAIN_DMA_EN 1
/*!
  * ID for this card
  */
static char *id = NULL;

#define MXC_ALSA_MAX_PCM_DEV 2
#define MXC_ALSA_MAX_PLAYBACK 2
#define MXC_ALSA_MAX_CAPTURE 1

/*!
  * This structure is the global configuration of the soundcard
  * that are accessed by the mixer as well as by the playback/recording
  * stream. This contains various volume, balance, mono adder settings
  *
  */
typedef struct audio_mixer_control {

	/*!
	 * This variable holds the current active output device(s)
	 */
	int output_device;

	/*!
	 * This variable holds the current active input device.
	 */
	int input_device;

	/* Used only for playback/recording on codec .. Use 1 for playback
	 * and 0 for recording*/
	int direction;

	/*!
	 * This variable holds the current source for active ouput device(s)
	 */
	OUTPUT_SOURCE source_for_output[OP_MAXDEV];

	/*!
	 * This variable says if a given output device is part of an ongoing
	 * playback. This variable will be set and reset by the playback stream
	 * when stream is activated and when stream is closed. This shall also
	 * be set and reset my mixer functions for enabling/disabling output devs
	 */
	int output_active[OP_MAXDEV];

	/*!
	 * This variable holds the current volume for active input device.
	 * This maps to the input gain of recording device
	 */
	int input_volume;

	/*!
	 * This variable holds the current volume for playback devices.
	 */
	//int output_volume[OP_MAXDEV];
	int master_volume_out;

	/*!
	 * This variable holds the balance setting for the mixer out.
	 * The range is 0 to 100. 50 means both L and R equal.
	 * < 50 attenuates left side and > 50 attenualtes right side
	 */
	int mixer_balance;

	/*!
	 * This variable holds the current mono adder config.
	 */
	PMIC_AUDIO_MONO_ADDER_MODE mixer_mono_adder;

	/*!
	 * Semaphore used to control the access to this structure.
	 */
	struct semaphore sem;

	/*!
	 * These variables are set by PCM stream and mixer when the voice codec's / ST dac's outputs are
	 * connected to the analog mixer of PMIC audio chip
	 */
	int codec_out_to_mixer;
	int stdac_out_to_mixer;

	int codec_playback_active;
	int codec_capture_active;
	int stdac_playback_active;

	PMIC_AUDIO_HANDLE stdac_handle;
	PMIC_AUDIO_HANDLE voice_codec_handle;

} audio_mixer_control_t;

/*!
  * This structure stores current state of audio configuration
  * soundcard wrt a specific stream (playback on different DACs, recording on the codec etc).
  * It is used to set/get current values and are NOT accessed by the Mixer. This structure shall
  * be retrieved thru pcm substream pointer and hence the mixer component will have no access
  * to it. There will be as many structures as the number of streams. In our case it's 3. Codec playback
  * STDAC playback and voice codec recording.
  * This structure will be used at the beginning of activating a stream to configure audio chip.
  *
  */
typedef struct pmic_audio_device {

	PMIC_AUDIO_HANDLE handle;
	/*!
	 * This variable holds the sample rate currently being used.
	 */
	int sample_rate;

	/*!
	 * This variable holds the current protocol PMIC is using.
	 * PMIC can use one of three protocols at any given time:
	 * normal, network and I2S.
	 */
	int protocol;

	/*!
	 * This variables tells us whether PMIC runs in
	 * master mode (PMIC generates audio clocks)or slave mode (AP side
	 * generates audio clocks)
	 *
	 * Currently the default mode is master mode because PMIC clocks have
	 * higher precision.
	 */
	int mode;

	/* This variable holds the value representing the
	 * base clock PMIC will use to generate internal
	 * clocks (BCL clock and FrameSync clock)
	 */
	int pll;

	/*!
	 * This variable holds the SSI to which PMIC is currently connected.
	 */
	int ssi;

	/*!
	 * This variable tell us whether bit clock is inverted or not.
	 */
	int bcl_inverted;

	/*!
	 * This variable tell us whether frame clock is inverted or not.
	 */
	int fs_inverted;

	/*!
	 * This variable holds the pll used for PMIC audio operations.
	 */
	int pll_rate;

	/*!
	 * This variable holds the filter that PMIC is applying to
	 * CODEC operations.
	 */
	int codec_filter;

} pmic_audio_device_t;

/*!
  * This structure represents an audio stream in term of
  * channel DMA, HW configuration on PMIC and on AudioMux/SSI
  */
typedef struct audio_stream {
	/*!
	 * identification string
	 */
	char *id;

	/*!
	 * numeric identification
	 */
	int stream_id;

	/*!
	 * SSI ID on the ARM side
	 */
	int ssi;

	/*!
	 * DAM port on the ARM side
	 */
	int dam_port;

	/*!
	 * device identifier for DMA
	 */
	int dma_wchannel;

	/*!
	 * we are using this stream for transfer now
	 */
	int active:1;

	/*!
	 * current transfer period
	 */
	int period;

	/*!
	 * current count of transfered periods
	 */
	int periods;

	/*!
	 * are we recording - flag used to do DMA trans. for sync
	 */
	int tx_spin;

	/*!
	 * Previous offset value for resume
	 */
	unsigned int old_offset;
#if 0
	/*!
	 * Path for this stream
	 */
	device_data_t stream_device;
#endif

	/*!
	 * pmic audio chip stream specific configuration
	 */
	pmic_audio_device_t pmic_audio_device;

	/*!
	 * for locking in DMA operations
	 */
	spinlock_t dma_lock;

	/*!
	 * Alsa substream pointer
	 */
	snd_pcm_substream_t *stream;
} audio_stream_t;

/*!
  * This structure represents the PMIC sound card with its
  * 2 streams (StDac and Codecs) and its shared parameters
  */
typedef struct snd_card_mxc_pmic_audio {
	/*!
	 * ALSA sound card handle
	 */
	snd_card_t *card;

	/*!
	 * ALSA pcm driver type handle
	 */
	snd_pcm_t *pcm[MXC_ALSA_MAX_PCM_DEV];

	/*!
	 * playback & capture streams handle
	 * We can support a maximum of two playback streams (voice-codec
	 * and ST-DAC) and 1 recording stream
	 */
	audio_stream_t s[MXC_ALSA_MAX_CAPTURE + MXC_ALSA_MAX_PLAYBACK];

} mxc_pmic_audio_t;

/*!
 * pmic audio chip parameters for IP/OP and volume controls
 */
audio_mixer_control_t audio_mixer_control;

/*!
  * Global variable that represents the PMIC soundcard
  * with its 2 availables stream devices: stdac and codec
  */
mxc_pmic_audio_t *mxc_audio = NULL;

/*!
  * Supported playback rates array
  */
static unsigned int playback_rates_stereo[] = {
	8000,
	11025,
	12000,
	16000,
	22050,
	24000,
	32000,
	44100,
	48000,
	64000,
	96000,
};

static unsigned int playback_rates_mono[] = {
	8000,
	16000,
};

/*!
  * Supported capture rates array
  */
static unsigned int capture_rates[] = {
	8000,
	16000,
};

/*!
  * this structure represents the sample rates supported
  * by PMIC for playback operations on StDac.
  */
static snd_pcm_hw_constraint_list_t hw_playback_rates_stereo = {
	.count = ARRAY_SIZE(playback_rates_stereo),
	.list = playback_rates_stereo,
	.mask = 0,
};

/*!
  * this structure represents the sample rates supported
  * by PMIC for playback operations on Voice codec.
  */
static snd_pcm_hw_constraint_list_t hw_playback_rates_mono = {
	.count = ARRAY_SIZE(playback_rates_mono),
	.list = playback_rates_mono,
	.mask = 0,
};

/*!
  * this structure represents the sample rates supported
  * by PMIC for capture operations on Codec.
  */
static snd_pcm_hw_constraint_list_t hw_capture_rates = {
	.count = ARRAY_SIZE(capture_rates),
	.list = capture_rates,
	.mask = 0,
};

static struct platform_device *device;
/*!
  * This function configures audio multiplexer to support
  * audio data routing in PMIC master mode.
  *
  * @param       ssi	SSI of the ARM to connect to the DAM.
  */
void configure_dam_pmic_master(int ssi)
{
	int source_port;
	int target_port;

	if (ssi == SSI1) {
		pr_debug("DAM: port 1 -> port 4\n");
		source_port = port_1;
		target_port = port_4;
	} else {
		source_port = port_2;
		if (machine_is_mx27_mdk27v0()) {
			pr_debug("DAM: port 2 -> port 3\n");
			target_port = port_3;
		} else {
			pr_debug("DAM: port 2 -> port 5\n");
			target_port = port_5;
		}
	}

	dam_reset_register(source_port);
	dam_reset_register(target_port);

	dam_select_mode(source_port, normal_mode);
	dam_select_mode(target_port, internal_network_mode);

	dam_set_synchronous(source_port, true);
	dam_set_synchronous(target_port, true);

	dam_select_RxD_source(source_port, target_port);
	dam_select_RxD_source(target_port, source_port);

	dam_select_TxFS_direction(source_port, signal_out);
	dam_select_TxFS_source(source_port, false, target_port);

	dam_select_TxClk_direction(source_port, signal_out);
	dam_select_TxClk_source(source_port, false, target_port);

	dam_select_RxFS_direction(source_port, signal_out);
	dam_select_RxFS_source(source_port, false, target_port);

	dam_select_RxClk_direction(source_port, signal_out);
	dam_select_RxClk_source(source_port, false, target_port);

	dam_set_internal_network_mode_mask(target_port, 0xfc);

	writel(AUD_MUX_CONF, IO_ADDRESS(AUDMUX_BASE_ADDR) + 0x38);
}

/*!
  * This function configures the SSI in order to receive audio
  * from PMIC (recording). Configuration of SSI consists mainly in
  * setting the following:
  *
  * 1) SSI to use (SSI1 or SSI2)
  * 2) SSI mode (normal or network. We use always network mode)
  * 3) SSI STCCR register settings, which control the sample rate (BCL and
  *    FS clocks)
  * 4) Watermarks for SSI FIFOs as well as timeslots to be used.
  * 5) Enable SSI.
  *
  * @param	substream	pointer to the structure of the current stream.
  */
void configure_ssi_rx(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	audio_stream_t *s;
	int ssi;

	chip = snd_pcm_substream_chip(substream);
	s = &chip->s[substream->pstr->stream];
	ssi = s->ssi;

	pr_debug("configure_ssi_rx: SSI %d\n", ssi + 1);

	ssi_enable(ssi, false);
	ssi_synchronous_mode(ssi, true);
	ssi_network_mode(ssi, true);

	if (machine_is_mx27ads() || machine_is_mx27_mdk27v0()) {
		ssi_tx_clock_divide_by_two(ssi, 0);
		ssi_tx_clock_prescaler(ssi, 0);
		ssi_tx_frame_rate(ssi, 2);
	}
	/* OJO */
	ssi_tx_frame_rate(ssi, 1);

	ssi_tx_early_frame_sync(ssi, ssi_frame_sync_one_bit_before);
	ssi_tx_frame_sync_length(ssi, ssi_frame_sync_one_bit);
	ssi_tx_word_length(ssi, ssi_16_bits);

	ssi_rx_early_frame_sync(ssi, ssi_frame_sync_one_bit_before);
	ssi_rx_frame_sync_length(ssi, ssi_frame_sync_one_bit);
	ssi_rx_fifo_enable(ssi, ssi_fifo_0, true);
	ssi_rx_bit0(ssi, true);

	ssi_rx_fifo_full_watermark(ssi, ssi_fifo_0, RX_WATERMARK);

	/* We never use the divider by 2 implemented in SSI */
	ssi_rx_clock_divide_by_two(ssi, 0);

	/* Set prescaler range (a fixed divide-by-eight prescaler
	 * in series with the variable prescaler) to 0 as we don't
	 * need it.
	 */
	ssi_rx_clock_prescaler(ssi, 0);

	/* Currently, only supported sample length is 16 bits */
	ssi_rx_word_length(ssi, ssi_16_bits);

	/* set direction of clocks ("externally" means that clocks come
	 * from PMIC to MCU)
	 */
	ssi_rx_frame_direction(ssi, ssi_tx_rx_externally);
	ssi_rx_clock_direction(ssi, ssi_tx_rx_externally);

	/* Frame Rate Divider Control.
	 * In Normal mode, this ratio determines the word
	 * transfer rate. In Network mode, this ration sets
	 * the number of words per frame.
	 */
	ssi_tx_frame_rate(ssi, 4);
	ssi_rx_frame_rate(ssi, 4);

	ssi_enable(ssi, true);
}

/*!
  * This function configures the SSI in order to
  * send data to PMIC. Configuration of SSI consists
  * mainly in setting the following:
  *
  * 1) SSI to use (SSI1 or SSI2)
  * 2) SSI mode (normal for normal use e.g. playback, network for mixing)
  * 3) SSI STCCR register settings, which control the sample rate (BCL and
  *    FS clocks)
  * 4) Watermarks for SSI FIFOs as well as timeslots to be used.
  * 5) Enable SSI.
  *
  * @param	substream	pointer to the structure of the current stream.
  */
void configure_ssi_tx(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	audio_stream_t *s;
	snd_pcm_runtime_t *runtime;
	int ssi;
	int device, stream_id = -1;
	device = substream->pcm->device;
	if (device == 0)
		stream_id = 0;
	else if (device == 1)
		stream_id = 2;

	chip = snd_pcm_substream_chip(substream);
	s = &chip->s[stream_id];
	runtime = substream->runtime;
	ssi = s->ssi;

	pr_debug("configure_ssi_tx: SSI %d\n", ssi + 1);

	ssi_enable(ssi, false);
	ssi_synchronous_mode(ssi, true);

	if (runtime->channels == 1) {
		if (stream_id == 2) {
			ssi_network_mode(ssi, true);
		} else {
			ssi_network_mode(ssi, false);
		}
	} else {
		ssi_network_mode(ssi, true);
	}

	ssi_tx_early_frame_sync(ssi, ssi_frame_sync_one_bit_before);
	ssi_tx_frame_sync_length(ssi, ssi_frame_sync_one_bit);
	ssi_tx_fifo_enable(ssi, ssi_fifo_0, true);
	ssi_tx_bit0(ssi, true);

	ssi_tx_fifo_empty_watermark(ssi, ssi_fifo_0, TX_WATERMARK);

	/* We never use the divider by 2 implemented in SSI */
	ssi_tx_clock_divide_by_two(ssi, 0);

	ssi_tx_clock_prescaler(ssi, 0);

	/*Currently, only supported sample length is 16 bits */
	ssi_tx_word_length(ssi, ssi_16_bits);

	/* clocks are being provided by PMIC */
	ssi_tx_frame_direction(ssi, ssi_tx_rx_externally);
	ssi_tx_clock_direction(ssi, ssi_tx_rx_externally);

	if (runtime->channels == 1) {
		if (stream_id == 2) {
			ssi_tx_frame_rate(ssi, 4);
		} else {
			ssi_tx_frame_rate(ssi, 1);
		}
	} else {
		ssi_tx_frame_rate(ssi, 2);
	}

	ssi_enable(ssi, true);
}

/*!
  * This function normalizes speed given by the user
  * if speed is not supported, the function will
  * calculate the nearest one.
  *
  * @param       speed   speed requested by the user.
  *
  * @return      The normalized speed.
  */
int adapt_speed(int speed)
{

	/* speeds from 8k to 96k */
	if (speed >= (64000 + 96000) / 2) {
		speed = 96000;
	} else if (speed >= (48000 + 64000) / 2) {
		speed = 64000;
	} else if (speed >= (44100 + 48000) / 2) {
		speed = 48000;
	} else if (speed >= (32000 + 44100) / 2) {
		speed = 44100;
	} else if (speed >= (24000 + 32000) / 2) {
		speed = 32000;
	} else if (speed >= (22050 + 24000) / 2) {
		speed = 24000;
	} else if (speed >= (16000 + 22050) / 2) {
		speed = 22050;
	} else if (speed >= (12000 + 16000) / 2) {
		speed = 16000;
	} else if (speed >= (11025 + 12000) / 2) {
		speed = 12000;
	} else if (speed >= (8000 + 11025) / 2) {
		speed = 11025;
	} else {
		speed = 8000;
	}
	return speed;
}

/*!
  * This function get values to be put in PMIC registers.
  * This values represents the sample rate that PMIC
  * should use for current playback or recording.
  *
  * @param	substream	pointer to the structure of the current stream.
  */
void normalize_speed_for_pmic(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	audio_stream_t *s;
	pmic_audio_device_t *pmic_device;
	snd_pcm_runtime_t *runtime;
	int device, stream_id = -1;
	device = substream->pcm->device;
	if (device == 0) {
		if ((audio_mixer_control.codec_capture_active == 1)
		    && (substream->stream == 1)) {
			stream_id = 1;
		} else
			stream_id = 0;
	} else {
		stream_id = 2;
	}

	chip = snd_pcm_substream_chip(substream);
	s = &chip->s[stream_id];
	pmic_device = &s->pmic_audio_device;
	runtime = substream->runtime;

	/* As the driver allows continuous sample rate, we must adapt the rate */
	runtime->rate = adapt_speed(runtime->rate);

	if (pmic_device->handle == audio_mixer_control.voice_codec_handle) {
		switch (runtime->rate) {
		case 8000:
			pmic_device->sample_rate = VCODEC_RATE_8_KHZ;
			break;
		case 16000:
			pmic_device->sample_rate = VCODEC_RATE_16_KHZ;
			break;
		default:
			pmic_device->sample_rate = VCODEC_RATE_8_KHZ;
			break;
		}

	} else if (pmic_device->handle == audio_mixer_control.stdac_handle) {
		switch (runtime->rate) {
		case 8000:
			pmic_device->sample_rate = STDAC_RATE_8_KHZ;
			break;

		case 11025:
			pmic_device->sample_rate = STDAC_RATE_11_025_KHZ;
			break;

		case 12000:
			pmic_device->sample_rate = STDAC_RATE_12_KHZ;
			break;

		case 16000:
			pmic_device->sample_rate = STDAC_RATE_16_KHZ;
			break;

		case 22050:
			pmic_device->sample_rate = STDAC_RATE_22_050_KHZ;
			break;

		case 24000:
			pmic_device->sample_rate = STDAC_RATE_24_KHZ;
			break;

		case 32000:
			pmic_device->sample_rate = STDAC_RATE_32_KHZ;
			break;

		case 44100:
			pmic_device->sample_rate = STDAC_RATE_44_1_KHZ;
			break;

		case 48000:
			pmic_device->sample_rate = STDAC_RATE_48_KHZ;
			break;

		case 64000:
			pmic_device->sample_rate = STDAC_RATE_64_KHZ;
			break;

		case 96000:
			pmic_device->sample_rate = STDAC_RATE_96_KHZ;
			break;

		default:
			pmic_device->sample_rate = STDAC_RATE_8_KHZ;
		}
	}

}

/*!
  * This function configures number of channels for next audio operation
  * (recording/playback) Number of channels define if sound is stereo
  * or mono.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  */
void set_pmic_channels(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	audio_stream_t *s;
	snd_pcm_runtime_t *runtime;
	int device = -1, stream_id = -1;

	chip = snd_pcm_substream_chip(substream);
	device = substream->pcm->device;

	if (device == 0) {
		if (substream->pstr->stream == 1) {
			stream_id = 1;
		} else {
			stream_id = 0;
		}
	} else {
		stream_id = 2;
	}
	s = &chip->s[stream_id];
	runtime = substream->runtime;

	if (runtime->channels == 2) {
		ssi_tx_mask_time_slot(s->ssi, MASK_2_TS);
		ssi_rx_mask_time_slot(s->ssi, MASK_1_TS_REC);
	} else {
		if (stream_id == 2) {
			ssi_tx_mask_time_slot(s->ssi, MASK_1_TS);
		} else {
			ssi_tx_mask_time_slot(s->ssi, MASK_1_TS_STDAC);
		}
		ssi_rx_mask_time_slot(s->ssi, MASK_1_TS_REC);
	}

}

/*!
  * This function sets the input device in PMIC. It takes an
  * ALSA value and modifies registers using pmic-specific values.
  *
  * @param       handle  Handle to the PMIC device opened
  * @param       val     ALSA value. This value defines the input device that
  *                      PMIC should activate to get audio signal (recording)
  * @param       enable  Whether to enable or diable the input
  */
int set_mixer_input_device(PMIC_AUDIO_HANDLE handle, INPUT_DEVICES dev,
			   bool enable)
{

	if (down_interruptible(&audio_mixer_control.sem))
		return -EINTR;
	if (handle != NULL) {
		if (audio_mixer_control.input_device & SOUND_MASK_PHONEIN) {
			pmic_audio_vcodec_set_mic(handle, MIC1_LEFT,
						  MIC1_RIGHT_MIC_MONO);
			pmic_audio_vcodec_enable_micbias(handle, MIC_BIAS1);
		} else {
			pmic_audio_vcodec_set_mic_on_off(handle,
							 MIC1_LEFT,
							 MIC1_RIGHT_MIC_MONO);
			pmic_audio_vcodec_disable_micbias(handle, MIC_BIAS1);
		}
		if (audio_mixer_control.input_device & SOUND_MASK_MIC) {
			pmic_audio_vcodec_set_mic(handle, NO_MIC, MIC2_AUX);
			pmic_audio_vcodec_enable_micbias(handle, MIC_BIAS2);
		} else {
			pmic_audio_vcodec_set_mic_on_off(handle, NO_MIC,
							 MIC2_AUX);
			pmic_audio_vcodec_disable_micbias(handle, MIC_BIAS2);
		}
		if (audio_mixer_control.input_device & SOUND_MASK_LINE) {
			pmic_audio_vcodec_set_mic(handle, NO_MIC, TXIN_EXT);
		} else {
			pmic_audio_vcodec_set_mic_on_off(handle, NO_MIC,
							 TXIN_EXT);
		}
		up(&audio_mixer_control.sem);
		return 0;

	}
	switch (dev) {
	case IP_HANDSET:
		pr_debug("Input: SOUND_MASK_PHONEIN \n");
		if (handle == NULL) {
			if (enable) {
				if (audio_mixer_control.codec_capture_active) {
					handle =
					    audio_mixer_control.
					    voice_codec_handle;
					pmic_audio_vcodec_set_mic(handle,
								  MIC1_LEFT,
								  MIC1_RIGHT_MIC_MONO);
					pmic_audio_vcodec_enable_micbias(handle,
									 MIC_BIAS1);
				}
				audio_mixer_control.input_device |=
				    SOUND_MASK_PHONEIN;
			} else {
				if (audio_mixer_control.codec_capture_active) {
					handle =
					    audio_mixer_control.
					    voice_codec_handle;
					pmic_audio_vcodec_set_mic_on_off(handle,
									 MIC1_LEFT,
									 MIC1_RIGHT_MIC_MONO);
					pmic_audio_vcodec_disable_micbias
					    (handle, MIC_BIAS1);
				}
				audio_mixer_control.input_device &=
				    ~SOUND_MASK_PHONEIN;
			}
		}
		break;

	case IP_HEADSET:
		if (handle == NULL) {
			if (enable) {
				if (audio_mixer_control.codec_capture_active) {
					handle =
					    audio_mixer_control.
					    voice_codec_handle;
					pmic_audio_vcodec_set_mic(handle,
								  NO_MIC,
								  MIC2_AUX);
					pmic_audio_vcodec_enable_micbias(handle,
									 MIC_BIAS2);
				}
				audio_mixer_control.input_device |=
				    SOUND_MASK_MIC;
			} else {
				if (audio_mixer_control.codec_capture_active) {
					handle =
					    audio_mixer_control.
					    voice_codec_handle;
					pmic_audio_vcodec_set_mic_on_off(handle,
									 NO_MIC,
									 MIC2_AUX);
					pmic_audio_vcodec_disable_micbias
					    (handle, MIC_BIAS2);
				}
				audio_mixer_control.input_device &=
				    ~SOUND_MASK_MIC;
			}
			// Enable Mic with MIC2_AUX
		}
		break;

	case IP_LINEIN:
		if (handle == NULL) {
			if (enable) {
				if (audio_mixer_control.codec_capture_active) {
					handle =
					    audio_mixer_control.
					    voice_codec_handle;
					pmic_audio_vcodec_set_mic(handle,
								  NO_MIC,
								  TXIN_EXT);
				}
				audio_mixer_control.input_device |=
				    SOUND_MASK_LINE;
			} else {
				if (audio_mixer_control.codec_capture_active) {
					handle =
					    audio_mixer_control.
					    voice_codec_handle;
					pmic_audio_vcodec_set_mic_on_off(handle,
									 NO_MIC,
									 TXIN_EXT);
				}
				audio_mixer_control.input_device &=
				    ~SOUND_MASK_LINE;
			}
		}
		break;

	default:
		up(&audio_mixer_control.sem);
		return -1;
		break;
	}
	up(&audio_mixer_control.sem);
	return 0;
}

int get_mixer_input_device()
{
	int val;
	val = audio_mixer_control.input_device;
	return val;
}

/*!
  * This function sets the PMIC input device's gain.
  * Note that the gain is the input volume
  *
  * @param       handle  Handle to the opened PMIC device
  * @param       val     gain to be applied. This value can go
  *                      from 0 (mute) to 100 (max gain)
  */
int set_mixer_input_gain(PMIC_AUDIO_HANDLE handle, int val)
{
	int leftdb, rightdb;
	int left, right;

	left = (val & 0x00ff);
	right = ((val & 0xff00) >> 8);
	if (down_interruptible(&audio_mixer_control.sem))
		return -EINTR;
	leftdb = (left * PMIC_INPUT_VOLUME_MAX) / INPUT_VOLUME_MAX;
	rightdb = (right * PMIC_INPUT_VOLUME_MAX) / INPUT_VOLUME_MAX;
	audio_mixer_control.input_volume = val;
	if (audio_mixer_control.voice_codec_handle == handle) {
		pmic_audio_vcodec_set_record_gain(handle, VOLTAGE_TO_VOLTAGE,
						  leftdb, VOLTAGE_TO_VOLTAGE,
						  rightdb);
	} else if ((handle == NULL)
		   && (audio_mixer_control.codec_capture_active)) {
		pmic_audio_vcodec_set_record_gain(audio_mixer_control.
						  voice_codec_handle,
						  VOLTAGE_TO_VOLTAGE, leftdb,
						  VOLTAGE_TO_VOLTAGE, rightdb);
	}
	up(&audio_mixer_control.sem);
	return 0;
}

int get_mixer_input_gain()
{
	int val;
	val = audio_mixer_control.input_volume;
	return val;
}

/*!
  * This function sets the PMIC output device's volume.
  *
  * @param       handle  Handle to the PMIC device opened
  * @param       volume  ALSA value. This value defines the playback volume
  * @param       dev     which output device gets affected by this volume
  *
  */

int set_mixer_output_volume(PMIC_AUDIO_HANDLE handle, int volume,
			    OUTPUT_DEVICES dev)
{
	int leftdb, rightdb;
	int right, left;

	if (down_interruptible(&audio_mixer_control.sem))
		return -EINTR;
	left = (volume & 0x00ff);
	right = ((volume & 0xff00) >> 8);

	leftdb = (left * PMIC_OUTPUT_VOLUME_MAX) / OUTPUT_VOLUME_MAX;
	rightdb = (right * PMIC_OUTPUT_VOLUME_MAX) / OUTPUT_VOLUME_MAX;
	if (handle == NULL) {
		/* Invoked by mixer */
		audio_mixer_control.master_volume_out = volume;
		if (audio_mixer_control.codec_playback_active)
			pmic_audio_output_set_pgaGain(audio_mixer_control.
						      voice_codec_handle,
						      rightdb);
		if (audio_mixer_control.stdac_playback_active)
			pmic_audio_output_set_pgaGain(audio_mixer_control.
						      stdac_handle, rightdb);

	} else {
		/* change the required volume */
		audio_mixer_control.master_volume_out = volume;
		pmic_audio_output_set_pgaGain(handle, rightdb);
	}
	up(&audio_mixer_control.sem);
	return 0;
}

int get_mixer_output_volume()
{
	int val;
	val = audio_mixer_control.master_volume_out;
	return val;
}

/*!
  * This function sets the PMIC output device's balance.
  *
  * @param       bal     Balance to be applied. This value can go
  *                      from 0 (Left atten) to 100 (Right atten)
  *                      50 is both equal
  */
int set_mixer_output_balance(int bal)
{
	int channel = 0;
	PMIC_AUDIO_OUTPUT_BALANCE_GAIN b_gain;
	PMIC_AUDIO_HANDLE handle;
	if (down_interruptible(&audio_mixer_control.sem))
		return -EINTR;
	// Convert ALSA value to PMIC value i.e. atten and channel value
	if (bal < 0)
		bal = 0;
	if (bal > 100)
		bal = 100;
	if (bal < 50) {
		channel = 1;
	} else {
		bal = 100 - bal;
		channel = 0;
	}

	b_gain = bal / 8;

	audio_mixer_control.mixer_balance = bal;
	if (audio_mixer_control.codec_playback_active) {
		handle = audio_mixer_control.voice_codec_handle;
		// Use codec's handle to set balance
	} else if (audio_mixer_control.stdac_playback_active) {
		handle = audio_mixer_control.stdac_handle;
		// Use STDac's handle to set balance
	} else {
		up(&audio_mixer_control.sem);
		return 0;
	}
	if (channel == 0)
		pmic_audio_output_set_balance(handle, BAL_GAIN_0DB, b_gain);
	else
		pmic_audio_output_set_balance(handle, b_gain, BAL_GAIN_0DB);
	up(&audio_mixer_control.sem);
	return 0;
}

int get_mixer_output_balance()
{
	int val;
	val = audio_mixer_control.mixer_balance;
	return val;
}

/*!
  * This function sets the PMIC output device's mono adder config.
  *
  * @param       mode    Mono adder mode to be set
  */
int set_mixer_output_mono_adder(PMIC_AUDIO_MONO_ADDER_MODE mode)
{
	PMIC_AUDIO_HANDLE handle;
	if (down_interruptible(&audio_mixer_control.sem))
		return -EINTR;
	audio_mixer_control.mixer_mono_adder = mode;
	if (audio_mixer_control.codec_playback_active) {
		handle = audio_mixer_control.voice_codec_handle;
		// Use codec's handle to set balance
		pmic_audio_output_enable_mono_adder(audio_mixer_control.
						    voice_codec_handle, mode);
	} else if (audio_mixer_control.stdac_playback_active) {
		handle = audio_mixer_control.stdac_handle;
		pmic_audio_output_enable_mono_adder(audio_mixer_control.
						    stdac_handle, mode);
		// Use STDac's handle to set balance
	}
	up(&audio_mixer_control.sem);
	return 0;
}

int get_mixer_output_mono_adder()
{
	int val;
	val = audio_mixer_control.mixer_mono_adder;
	return val;
}

/*!
  * This function sets the output device(s) in PMIC. It takes an
  * ALSA value and modifies registers using PMIC-specific values.
  *
  * @param       handle  handle to the device already opened
  * @param       src     Source connected to o/p device
  * @param       dev     Output device to be enabled
  * @param       enable  Enable or disable the device
  *
  */
int set_mixer_output_device(PMIC_AUDIO_HANDLE handle, OUTPUT_SOURCE src,
			    OUTPUT_DEVICES dev, bool enable)
{
	PMIC_AUDIO_OUTPUT_PORT port;
	if (down_interruptible(&audio_mixer_control.sem))
		return -EINTR;
	if (!((src == CODEC_DIR_OUT) || (src == MIXER_OUT))) {
		up(&audio_mixer_control.sem);
		return -1;
	}
	if (handle != (PMIC_AUDIO_HANDLE) NULL) {
		/* Invoked by playback stream */
		if (audio_mixer_control.output_device & SOUND_MASK_PHONEOUT) {
			audio_mixer_control.output_active[OP_EARPIECE] = 1;
			pmic_audio_output_set_port(handle, MONO_SPEAKER);
		}
		if (audio_mixer_control.output_device & SOUND_MASK_SPEAKER) {
			audio_mixer_control.output_active[OP_HANDSFREE] = 1;
			pmic_audio_output_set_port(handle, MONO_LOUDSPEAKER);
		}
		if (audio_mixer_control.output_device & SOUND_MASK_VOLUME) {
			audio_mixer_control.output_active[OP_HEADSET] = 1;
			pmic_audio_output_set_port(handle,
						   STEREO_HEADSET_LEFT |
						   STEREO_HEADSET_RIGHT);
		}
		if (audio_mixer_control.output_device & SOUND_MASK_PCM) {
			audio_mixer_control.output_active[OP_LINEOUT] = 1;
			pmic_audio_output_set_port(handle,
						   STEREO_OUT_LEFT |
						   STEREO_OUT_RIGHT);
		}
	} else {
		switch (dev) {
		case OP_EARPIECE:
			if (enable) {
				audio_mixer_control.output_device |=
				    SOUND_MASK_PHONEOUT;
				audio_mixer_control.source_for_output[dev] =
				    src;
			} else {
				audio_mixer_control.output_device &=
				    ~SOUND_MASK_PHONEOUT;
			}
			port = MONO_SPEAKER;
			break;
		case OP_HANDSFREE:
			if (enable) {
				audio_mixer_control.output_device |=
				    SOUND_MASK_SPEAKER;
				audio_mixer_control.source_for_output[dev] =
				    src;
			} else {
				audio_mixer_control.output_device &=
				    ~SOUND_MASK_SPEAKER;
			}
			port = MONO_LOUDSPEAKER;
			break;
		case OP_HEADSET:
			if (enable) {
				audio_mixer_control.output_device |=
				    SOUND_MASK_VOLUME;
				audio_mixer_control.source_for_output[dev] =
				    src;
			} else {
				audio_mixer_control.output_device &=
				    ~SOUND_MASK_VOLUME;
			}
			port = STEREO_HEADSET_LEFT | STEREO_HEADSET_RIGHT;
			break;
		case OP_LINEOUT:
			if (enable) {
				audio_mixer_control.output_device |=
				    SOUND_MASK_PCM;
				audio_mixer_control.source_for_output[dev] =
				    src;
			} else {
				audio_mixer_control.output_device &=
				    ~SOUND_MASK_PCM;
			}
			port = STEREO_OUT_LEFT | STEREO_OUT_RIGHT;
			break;
		default:
			up(&audio_mixer_control.sem);
			return -1;
			break;
		}
		/* Invoked by mixer .. little tricky to handle over here */
		if (audio_mixer_control.codec_playback_active) {
			if (enable) {
				audio_mixer_control.output_active[dev] = 1;
				pmic_audio_output_set_port(audio_mixer_control.
							   voice_codec_handle,
							   port);
			} else {
				audio_mixer_control.output_active[dev] = 0;
				pmic_audio_output_clear_port
				    (audio_mixer_control.voice_codec_handle,
				     port);
			}
		}
		if (audio_mixer_control.stdac_playback_active) {
			if (enable) {
				audio_mixer_control.output_active[dev] = 1;
				pmic_audio_output_set_port(audio_mixer_control.
							   stdac_handle, port);
			} else {
				audio_mixer_control.output_active[dev] = 0;
				pmic_audio_output_clear_port
				    (audio_mixer_control.stdac_handle, port);
			}
		}

	}
	up(&audio_mixer_control.sem);
	return 0;
	// Set O/P device with handle and port

}

int get_mixer_output_device()
{
	int val;
	val = audio_mixer_control.output_device;
	return val;
}

/*!
  * This function configures the CODEC for playback/recording.
  *
  * main configured elements are:
  *	- audio path on PMIC
  *	- external clock to generate BC and FS clocks
  *	- PMIC mode (master or slave)
  *	- protocol
  *	- sample rate
  *
  * @param	substream	pointer to the structure of the current stream.
  * @param	stream_id	index into the audio_stream array.
  */
void configure_codec(snd_pcm_substream_t * substream, int stream_id)
{
	mxc_pmic_audio_t *chip;
	audio_stream_t *s;
	pmic_audio_device_t *pmic;
	PMIC_AUDIO_HANDLE handle;
	int ssi_bus;

	chip = snd_pcm_substream_chip(substream);
	s = &chip->s[stream_id];
	pmic = &s->pmic_audio_device;
	handle = audio_mixer_control.voice_codec_handle;

	if (machine_is_mx27_mdk27v0()) {
		ssi_bus = AUDIO_DATA_BUS_1;
	} else {
		ssi_bus =
		    (pmic->ssi == SSI1) ? AUDIO_DATA_BUS_1 : AUDIO_DATA_BUS_2;
	}

	pmic_audio_vcodec_set_rxtx_timeslot(handle, USE_TS0);
	pmic_audio_vcodec_enable_mixer(handle, USE_TS1, VCODEC_MIX_IN_0DB,
				       VCODEC_MIX_OUT_0DB);
	pmic_audio_set_protocol(handle, ssi_bus, pmic->protocol, pmic->mode,
				USE_4_TIMESLOTS);

	msleep(20);
	pmic_audio_vcodec_set_clock(handle, pmic->pll, pmic->pll_rate,
				    pmic->sample_rate, NO_INVERT);
	msleep(20);
	pmic_audio_vcodec_set_config(handle, VCODEC_MASTER_CLOCK_OUTPUTS);
	pmic_audio_digital_filter_reset(handle);
	pmic_audio_output_enable_phantom_ground(handle);
	msleep(15);
	if (stream_id == 2) {
		pmic_audio_output_enable_mixer(handle);
		set_mixer_output_device(handle, MIXER_OUT, OP_NODEV, 1);
		set_mixer_output_volume(handle,
					audio_mixer_control.master_volume_out,
					OP_HEADSET);
	} else {
		set_mixer_input_device(handle, IP_NODEV, 1);
		set_mixer_input_gain(handle, audio_mixer_control.input_volume);
	}
	pmic_audio_enable(handle);
}

/*!
  * This function configures the STEREODAC for playback/recording.
  *
  * main configured elements are:
  *      - audio path on PMIC
  *      - external clock to generate BC and FS clocks
  *      - PMIC mode (master or slave)
  *      - protocol
  *      - sample rate
  *
  * @param	substream	pointer to the structure of the current stream.
  */
void configure_stereodac(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	int stream_id;
	audio_stream_t *s;
	pmic_audio_device_t *pmic;
	int ssi_bus;
	PMIC_AUDIO_HANDLE handle;
	snd_pcm_runtime_t *runtime;

	chip = snd_pcm_substream_chip(substream);
	stream_id = substream->pstr->stream;
	s = &chip->s[stream_id];
	pmic = &s->pmic_audio_device;
	handle = pmic->handle;
	runtime = substream->runtime;

	if (runtime->channels == 1) {
		audio_mixer_control.mixer_mono_adder = MONO_ADD_LEFT_RIGHT;
	} else {
		audio_mixer_control.mixer_mono_adder = MONO_ADDER_OFF;
	}

	if (machine_is_mx27_mdk27v0()) {
		ssi_bus = AUDIO_DATA_BUS_1;
	} else {
		ssi_bus =
		    (pmic->ssi == SSI1) ? AUDIO_DATA_BUS_1 : AUDIO_DATA_BUS_2;
	}
	pmic_audio_stdac_set_rxtx_timeslot(handle, USE_TS0_TS1);
	pmic_audio_stdac_enable_mixer(handle, USE_TS2_TS3, STDAC_NO_MIX,
				      STDAC_MIX_OUT_0DB);
	pmic_audio_set_protocol(handle, ssi_bus, pmic->protocol, pmic->mode,
				USE_2_TIMESLOTS);
	pmic_audio_stdac_set_clock(handle, pmic->pll, pmic->pll_rate,
				   pmic->sample_rate, NO_INVERT);

	pmic_audio_stdac_set_config(handle, STDAC_MASTER_CLOCK_OUTPUTS);
	pmic_audio_output_enable_mixer(handle);
	audio_mixer_control.stdac_out_to_mixer = 1;
	pmic_audio_digital_filter_reset(handle);
	msleep(10);
	pmic_audio_output_enable_phantom_ground(handle);
	set_mixer_output_volume(handle, audio_mixer_control.master_volume_out,
				OP_HEADSET);
	pmic_audio_output_enable_mono_adder(handle,
					    audio_mixer_control.
					    mixer_mono_adder);
	set_mixer_output_device(handle, MIXER_OUT, OP_NODEV, 1);
	pmic_audio_enable(handle);
}

/*!
  * This function disables CODEC's amplifiers, volume and clock.
  * @param  handle  Handle of voice codec
  */

void disable_codec(PMIC_AUDIO_HANDLE handle)
{
	pmic_audio_disable(handle);
	pmic_audio_vcodec_clear_config(handle, VCODEC_MASTER_CLOCK_OUTPUTS);
}

/*!
  * This function disables STEREODAC's amplifiers, volume and clock.
  * @param  handle  Handle of STdac
  * @param
  */

void disable_stereodac(void)
{

	audio_mixer_control.stdac_out_to_mixer = 0;
}

/*!
  * This function configures PMIC for recording.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  * @return		0 on success, -1 otherwise.
  */
int configure_pmic_recording(snd_pcm_substream_t * substream)
{

	configure_codec(substream, 1);
	return 0;
}

/*!
  * This function configures PMIC for playing back.
  *
  * @param	substream	pointer to the structure of the current stream.
  * @param	stream_id	Index into the audio_stream array .
  *
  * @return              0 on success, -1 otherwise.
  */

int configure_pmic_playback(snd_pcm_substream_t * substream, int stream_id)
{
	if (stream_id == 0) {
		configure_stereodac(substream);
	} else if (stream_id == 2) {
		configure_codec(substream, stream_id);
	}
	return 0;
}

/*!
  * This function shutsdown the PMIC soundcard.
  * Nothing to be done here
  *
  * @param	mxc_audio	pointer to the sound card structure.
  *
  * @return
  */
/*
static void mxc_pmic_audio_shutdown(mxc_pmic_audio_t * mxc_audio)
{

}
*/

/*!
  * This function configures the DMA channel used to transfer
  * audio from MCU to PMIC
  *
  * @param	substream	pointer to the structure of the current stream.
  * @param       callback        pointer to function that will be
  *                              called when a SDMA TX transfer finishes.
  *
  * @return              0 on success, -1 otherwise.
  */
static int
configure_write_channel(audio_stream_t * s, mxc_dma_callback_t callback,
			int stream_id)
{
	int ret = -1;
	int channel = -1;

	if (stream_id == 0) {
		channel =
		    mxc_dma_request(MXC_DMA_SSI2_16BIT_TX0, "ALSA TX DMA");
	} else if (stream_id == 2) {
		channel =
		    mxc_dma_request(MXC_DMA_SSI1_16BIT_TX0, "ALSA TX DMA");
	}
	if (channel < 0) {
		pr_debug("error requesting a write dma channel\n");
		return -1;
	}

	ret =
	    mxc_dma_callback_set(channel, (mxc_dma_callback_t) callback,
				 (void *)s);
	if (ret != 0) {
		mxc_dma_free(channel);
		return -1;
	}
	s->dma_wchannel = channel;

	return 0;
}

/*!
  * This function configures the DMA channel used to transfer
  * audio from PMIC to MCU
  *
  * @param	substream	pointer to the structure of the current stream.
  * @param       callback        pointer to function that will be
  *                              called when a SDMA RX transfer finishes.
  *
  * @return              0 on success, -1 otherwise.
  */
static int configure_read_channel(audio_stream_t * s,
				  mxc_dma_callback_t callback)
{
	int ret = -1;
	int channel = -1;

	if (machine_is_mx27_mdk27v0()) {
		channel =
		    mxc_dma_request(MXC_DMA_SSI2_16BIT_RX0, "ALSA RX DMA");
	} else {
		channel =
		    mxc_dma_request(MXC_DMA_SSI1_16BIT_RX0, "ALSA RX DMA");
	}
	if (channel < 0) {
		pr_debug("error requesting a read dma channel\n");
		return -1;
	}

	ret =
	    mxc_dma_callback_set(channel, (mxc_dma_callback_t) callback,
				 (void *)s);
	if (ret != 0) {
		mxc_dma_free(channel);
		return -1;
	}
	s->dma_wchannel = channel;

	return 0;
}

/*!
  * This function frees the stream structure
  *
  * @param	s	pointer to the structure of the current stream.
  */
static void audio_dma_free(audio_stream_t * s)
{
	/*
	 * There is nothing to be done here since the dma channel has been
	 * freed either in the callback or in the stop method
	 */

}

/*!
  * This function gets the dma pointer position during record.
  * Our DMA implementation does not allow to retrieve this position
  * when a transfert is active, so, it answers the middle of
  * the current period beeing transfered
  *
  * @param	s	pointer to the structure of the current stream.
  *
  */
static u_int audio_get_capture_dma_pos(audio_stream_t * s)
{
	snd_pcm_substream_t *substream;
	snd_pcm_runtime_t *runtime;
	unsigned int offset;

	substream = s->stream;
	runtime = substream->runtime;
	offset = 0;

	/* tx_spin value is used here to check if a transfert is active */
	if (s->tx_spin) {
		offset = (runtime->period_size * (s->periods)) + 0;
		if (offset >= runtime->buffer_size)
			offset = 0;
		pr_debug("MXC: audio_get_dma_pos offset  %d\n", offset);
	} else {
		offset = (runtime->period_size * (s->periods));
		if (offset >= runtime->buffer_size)
			offset = 0;
		pr_debug("MXC: audio_get_dma_pos BIS offset  %d\n", offset);
	}

	return offset;
}

/*!
  * This function gets the dma pointer position during playback.
  * Our DMA implementation does not allow to retrieve this position
  * when a transfert is active, so, it answers the middle of
  * the current period beeing transfered
  *
  * @param	s	pointer to the structure of the current stream.
  *
  */
static u_int audio_get_playback_dma_pos(audio_stream_t * s)
{
	snd_pcm_substream_t *substream;
	snd_pcm_runtime_t *runtime;
	unsigned int offset;

	substream = s->stream;
	runtime = substream->runtime;
	offset = 0;

	/* tx_spin value is used here to check if a transfert is active */
	if (s->tx_spin) {
		offset = (runtime->period_size * (s->periods)) + 0;
		if (offset >= runtime->buffer_size)
			offset = 0;
		pr_debug("MXC: audio_get_dma_pos offset  %d\n", offset);
	} else {
		offset = (runtime->period_size * (s->periods));
		if (offset >= runtime->buffer_size)
			offset = 0;
		pr_debug("MXC: audio_get_dma_pos BIS offset  %d\n", offset);
	}

	return offset;
}

/*!
  * This function stops the current dma transfert for playback
  * and clears the dma pointers.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  */
static void audio_playback_stop_dma(audio_stream_t * s)
{
	unsigned long flags;
	snd_pcm_substream_t *substream;
	snd_pcm_runtime_t *runtime;
	unsigned int dma_size;
	unsigned int offset;

	substream = s->stream;
	runtime = substream->runtime;
	dma_size = frames_to_bytes(runtime, runtime->period_size);
	offset = dma_size * s->periods;

	spin_lock_irqsave(&s->dma_lock, flags);

	pr_debug("MXC : audio_stop_dma active = 0\n");
	s->active = 0;
	s->period = 0;
	s->periods = 0;

	/* this stops the dma channel and clears the buffer ptrs */
	mxc_dma_disable(s->dma_wchannel);
	dma_unmap_single(NULL, runtime->dma_addr + offset, dma_size,
			 DMA_TO_DEVICE);

	spin_unlock_irqrestore(&s->dma_lock, flags);

}

/*!
  * This function stops the current dma transfert for capture
  * and clears the dma pointers.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  */
static void audio_capture_stop_dma(audio_stream_t * s)
{
	unsigned long flags;
	snd_pcm_substream_t *substream;
	snd_pcm_runtime_t *runtime;
	unsigned int dma_size;
	unsigned int offset;

	substream = s->stream;
	runtime = substream->runtime;
	dma_size = frames_to_bytes(runtime, runtime->period_size);
	offset = dma_size * s->periods;

	spin_lock_irqsave(&s->dma_lock, flags);

	pr_debug("MXC : audio_stop_dma active = 0\n");
	s->active = 0;
	s->period = 0;
	s->periods = 0;

	/* this stops the dma channel and clears the buffer ptrs */
	mxc_dma_disable(s->dma_wchannel);
	dma_unmap_single(NULL, runtime->dma_addr + offset, dma_size,
			 DMA_FROM_DEVICE);

	spin_unlock_irqrestore(&s->dma_lock, flags);

}

/*!
  * This function is called whenever a new audio block needs to be
  * transferred to PMIC. The function receives the address and the size
  * of the new block and start a new DMA transfer.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  */
static void audio_playback_dma(audio_stream_t * s)
{
	snd_pcm_substream_t *substream;
	snd_pcm_runtime_t *runtime;
	unsigned int dma_size;
	unsigned int offset;
	int ret = 0;
	mxc_dma_requestbuf_t dma_request;
	int device;
	int stream_id;

	substream = s->stream;
	runtime = substream->runtime;
	device = substream->pcm->device;
	if (device == 0) {
		stream_id = 0;
	} else {
		stream_id = 2;
	}

	pr_debug("\nDMA direction %d\(0 is playback 1 is capture)\n",
		 s->stream_id);

	memset(&dma_request, 0, sizeof(mxc_dma_requestbuf_t));

	if (s->active) {
		if (ssi_get_status(s->ssi) & ssi_transmitter_underrun_0) {
			ssi_enable(s->ssi, false);
			ssi_transmit_enable(s->ssi, false);
			ssi_enable(s->ssi, true);
		}
		dma_size = frames_to_bytes(runtime, runtime->period_size);
		pr_debug("s->period (%x) runtime->periods (%d)\n",
			 s->period, runtime->periods);
		pr_debug("runtime->period_size (%d) dma_size (%d)\n",
			 (unsigned int)runtime->period_size,
			 runtime->dma_bytes);

		offset = dma_size * s->period;
		snd_assert(dma_size <= DMA_BUF_SIZE,);

		dma_request.src_addr = (dma_addr_t) (dma_map_single(NULL,
								    runtime->
								    dma_area +
								    offset,
								    dma_size,
								    DMA_TO_DEVICE));
		if (stream_id == 0) {
			dma_request.dst_addr =
			    (dma_addr_t) (SSI2_BASE_ADDR + MXC_SSI2STX0);
		} else if (stream_id == 2) {
			dma_request.dst_addr =
			    (dma_addr_t) (SSI1_BASE_ADDR + MXC_SSI1STX0);
		}
		dma_request.num_of_bytes = dma_size;

		pr_debug("MXC: Start DMA offset (%d) size (%d)\n", offset,
			 runtime->dma_bytes);

		mxc_dma_config(s->dma_wchannel, &dma_request, 1,
			       MXC_DMA_MODE_WRITE);
		ret = mxc_dma_enable(s->dma_wchannel);
		ssi_transmit_enable(s->ssi, true);
		s->tx_spin = 1;	/* FGA little trick to retrieve DMA pos */

		if (ret) {
			pr_debug("audio_process_dma: cannot queue DMA buffer\
								(%i)\n", ret);
			return;
		}
		s->period++;
		s->period %= runtime->periods;

#ifdef MXC_SOUND_PLAYBACK_CHAIN_DMA_EN
		if ((s->period > s->periods) && ((s->period - s->periods) > 1)) {
			pr_debug
			    ("audio playback chain dma: already double buffered\n");
			return;
		}

		if ((s->period < s->periods)
		    && ((s->period + runtime->periods - s->periods) > 1)) {
			pr_debug
			    ("audio playback chain dma: already double buffered\n");
			return;
		}

		if (s->period == s->periods) {
			pr_debug
			    ("audio playback chain dma: s->period == s->periods\n");
			return;
		}

		if (snd_pcm_playback_hw_avail(runtime) <
		    2 * runtime->period_size) {
			pr_debug
			    ("audio playback chain dma: available data is not enough\n");
			return;
		}

		pr_debug
		    ("audio playback chain dma:to set up the 2nd dma buffer\n");
		offset = dma_size * s->period;
		dma_request.src_addr = (dma_addr_t) (dma_map_single(NULL,
								    runtime->
								    dma_area +
								    offset,
								    dma_size,
								    DMA_TO_DEVICE));
		mxc_dma_config(s->dma_wchannel, &dma_request, 1,
			       MXC_DMA_MODE_WRITE);
		ret = mxc_dma_enable(s->dma_wchannel);

		s->period++;
		s->period %= runtime->periods;
#endif				/* MXC_SOUND_PLAYBACK_CHAIN_DMA_EN */
	}
}

/*!
  * This function is called whenever a new audio block needs to be
  * transferred from PMIC. The function receives the address and the size
  * of the block that will store the audio samples and start a new DMA transfer.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  */
static void audio_capture_dma(audio_stream_t * s)
{
	snd_pcm_substream_t *substream;
	snd_pcm_runtime_t *runtime;
	unsigned int dma_size;
	unsigned int offset;
	int ret = 0;
	mxc_dma_requestbuf_t dma_request;

	substream = s->stream;
	runtime = substream->runtime;

	pr_debug("\nDMA direction %d\
		(0 is playback 1 is capture)\n", s->stream_id);

	memset(&dma_request, 0, sizeof(mxc_dma_requestbuf_t));

	if (s->active) {
		dma_size = frames_to_bytes(runtime, runtime->period_size);
		pr_debug("s->period (%x) runtime->periods (%d)\n",
			 s->period, runtime->periods);
		pr_debug("runtime->period_size (%d) dma_size (%d)\n",
			 (unsigned int)runtime->period_size,
			 runtime->dma_bytes);

		offset = dma_size * s->period;
		snd_assert(dma_size <= DMA_BUF_SIZE,);

		dma_request.dst_addr = (dma_addr_t) (dma_map_single(NULL,
								    runtime->
								    dma_area +
								    offset,
								    dma_size,
								    DMA_FROM_DEVICE));
		if (machine_is_mx27_mdk27v0()) {
			dma_request.src_addr =
			    (dma_addr_t) (SSI2_BASE_ADDR + MXC_SSI2SRX0);
		} else {
			dma_request.src_addr =
			    (dma_addr_t) (SSI1_BASE_ADDR + MXC_SSI1SRX0);
		}
		dma_request.num_of_bytes = dma_size;

		pr_debug("MXC: Start DMA offset (%d) size (%d)\n", offset,
			 runtime->dma_bytes);

		mxc_dma_config(s->dma_wchannel, &dma_request, 1,
			       MXC_DMA_MODE_READ);
		ret = mxc_dma_enable(s->dma_wchannel);

		s->tx_spin = 1;	/* FGA little trick to retrieve DMA pos */

		if (ret) {
			pr_debug("audio_process_dma: cannot queue DMA buffer\
								(%i)\n", ret);
			return;
		}
		s->period++;
		s->period %= runtime->periods;

#ifdef MXC_SOUND_CAPTURE_CHAIN_DMA_EN
		if ((s->period > s->periods) && ((s->period - s->periods) > 1)) {
			pr_debug
			    ("audio capture chain dma: already double buffered\n");
			return;
		}

		if ((s->period < s->periods)
		    && ((s->period + runtime->periods - s->periods) > 1)) {
			pr_debug
			    ("audio capture chain dma: already double buffered\n");
			return;
		}

		if (s->period == s->periods) {
			pr_debug
			    ("audio capture chain dma: s->period == s->periods\n");
			return;
		}

		if (snd_pcm_capture_hw_avail(runtime) <
		    2 * runtime->period_size) {
			pr_debug
			    ("audio capture chain dma: available data is not enough\n");
			return;
		}

		pr_debug
		    ("audio capture chain dma:to set up the 2nd dma buffer\n");
		offset = dma_size * s->period;
		dma_request.dst_addr = (dma_addr_t) (dma_map_single(NULL,
								    runtime->
								    dma_area +
								    offset,
								    dma_size,
								    DMA_FROM_DEVICE));
		mxc_dma_config(s->dma_wchannel, &dma_request, 1,
			       MXC_DMA_MODE_READ);
		ret = mxc_dma_enable(s->dma_wchannel);

		s->period++;
		s->period %= runtime->periods;
#endif				/* MXC_SOUND_CAPTURE_CHAIN_DMA_EN */
	}
}

/*!
  * This is a callback which will be called
  * when a TX transfer finishes. The call occurs
  * in interrupt context.
  *
  * @param	dat	pointer to the structure of the current stream.
  *
  */
static void audio_playback_dma_callback(void *data, int error,
					unsigned int count)
{
	audio_stream_t *s;
	snd_pcm_substream_t *substream;
	snd_pcm_runtime_t *runtime;
	unsigned int dma_size;
	unsigned int previous_period;
	unsigned int offset;

	s = data;
	substream = s->stream;
	runtime = substream->runtime;
	previous_period = s->periods;
	dma_size = frames_to_bytes(runtime, runtime->period_size);
	offset = dma_size * previous_period;

	s->tx_spin = 0;
	s->periods++;
	s->periods %= runtime->periods;

	/*
	 * Give back to the CPU the access to the non cached memory
	 */
	dma_unmap_single(NULL, runtime->dma_addr + offset, dma_size,
			 DMA_TO_DEVICE);

	/*
	 * If we are getting a callback for an active stream then we inform
	 * the PCM middle layer we've finished a period
	 */
	if (s->active)
		snd_pcm_period_elapsed(s->stream);
	spin_lock(&s->dma_lock);

	/*
	 * Trig next DMA transfer
	 */
	audio_playback_dma(s);

	spin_unlock(&s->dma_lock);

}

/*!
  * This is a callback which will be called
  * when a RX transfer finishes. The call occurs
  * in interrupt context.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  */
static void audio_capture_dma_callback(void *data, int error,
				       unsigned int count)
{
	audio_stream_t *s;
	snd_pcm_substream_t *substream;
	snd_pcm_runtime_t *runtime;
	unsigned int dma_size;
	unsigned int previous_period;
	unsigned int offset;

	s = data;
	substream = s->stream;
	runtime = substream->runtime;
	previous_period = s->periods;
	dma_size = frames_to_bytes(runtime, runtime->period_size);
	offset = dma_size * previous_period;

	s->tx_spin = 0;
	s->periods++;
	s->periods %= runtime->periods;

	/*
	 * Give back to the CPU the access to the non cached memory
	 */
	dma_unmap_single(NULL, runtime->dma_addr + offset, dma_size,
			 DMA_FROM_DEVICE);

	/*
	 * If we are getting a callback for an active stream then we inform
	 * the PCM middle layer we've finished a period
	 */
	if (s->active)
		snd_pcm_period_elapsed(s->stream);

	spin_lock(&s->dma_lock);

	/*
	 * Trig next DMA transfer
	 */
	audio_capture_dma(s);

	spin_unlock(&s->dma_lock);

}

/*!
  * This function is a dispatcher of command to be executed
  * by the driver for playback.
  *
  * @param	substream	pointer to the structure of the current stream.
  * @param	cmd		command to be executed
  *
  * @return              0 on success, -1 otherwise.
  */
static int
snd_mxc_audio_playback_trigger(snd_pcm_substream_t * substream, int cmd)
{
	mxc_pmic_audio_t *chip;
	int stream_id;
	audio_stream_t *s;
	int err;
	int device;
	device = substream->pcm->device;
	if (device == 0) {
		stream_id = 0;
	} else {
		stream_id = 2;
	}

	chip = snd_pcm_substream_chip(substream);
	//stream_id = substream->pstr->stream;
	s = &chip->s[stream_id];
	err = 0;

	/* note local interrupts are already disabled in the midlevel code */
	spin_lock(&s->dma_lock);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		pr_debug("MXC: SNDRV_PCM_TRIGGER_START\n");
		s->tx_spin = 0;
		/* requested stream startup */
		s->active = 1;
		audio_playback_dma(s);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		pr_debug("MXC: SNDRV_PCM_TRIGGER_STOP\n");
		/* requested stream shutdown */
		audio_playback_stop_dma(s);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
		pr_debug("MXC : SNDRV_PCM_TRIGGER_SUSPEND active = 0\n");
		s->active = 0;
		s->periods = 0;
		break;
	case SNDRV_PCM_TRIGGER_RESUME:
		pr_debug("MXC: SNDRV_PCM_TRIGGER_RESUME\n");
		s->active = 1;
		s->tx_spin = 0;
		audio_playback_dma(s);
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		pr_debug("MXC: SNDRV_PCM_TRIGGER_PAUSE_PUSH\n");
		s->active = 0;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		pr_debug("MXC: SNDRV_PCM_TRIGGER_PAUSE_RELEASE\n");
		s->active = 1;
		if (s->old_offset) {
			s->tx_spin = 0;
			audio_playback_dma(s);
			break;
		}
		break;
	default:
		err = -EINVAL;
		break;
	}
	spin_unlock(&s->dma_lock);
	return err;
}

/*!
  * This function is a dispatcher of command to be executed
  * by the driver for capture.
  *
  * @param	substream	pointer to the structure of the current stream.
  * @param	cmd		command to be executed
  *
  * @return              0 on success, -1 otherwise.
  */
static int
snd_mxc_audio_capture_trigger(snd_pcm_substream_t * substream, int cmd)
{
	mxc_pmic_audio_t *chip;
	int stream_id;
	audio_stream_t *s;
	int err;

	chip = snd_pcm_substream_chip(substream);
	stream_id = substream->pstr->stream;
	s = &chip->s[stream_id];
	err = 0;

	/* note local interrupts are already disabled in the midlevel code */
	spin_lock(&s->dma_lock);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		pr_debug("MXC: SNDRV_PCM_TRIGGER_START\n");
		s->tx_spin = 0;
		/* requested stream startup */
		s->active = 1;
		audio_capture_dma(s);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		pr_debug("MXC: SNDRV_PCM_TRIGGER_STOP\n");
		/* requested stream shutdown */
		audio_capture_stop_dma(s);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
		pr_debug("MXC : SNDRV_PCM_TRIGGER_SUSPEND active = 0\n");
		s->active = 0;
		s->periods = 0;
		break;
	case SNDRV_PCM_TRIGGER_RESUME:
		pr_debug("MXC: SNDRV_PCM_TRIGGER_RESUME\n");
		s->active = 1;
		s->tx_spin = 0;
		audio_capture_dma(s);
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		pr_debug("MXC: SNDRV_PCM_TRIGGER_PAUSE_PUSH\n");
		s->active = 0;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		pr_debug("MXC: SNDRV_PCM_TRIGGER_PAUSE_RELEASE\n");
		s->active = 1;
		if (s->old_offset) {
			s->tx_spin = 0;
			audio_capture_dma(s);
			break;
		}
		break;
	default:
		err = -EINVAL;
		break;
	}
	spin_unlock(&s->dma_lock);
	return err;
}

/*!
  * This function configures the hardware to allow audio
  * playback operations. It is called by ALSA framework.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  * @return              0 on success, -1 otherwise.
  */
static int snd_mxc_audio_playback_prepare(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	audio_stream_t *s;
	int ssi;
	int device = -1, stream_id = -1;

	device = substream->pcm->device;
	if (device == 0)
		stream_id = 0;
	else if (device == 1)
		stream_id = 2;

	chip = snd_pcm_substream_chip(substream);
	s = &chip->s[stream_id];
	ssi = s->ssi;

	normalize_speed_for_pmic(substream);

	configure_dam_pmic_master(ssi);

	configure_ssi_tx(substream);

	ssi_interrupt_enable(ssi, ssi_tx_dma_interrupt_enable);

	if (configure_pmic_playback(substream, stream_id) == -1)
		pr_debug(KERN_ERR "MXC: PMIC Playback Config FAILED\n");
	ssi_interrupt_enable(ssi, ssi_tx_fifo_0_empty);
	/*
	   ssi_transmit_enable(ssi, true);
	 */
	msleep(20);
	set_pmic_channels(substream);
	s->period = 0;
	s->periods = 0;

	msleep(100);

	return 0;
}

/*!
  * This function gets the current capture pointer position.
  * It is called by ALSA framework.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  */
static
snd_pcm_uframes_t snd_mxc_audio_capture_pointer(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;

	chip = snd_pcm_substream_chip(substream);
	return audio_get_capture_dma_pos(&chip->s[substream->pstr->stream]);
}

/*!
  * This function gets the current playback pointer position.
  * It is called by ALSA framework.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  */
static snd_pcm_uframes_t
snd_mxc_audio_playback_pointer(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	int device;
	int stream_id;
	device = substream->pcm->device;
	if (device == 0)
		stream_id = 0;
	else
		stream_id = 2;
	chip = snd_pcm_substream_chip(substream);
	return audio_get_playback_dma_pos(&chip->s[stream_id]);
}

/*!
  * This structure reprensents the capabilities of the driver
  * in capture mode.
  * It is used by ALSA framework.
  */
static snd_pcm_hardware_t snd_mxc_pmic_capture = {
	.info = (SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER |
		 SNDRV_PCM_INFO_MMAP |
		 SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000),
	.rate_min = 8000,
	.rate_max = 16000,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = MAX_BUFFER_SIZE,
	.period_bytes_min = MIN_PERIOD_SIZE,
	.period_bytes_max = DMA_BUF_SIZE,
	.periods_min = MIN_PERIOD,
	.periods_max = MAX_PERIOD,
	.fifo_size = 0,

};

/*!
  * This structure reprensents the capabilities of the driver
  * in playback mode for ST-Dac.
  * It is used by ALSA framework.
  */
static snd_pcm_hardware_t snd_mxc_pmic_playback_stereo = {
	.info = (SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER |
		 SNDRV_PCM_INFO_MMAP |
		 SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = (SNDRV_PCM_RATE_8000_48000 | SNDRV_PCM_RATE_CONTINUOUS),
	.rate_min = 8000,
	.rate_max = 96000,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = MAX_BUFFER_SIZE,
	.period_bytes_min = MIN_PERIOD_SIZE,
	.period_bytes_max = DMA_BUF_SIZE,
	.periods_min = MIN_PERIOD,
	.periods_max = MAX_PERIOD,
	.fifo_size = 0,

};

/*!
  * This structure reprensents the capabilities of the driver
  * in playback mode for Voice-codec.
  * It is used by ALSA framework.
  */
static snd_pcm_hardware_t snd_mxc_pmic_playback_mono = {
	.info = (SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER |
		 SNDRV_PCM_INFO_MMAP |
		 SNDRV_PCM_INFO_MMAP_VALID |
		 SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000),
	.rate_min = 8000,
	.rate_max = 16000,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = MAX_BUFFER_SIZE,
	.period_bytes_min = MIN_PERIOD_SIZE,
	.period_bytes_max = DMA_BUF_SIZE,
	.periods_min = MIN_PERIOD,
	.periods_max = MAX_PERIOD,
	.fifo_size = 0,

};

/*!
  * This function opens a PMIC audio device in playback mode
  * It is called by ALSA framework.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  * @return              0 on success, -1 otherwise.
  */
static int snd_card_mxc_audio_playback_open(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	snd_pcm_runtime_t *runtime;
	int stream_id = -1;
	int err;
	PMIC_AUDIO_HANDLE temp_handle;
	int device = -1;

	device = substream->pcm->device;

	chip = snd_pcm_substream_chip(substream);
	runtime = substream->runtime;
	if (device == 0)
		stream_id = 0;
	else if (device == 1)
		stream_id = 2;

	err = -1;

	if (stream_id == 0) {
		if (PMIC_SUCCESS == pmic_audio_open(&temp_handle, STEREO_DAC)) {
			audio_mixer_control.stdac_handle = temp_handle;
			audio_mixer_control.stdac_playback_active = 1;
			chip->s[stream_id].pmic_audio_device.handle =
			    temp_handle;
		} else {
			return -EBUSY;
		}
	} else if (stream_id == 2) {
		audio_mixer_control.codec_playback_active = 1;
		if (PMIC_SUCCESS == pmic_audio_open(&temp_handle, VOICE_CODEC)) {
			audio_mixer_control.voice_codec_handle = temp_handle;
			chip->s[stream_id].pmic_audio_device.handle =
			    temp_handle;
		} else {
			if (audio_mixer_control.codec_capture_active) {
				temp_handle =
				    audio_mixer_control.voice_codec_handle;
			} else {
				return -EBUSY;
			}
		}
	}

	pmic_audio_antipop_enable(ANTI_POP_RAMP_SLOW);
	msleep(250);

	chip->s[stream_id].stream = substream;

	if (stream_id == 0)
		runtime->hw = snd_mxc_pmic_playback_stereo;
	else if (stream_id == 2)
		runtime->hw = snd_mxc_pmic_playback_mono;
	else
		return err;

	if ((err = snd_pcm_hw_constraint_integer(runtime,
						 SNDRV_PCM_HW_PARAM_PERIODS)) <
	    0)
		return err;
	if (stream_id == 0) {
		if ((err = snd_pcm_hw_constraint_list(runtime, 0,
						      SNDRV_PCM_HW_PARAM_RATE,
						      &hw_playback_rates_stereo))
		    < 0)
			return err;
	} else if (stream_id == 2) {
		if ((err = snd_pcm_hw_constraint_list(runtime, 0,
						      SNDRV_PCM_HW_PARAM_RATE,
						      &hw_playback_rates_mono))
		    < 0)
			return err;
	}
	msleep(10);

	/* setup DMA controller for playback */
	if ((err =
	     configure_write_channel(&mxc_audio->s[stream_id],
				     audio_playback_dma_callback,
				     stream_id)) < 0)
		return err;

	return 0;
}

/*!
  * This function closes an PMIC audio device for playback.
  * It is called by ALSA framework.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  * @return              0 on success, -1 otherwise.
  */
static int snd_card_mxc_audio_playback_close(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	audio_stream_t *s;
	PMIC_AUDIO_HANDLE handle;
	int ssi;
	int device, stream_id = -1;

	handle = (PMIC_AUDIO_HANDLE) NULL;

	device = substream->pcm->device;
	if (device == 0)
		stream_id = 0;
	else if (device == 1)
		stream_id = 2;

	chip = snd_pcm_substream_chip(substream);
	s = &chip->s[stream_id];
	ssi = s->ssi;

	if (stream_id == 0) {
		disable_stereodac();
		audio_mixer_control.stdac_playback_active = 0;
		handle = audio_mixer_control.stdac_handle;
		audio_mixer_control.stdac_handle = NULL;
		chip->s[stream_id].pmic_audio_device.handle = NULL;
	} else if (stream_id == 2) {
		audio_mixer_control.codec_playback_active = 0;
		handle = audio_mixer_control.voice_codec_handle;
		disable_codec(handle);
		audio_mixer_control.voice_codec_handle = NULL;
		chip->s[stream_id].pmic_audio_device.handle = NULL;
	}

	pmic_audio_close(handle);

	ssi_transmit_enable(ssi, false);
	ssi_interrupt_disable(ssi, ssi_tx_dma_interrupt_enable);
	ssi_tx_fifo_enable(ssi, ssi_fifo_0, false);
	ssi_enable(ssi, false);
	mxc_dma_free((mxc_audio->s[stream_id]).dma_wchannel);

	chip->s[stream_id].stream = NULL;

	return 0;
}

/*!
  * This function closes a PMIC audio device for capture.
  * It is called by ALSA framework.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  * @return              0 on success, -1 otherwise.
  */
static int snd_card_mxc_audio_capture_close(snd_pcm_substream_t * substream)
{
	PMIC_AUDIO_HANDLE handle;
	mxc_pmic_audio_t *chip;
	audio_stream_t *s;
	int ssi;

	chip = snd_pcm_substream_chip(substream);
	s = &chip->s[substream->pstr->stream];
	ssi = s->ssi;

	audio_mixer_control.codec_capture_active = 0;
	handle = audio_mixer_control.voice_codec_handle;
	disable_codec(handle);
	audio_mixer_control.voice_codec_handle = NULL;
	chip->s[SNDRV_PCM_STREAM_CAPTURE].pmic_audio_device.handle = NULL;

	pmic_audio_close(handle);

	ssi_receive_enable(ssi, false);
	ssi_interrupt_disable(ssi, ssi_rx_dma_interrupt_enable);
	ssi_rx_fifo_enable(ssi, ssi_fifo_0, false);
	ssi_enable(ssi, false);
	mxc_dma_free((mxc_audio->s[1]).dma_wchannel);

	chip->s[substream->pstr->stream].stream = NULL;

	return 0;
}

/*!
  * This function configure the Audio HW in terms of memory allocation.
  * It is called by ALSA framework.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  * @return              0 on success, -1 otherwise.
  */
static int snd_mxc_audio_hw_params(snd_pcm_substream_t * substream,
				   snd_pcm_hw_params_t * hw_params)
{
	snd_pcm_runtime_t *runtime;
	int ret;

	runtime = substream->runtime;
	ret =
	    snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
	if (ret < 0)
		return ret;

	runtime->dma_addr = virt_to_phys(runtime->dma_area);

	pr_debug("MXC: snd_mxc_audio_hw_params runtime->dma_addr 0x(%x)\n",
		 (unsigned int)runtime->dma_addr);
	pr_debug("MXC: snd_mxc_audio_hw_params runtime->dma_area 0x(%x)\n",
		 (unsigned int)runtime->dma_area);
	pr_debug("MXC: snd_mxc_audio_hw_params runtime->dma_bytes 0x(%x)\n",
		 (unsigned int)runtime->dma_bytes);

	return ret;
}

/*!
  * This function frees the audio hardware at the end of playback/capture.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  * @return              0 on success, -1 otherwise.
  */
static int snd_mxc_audio_hw_free(snd_pcm_substream_t * substream)
{
	return snd_pcm_lib_free_pages(substream);
}

/*!
  * This function configures the hardware to allow audio
  * capture operations. It is called by ALSA framework.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  * @return              0 on success, -1 otherwise.
  */
static int snd_mxc_audio_capture_prepare(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	audio_stream_t *s;
	int ssi;

	chip = snd_pcm_substream_chip(substream);
	s = &chip->s[substream->pstr->stream];
	ssi = s->ssi;

	normalize_speed_for_pmic(substream);

	pr_debug("substream->pstr->stream %d\n", substream->pstr->stream);
	pr_debug("SSI %d\n", ssi + 1);
	configure_dam_pmic_master(ssi);

	configure_ssi_rx(substream);

	ssi_interrupt_enable(ssi, ssi_rx_dma_interrupt_enable);

	if (configure_pmic_recording(substream) == -1)
		pr_debug(KERN_ERR "MXC: PMIC Record Config FAILED\n");

	ssi_interrupt_enable(ssi, ssi_rx_fifo_0_full);
	ssi_receive_enable(ssi, true);

	msleep(20);
	set_pmic_channels(substream);

	s->period = 0;
	s->periods = 0;

	return 0;
}

/*!
  * This function opens an PMIC audio device in capture mode
  * on Codec.
  * It is called by ALSA framework.
  *
  * @param	substream	pointer to the structure of the current stream.
  *
  * @return              0 on success, -1 otherwise.
  */
static int snd_card_mxc_audio_capture_open(snd_pcm_substream_t * substream)
{
	mxc_pmic_audio_t *chip;
	snd_pcm_runtime_t *runtime;
	int stream_id;
	int err;
	PMIC_AUDIO_HANDLE temp_handle;

	chip = snd_pcm_substream_chip(substream);
	runtime = substream->runtime;
	stream_id = substream->pstr->stream;
	err = -1;

	if (PMIC_SUCCESS == pmic_audio_open(&temp_handle, VOICE_CODEC)) {
		audio_mixer_control.voice_codec_handle = temp_handle;
		audio_mixer_control.codec_capture_active = 1;
		chip->s[SNDRV_PCM_STREAM_CAPTURE].pmic_audio_device.handle =
		    temp_handle;
	} else {
		if (audio_mixer_control.codec_playback_active) {
			temp_handle = audio_mixer_control.voice_codec_handle;
		} else {
			return -EBUSY;
		}
	}
	pmic_audio_antipop_enable(ANTI_POP_RAMP_SLOW);

	chip->s[stream_id].stream = substream;

	if (stream_id == SNDRV_PCM_STREAM_CAPTURE) {
		runtime->hw = snd_mxc_pmic_capture;
	} else {
		return err;
	}

	if ((err = snd_pcm_hw_constraint_integer(runtime,
						 SNDRV_PCM_HW_PARAM_PERIODS)) <
	    0) {
		return err;
	}

	if ((err = snd_pcm_hw_constraint_list(runtime, 0,
					      SNDRV_PCM_HW_PARAM_RATE,
					      &hw_capture_rates)) < 0) {
		return err;
	}

	/* setup DMA controller for Record */
	err = configure_read_channel(&mxc_audio->s[SNDRV_PCM_STREAM_CAPTURE],
				     audio_capture_dma_callback);
	if (err < 0) {
		return err;
	}

	msleep(50);

	return 0;
}

/*!
  * This structure is the list of operation that the driver
  * must provide for the capture interface
  */
static snd_pcm_ops_t snd_card_mxc_audio_capture_ops = {
	.open = snd_card_mxc_audio_capture_open,
	.close = snd_card_mxc_audio_capture_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = snd_mxc_audio_hw_params,
	.hw_free = snd_mxc_audio_hw_free,
	.prepare = snd_mxc_audio_capture_prepare,
	.trigger = snd_mxc_audio_capture_trigger,
	.pointer = snd_mxc_audio_capture_pointer,
};

/*!
  * This structure is the list of operation that the driver
  * must provide for the playback interface
  */
static snd_pcm_ops_t snd_card_mxc_audio_playback_ops = {
	.open = snd_card_mxc_audio_playback_open,
	.close = snd_card_mxc_audio_playback_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = snd_mxc_audio_hw_params,
	.hw_free = snd_mxc_audio_hw_free,
	.prepare = snd_mxc_audio_playback_prepare,
	.trigger = snd_mxc_audio_playback_trigger,
	.pointer = snd_mxc_audio_playback_pointer,
};

/*!
  * This functions initializes the capture audio device supported by
  * PMIC IC.
  *
  * @param	mxc_audio	pointer to the sound card structure
  *
  */
void init_device_capture(mxc_pmic_audio_t * mxc_audio)
{
	audio_stream_t *audio_stream;
	pmic_audio_device_t *pmic_device;

	audio_stream = &mxc_audio->s[SNDRV_PCM_STREAM_CAPTURE];
	pmic_device = &audio_stream->pmic_audio_device;

	/* These parameters defines the identity of
	 * the device (codec or stereodac)
	 */
	if (machine_is_mx27_mdk27v0()) {
		audio_stream->ssi = SSI2;
		audio_stream->dam_port = DAM_PORT_3;
		pmic_device->ssi = SSI2;
	} else {
		audio_stream->ssi = SSI1;
		audio_stream->dam_port = DAM_PORT_4;
		pmic_device->ssi = SSI1;
	}

	pmic_device->mode = BUS_MASTER_MODE;
	pmic_device->protocol = NETWORK_MODE;

	if (machine_is_mx31ads()) {
		pmic_device->pll = CLOCK_IN_CLIB;
	} else {
		pmic_device->pll = CLOCK_IN_CLIA;
	}
	pmic_device->pll_rate = VCODEC_CLI_26MHZ;
	pmic_device->bcl_inverted = 0;
	pmic_device->fs_inverted = 0;

}

/*!
  * This functions initializes the playback audio device supported by
  * PMIC IC.
  *
  * @param	mxc_audio	pointer to the sound card structure.
  * @param	device		device ID of PCM instance.
  *
  */
void init_device_playback(mxc_pmic_audio_t * mxc_audio, int device)
{
	audio_stream_t *audio_stream;
	pmic_audio_device_t *pmic_device;
	if (device == 0)
		audio_stream = &mxc_audio->s[0];
	else
		audio_stream = &mxc_audio->s[2];
	pmic_device = &audio_stream->pmic_audio_device;

	/* These parameters defines the identity of
	 * the device (codec or stereodac)
	 */
	if (device == 0) {
		audio_stream->ssi = SSI2;
		if (machine_is_mx27_mdk27v0()) {
			audio_stream->dam_port = DAM_PORT_3;
		} else {
			audio_stream->dam_port = DAM_PORT_5;
		}
		pmic_device->ssi = SSI2;

		pmic_device->mode = BUS_MASTER_MODE;
		pmic_device->protocol = NETWORK_MODE;

		if (machine_is_mx31ads()) {
			pmic_device->pll = CLOCK_IN_CLIB;
		} else {
			pmic_device->pll = CLOCK_IN_CLIA;
		}
		pmic_device->pll_rate = STDAC_CLI_26MHZ;

		pmic_device->bcl_inverted = 0;
		pmic_device->fs_inverted = 0;

	} else if (device == 1) {
		audio_stream->ssi = SSI1;
		audio_stream->dam_port = DAM_PORT_4;
		pmic_device->ssi = SSI1;

		pmic_device->mode = BUS_MASTER_MODE;
		pmic_device->protocol = NETWORK_MODE;

		if (machine_is_mx31ads()) {
			pmic_device->pll = CLOCK_IN_CLIB;
		} else {
			pmic_device->pll = CLOCK_IN_CLIA;
		}
		pmic_device->pll_rate = VCODEC_CLI_26MHZ;
		pmic_device->bcl_inverted = 0;
		pmic_device->fs_inverted = 0;
	}

}

/*!
 * This functions initializes the mixer related information
 *
 * @param	mxc_audio	pointer to the sound card structure.
 *
 */
void mxc_pmic_mixer_controls_init(mxc_pmic_audio_t * mxc_audio)
{
	audio_mixer_control_t *audio_control;
	int i = 0;

	audio_control = &audio_mixer_control;

	memset(audio_control, 0, sizeof(audio_mixer_control_t));
	sema_init(&audio_control->sem, 1);

	audio_control->input_device = SOUND_MASK_MIC;
	audio_control->output_device = SOUND_MASK_VOLUME | SOUND_MASK_PCM;

	/* PMIC has to internal sources that can be routed to output
	   One is codec direct out and the other is mixer out
	   Initially we configure all outputs to have no source and
	   will be later configured either by PCM stream handler or mixer */
	for (i = 0; i < OP_MAXDEV && i != OP_HEADSET; i++) {
		audio_control->source_for_output[i] = MIXER_OUT;
	}

	/* These bits are initially reset and set when playback begins */
	audio_control->codec_out_to_mixer = 0;
	audio_control->stdac_out_to_mixer = 0;

	audio_control->mixer_balance = 50;
	if (machine_is_mx31ads())
		audio_control->mixer_mono_adder = STEREO_OPPOSITE_PHASE;
	else
		audio_control->mixer_mono_adder = MONO_ADDER_OFF;
	/* Default values for input and output */
	audio_control->input_volume = ((40 << 8) & 0xff00) | (40 & 0x00ff);
	audio_control->master_volume_out = ((50 << 8) & 0xff00) | (50 & 0x00ff);

	if (PMIC_SUCCESS != pmic_audio_set_autodetect(1))
		msleep(30);
}

/*!
 * This functions initializes the 2 audio devices supported by
 * PMIC IC. The parameters define the type of device (CODEC or STEREODAC)
 *
 * @param	mxc_audio	pointer to the sound card structure.
 * @param	device	        device id of the PCM stream.
 *
 */
void mxc_pmic_audio_init(mxc_pmic_audio_t * mxc_audio, int device)
{
	if (device == 0) {
		mxc_audio->s[SNDRV_PCM_STREAM_PLAYBACK].id = "Audio out";
		mxc_audio->s[SNDRV_PCM_STREAM_PLAYBACK].stream_id =
		    SNDRV_PCM_STREAM_PLAYBACK;
		mxc_audio->s[SNDRV_PCM_STREAM_CAPTURE].id = "Audio in";
		mxc_audio->s[SNDRV_PCM_STREAM_CAPTURE].stream_id =
		    SNDRV_PCM_STREAM_CAPTURE;
	} else if (device == 1) {
		mxc_audio->s[2].id = "Audio out";
		mxc_audio->s[2].stream_id = 2;
	}

	init_device_playback(mxc_audio, device);
	if (!device) {
		init_device_capture(mxc_audio);
	}
}

/*!
  * This function the soundcard structure.
  *
  * @param	mxc_audio	pointer to the sound card structure.
  * @param	device		the device index (zero based)
  *
  * @return              0 on success, -1 otherwise.
  */
static int __init snd_card_mxc_audio_pcm(mxc_pmic_audio_t * mxc_audio,
					 int device)
{
	snd_pcm_t *pcm;
	int err;

	/*
	 * Create a new PCM instance with 1 capture stream and 1 playback substream
	 */
	if ((err = snd_pcm_new(mxc_audio->card, "MXC", device, 1, 1, &pcm)) < 0) {
		return err;
	}

	/*
	 * this sets up our initial buffers and sets the dma_type to isa.
	 * isa works but I'm not sure why (or if) it's the right choice
	 * this may be too large, trying it for now
	 */
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      snd_dma_continuous_data
					      (GFP_KERNEL), MAX_BUFFER_SIZE * 2,
					      MAX_BUFFER_SIZE * 2);

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&snd_card_mxc_audio_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,
			&snd_card_mxc_audio_capture_ops);

	pcm->private_data = mxc_audio;
	pcm->info_flags = 0;
	strncpy(pcm->name, SOUND_CARD_NAME, sizeof(pcm->name));
	mxc_audio->pcm[device] = pcm;
	mxc_pmic_audio_init(mxc_audio, device);

	/* Allocating a second device for PCM playback on voice codec */
	device = 1;
	if ((err = snd_pcm_new(mxc_audio->card, "MXC", device, 1, 0, &pcm)) < 0) {
		return err;
	}
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      snd_dma_continuous_data
					      (GFP_KERNEL), MAX_BUFFER_SIZE * 2,
					      MAX_BUFFER_SIZE * 2);

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&snd_card_mxc_audio_playback_ops);
	pcm->private_data = mxc_audio;
	pcm->info_flags = 0;
	strncpy(pcm->name, SOUND_CARD_NAME, sizeof(pcm->name));
	mxc_audio->pcm[device] = pcm;
	mxc_pmic_audio_init(mxc_audio, device);
	/* End of allocation */
	/* FGA for record and not hard coded playback */
	mxc_pmic_mixer_controls_init(mxc_audio);

	return 0;
}

#ifdef CONFIG_PM
/*!
  * This function suspends all active streams.
  *
  * TBD
  *
  * @param	card	pointer to the sound card structure.
  * @param	state	requested state
  *
  * @return              0 on success, -1 otherwise.
  */
static int snd_mxc_audio_suspend(struct platform_device *dev,
				 pm_message_t state)
{
	struct snd_card *card = platform_get_drvdata(dev);
	mxc_pmic_audio_t *chip = card->private_data;

	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
	snd_pcm_suspend_all(chip->pcm[0]);
	//mxc_alsa_audio_shutdown(chip);

	return 0;
}

/*!
  * This function resumes all suspended streams.
  *
  * TBD
  *
  * @param	card	pointer to the sound card structure.
  * @param	state	requested state
  *
  * @return              0 on success, -1 otherwise.
  */
static int snd_mxc_audio_resume(struct platform_device *dev)
{
	struct snd_card *card = platform_get_drvdata(dev);

	snd_power_change_state(card, SNDRV_CTL_POWER_D0);

	return 0;
}
#endif				/* COMFIG_PM */

/*!
  * This function frees the sound card structure
  *
  * @param	card	pointer to the sound card structure.
  *
  * @return              0 on success, -1 otherwise.
  */
void snd_mxc_audio_free(snd_card_t * card)
{
	mxc_pmic_audio_t *chip;

	chip = card->private_data;
	audio_dma_free(&chip->s[SNDRV_PCM_STREAM_PLAYBACK]);
	audio_dma_free(&chip->s[SNDRV_PCM_STREAM_CAPTURE]);
	mxc_audio = NULL;
	card->private_data = NULL;
	kfree(chip);

}

/*!
  * This function initializes the driver in terms of memory of the soundcard
  * and some basic HW clock settings.
  *
  * @return              0 on success, -1 otherwise.
  */
static int __init mxc_alsa_audio_probe(struct platform_device *dev)
{
	int err;
	snd_card_t *card;

	/* register the soundcard */
	card = snd_card_new(-1, id, THIS_MODULE, sizeof(mxc_pmic_audio_t));
	if (card == NULL) {
		return -ENOMEM;
	}

	mxc_audio = kcalloc(1, sizeof(*mxc_audio), GFP_KERNEL);
	if (mxc_audio == NULL) {
		return -ENOMEM;
	}

	card->private_data = (void *)mxc_audio;
	card->private_free = snd_mxc_audio_free;

	mxc_audio->card = card;
	card->dev = &dev->dev;
	if ((err = snd_card_mxc_audio_pcm(mxc_audio, 0)) < 0) {
		goto nodev;
	}

	if (0 == mxc_alsa_create_ctl(card, (void *)&audio_mixer_control))
		printk(KERN_INFO "Control ALSA component registered\n");

	/* Set autodetect feature in order to allow audio operations */

	spin_lock_init(&(mxc_audio->s[0].dma_lock));
	spin_lock_init(&(mxc_audio->s[1].dma_lock));
	spin_lock_init(&(mxc_audio->s[2].dma_lock));

	strcpy(card->driver, "MXC");
	strcpy(card->shortname, "PMIC-audio");
	sprintf(card->longname, "MXC Freescale with PMIC");

	if ((err = snd_card_register(card)) == 0) {
		pr_debug(KERN_INFO "MXC audio support initialized\n");
		platform_set_drvdata(dev, card);
		return 0;
	}

      nodev:
	snd_card_free(card);
	return err;
}

static int mxc_alsa_audio_remove(struct platform_device *dev)
{
	snd_card_free(mxc_audio->card);
	kfree(mxc_audio);
	platform_set_drvdata(dev, NULL);

	return 0;
}

#define mxc_ALSA "mxc_ALSA"

static struct platform_driver mxc_alsa_audio_driver = {
	.probe = mxc_alsa_audio_probe,
	.remove = mxc_alsa_audio_remove,
#ifdef CONFIG_PM
	.suspend = snd_mxc_audio_suspend,
	.resume = snd_mxc_audio_resume,
#endif
	.driver = {
		   .name = "mxc_ALSA",
		   },
};

static int __init mxc_alsa_audio_init(void)
{
	int err;
	if ((err = platform_driver_register(&mxc_alsa_audio_driver)) < 0)
		return err;
	device = platform_device_register_simple(mxc_ALSA, -1, NULL, 0);
	if (!IS_ERR(device)) {
		if (platform_get_drvdata(device))
			return 0;
		platform_device_unregister(device);
		platform_driver_unregister(&mxc_alsa_audio_driver);
		err = -ENODEV;
	} else
		err = PTR_ERR(device);

	platform_driver_unregister(&mxc_alsa_audio_driver);
	return err;
}

/*!
  * This function frees the sound driver structure.
  *
  */

static void __exit mxc_alsa_audio_exit(void)
{
	platform_device_unregister(device);
	platform_driver_unregister(&mxc_alsa_audio_driver);
}

module_init(mxc_alsa_audio_init);
module_exit(mxc_alsa_audio_exit);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MXC driver for ALSA");
MODULE_SUPPORTED_DEVICE("{{PMIC}}");

module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for MXC  + PMIC soundcard.");
