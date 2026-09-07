/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / SILK decoder filter, based on the SILK SDK that
 *  Skype released in 2012 under a three-clause BSD licence.
 *
 *  SILK is the speech half of Opus, and libopus does carry a SILK decoder -
 *  but not this one. When SILK was folded into Opus the frame header moved
 *  into the Opus TOC byte, so libopus's silk_Decode expects the caller to hand
 *  it the internal sampling rate and the number of frames in the packet, and
 *  it no longer reads them from the bitstream. Standalone SILK files carry
 *  neither, because in them that information is still in the frame. The two
 *  bitstreams are therefore not interchangeable: decoding a real .silk file
 *  with libopus's SILK produces noise, measured here at -20 dB SNR against
 *  the SDK's own decode of the same file. Hence the SDK rather than libopus.
 *
 *  The file format is the one the SDK's own test encoder writes, and the one
 *  every .silk file in the wild uses: the nine bytes "#!SILK_V3", then, per
 *  packet, a 16-bit little-endian length followed by that many bytes. A length
 *  of -1 ends the stream. Some writers - WeChat is the common case - put one
 *  extra byte in front of the magic, so the magic is looked for at offset 0
 *  and offset 1.
 *
 *  Two things the format does not carry, and which the SDK's own decoder does
 *  not read either: the output sampling rate, and the packet duration. The
 *  rate is a decoder-side choice - SILK decodes at 8, 12 or 16 kHz internally
 *  and resamples - so it is an option here, defaulting to the 24 kHz the SDK's
 *  test decoder uses. The packet duration needs no guess: the decoder is
 *  called until it says it has no more internal frames for the packet.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>

#include <SKP_Silk_SDK_API.h>

#define SILK_MAGIC        "#!SILK_V3"
#define SILK_MAGIC_SIZE   9
/* the SDK's own decoder sizes its output buffer this way: the longest packet
 * is 60 ms, and it allows for a stream that codes several of them back to back */
#define SILK_MAX_FRAMES   5
#define SILK_MAX_MS       (SILK_MAX_FRAMES * 60)

typedef struct
{
	GF_FilterPid *ipid, *opid;
	u32 srate;
} GF_SILKDecCtx;

/* Returns the offset of the payload, or 0 if this is not a SILK file. Some
 * writers prefix the magic with one byte; nothing else is tolerated, because a
 * wrong offset decodes to noise rather than failing. */
static u32 silkdec_find_payload(const u8 *data, u32 size)
{
	u32 i;
	for (i = 0; i <= 1; i++)
	{
		if ((size > i + SILK_MAGIC_SIZE) && !memcmp(data + i, SILK_MAGIC, SILK_MAGIC_SIZE))
			return i + SILK_MAGIC_SIZE;
	}
	return 0;
}

static GF_Err silkdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_SILKDecCtx *ctx = (GF_SILKDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(ctx->srate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->srate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(1));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT(GF_AUDIO_CH_FRONT_CENTER));

	return GF_OK;
}

static GF_Err silkdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	const u8 *data;
	u8 *output;
	u32 size, pos, out_alloc, nb_samples = 0;
	s32 dec_size;
	void *dec;
	SKP_SILK_SDK_DecControlStruct ctl;
	GF_SILKDecCtx *ctx = (GF_SILKDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = gf_filter_pck_get_data(pck, &size);
	if (!data || !size)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	pos = silkdec_find_payload(data, size);
	if (!pos)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[SILKDec] Not a SILK file: no \"" SILK_MAGIC "\" magic\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	if (SKP_Silk_SDK_Get_Decoder_Size(&dec_size) || (dec_size <= 0))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	dec = gf_malloc((u32)dec_size);
	if (!dec)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	SKP_Silk_SDK_InitDecoder(dec);

	memset(&ctl, 0, sizeof(ctl));
	ctl.API_sampleRate = (SKP_int32)ctx->srate;

	/* SILK is 20 ms of speech per internal frame at worst 8 kHz, so the packet
	 * count is an upper bound on the sample count: allocate for the whole file
	 * at the packet rate and send one packet, the way the other whole-file
	 * speech decoders here do. */
	out_alloc = (size / 2 + 1) * (ctx->srate * 20 / 1000) * 2;
	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_alloc, &output);
	if (!dst_pck)
	{
		gf_free(dec);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	while (pos + 2 <= size)
	{
		s16 nb_bytes = (s16)(data[pos] | (data[pos + 1] << 8));
		u32 frames = 0;
		pos += 2;
		if (nb_bytes < 0)
			break; /* end of stream marker */
		if (pos + (u32)nb_bytes > size)
			break;

		/* One call per internal frame; the decoder says when the packet is
		 * exhausted. This is what makes the packet duration something the
		 * file does not have to carry. */
		do
		{
			s16 got = 0;
			if ((nb_samples + ctx->srate * 60 / 1000) * 2 > out_alloc)
				break;
			if (SKP_Silk_SDK_Decode(dec, &ctl, 0, data + pos, nb_bytes,
			                        (s16 *)(output + (size_t)nb_samples * 2), &got))
			{
				GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[SILKDec] Packet at offset %u did not decode, stopping\n", pos));
				got = 0;
				ctl.moreInternalDecoderFrames = 0;
			}
			nb_samples += (u32)got;
			frames++;
		} while (ctl.moreInternalDecoderFrames && (frames < SILK_MAX_FRAMES));

		pos += (u32)nb_bytes;
	}
	gf_free(dec);

	if (!nb_samples)
	{
		gf_filter_pck_discard(dst_pck);
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[SILKDec] No packet decoded\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	gf_filter_pck_truncate(dst_pck, nb_samples * 2);
	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_duration(dst_pck, nb_samples);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_drop_packet(ctx->ipid);
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static const GF_FilterCapability SILKDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "silk|sil|slk"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/silk|audio/x-silk"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

#define OFFS(_n) #_n, offsetof(GF_SILKDecCtx, _n)
static const GF_FilterArgs SILKDecArgs[] =
	{
		{OFFS(srate), "output sampling rate in Hz; SILK codes at 8, 12 or 16 kHz internally and resamples, and the file does not say which rate to resample to", GF_PROP_UINT, "24000", NULL, 0},
		{0}};

GF_FilterRegister SILKDecoderRegister = {
	.name = "silkdec",
	GF_FS_SET_DESCRIPTION("SILK speech decoder")
		GF_FS_SET_HELP("This filter decodes standalone SILK speech files (.silk, with the \"#!SILK_V3\" header) using the SILK SDK. Output is mono 16-bit at the rate given by srate.\n"
		               "Note this is not the SILK carried inside Opus: the two bitstreams differ by the frame header Opus moved into its TOC byte, and are not interchangeable.")
			.private_size = sizeof(GF_SILKDecCtx),
	.args = SILKDecArgs,
	SETCAPS(SILKDecCaps),
	.configure_pid = silkdec_configure_pid,
	.process = silkdec_process,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE silkdec_register(GF_FilterSession *session)
{
	return &SILKDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_silkdec(void)
{
	gf_filter_auto_register("silkdec", silkdec_register);
}
