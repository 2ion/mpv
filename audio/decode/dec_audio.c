/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>

#include <libavutil/mem.h>

#include "demux/codec_tags.h"

#include "common/codecs.h"
#include "common/msg.h"
#include "misc/bstr.h"

#include "stream/stream.h"
#include "demux/demux.h"

#include "demux/stheader.h"

#include "dec_audio.h"
#include "ad.h"
#include "audio/format.h"
#include "audio/audio.h"
#include "audio/audio_buffer.h"

#include "audio/filter/af.h"

extern const struct ad_functions ad_lavc;
extern const struct ad_functions ad_spdif;

static const struct ad_functions * const ad_drivers[] = {
    &ad_lavc,
    &ad_spdif,
    NULL
};

static void uninit_decoder(struct dec_audio *d_audio)
{
    audio_reset_decoding(d_audio);
    if (d_audio->ad_driver) {
        MP_VERBOSE(d_audio, "Uninit audio decoder.\n");
        d_audio->ad_driver->uninit(d_audio);
    }
    d_audio->ad_driver = NULL;
    talloc_free(d_audio->priv);
    d_audio->priv = NULL;
}

static int init_audio_codec(struct dec_audio *d_audio, const char *decoder)
{
    if (!d_audio->ad_driver->init(d_audio, decoder)) {
        MP_VERBOSE(d_audio, "Audio decoder init failed.\n");
        d_audio->ad_driver = NULL;
        uninit_decoder(d_audio);
        return 0;
    }

    return 1;
}

struct mp_decoder_list *audio_decoder_list(void)
{
    struct mp_decoder_list *list = talloc_zero(NULL, struct mp_decoder_list);
    for (int i = 0; ad_drivers[i] != NULL; i++)
        ad_drivers[i]->add_decoders(list);
    return list;
}

static struct mp_decoder_list *audio_select_decoders(struct dec_audio *d_audio)
{
    struct MPOpts *opts = d_audio->opts;
    const char *codec = d_audio->header->codec->codec;

    struct mp_decoder_list *list = audio_decoder_list();
    struct mp_decoder_list *new =
        mp_select_decoders(list, codec, opts->audio_decoders);
    if (d_audio->try_spdif) {
        struct mp_decoder_list *spdif =
            mp_select_decoder_list(list, codec, "spdif", opts->audio_spdif);
        mp_append_decoders(spdif, new);
        talloc_free(new);
        new = spdif;
    }
    talloc_free(list);
    return new;
}

static const struct ad_functions *find_driver(const char *name)
{
    for (int i = 0; ad_drivers[i] != NULL; i++) {
        if (strcmp(ad_drivers[i]->name, name) == 0)
            return ad_drivers[i];
    }
    return NULL;
}

int audio_init_best_codec(struct dec_audio *d_audio)
{
    uninit_decoder(d_audio);
    assert(!d_audio->ad_driver);

    struct mp_decoder_entry *decoder = NULL;
    struct mp_decoder_list *list = audio_select_decoders(d_audio);

    mp_print_decoders(d_audio->log, MSGL_V, "Codec list:", list);

    for (int n = 0; n < list->num_entries; n++) {
        struct mp_decoder_entry *sel = &list->entries[n];
        const struct ad_functions *driver = find_driver(sel->family);
        if (!driver)
            continue;
        MP_VERBOSE(d_audio, "Opening audio decoder %s:%s\n",
                   sel->family, sel->decoder);
        d_audio->ad_driver = driver;
        if (init_audio_codec(d_audio, sel->decoder)) {
            decoder = sel;
            break;
        }
        MP_WARN(d_audio, "Audio decoder init failed for "
                "%s:%s\n", sel->family, sel->decoder);
    }

    if (d_audio->ad_driver) {
        d_audio->decoder_desc =
            talloc_asprintf(d_audio, "%s [%s:%s]", decoder->desc, decoder->family,
                            decoder->decoder);
        MP_VERBOSE(d_audio, "Selected audio codec: %s\n", d_audio->decoder_desc);
    } else {
        MP_ERR(d_audio, "Failed to initialize an audio decoder for codec '%s'.\n",
               d_audio->header->codec->codec);
    }

    talloc_free(list);
    return !!d_audio->ad_driver;
}

void audio_uninit(struct dec_audio *d_audio)
{
    if (!d_audio)
        return;
    MP_VERBOSE(d_audio, "Uninit audio filters...\n");
    uninit_decoder(d_audio);
    talloc_free(d_audio->current_frame);
    talloc_free(d_audio->packet);
    talloc_free(d_audio);
}

void audio_reset_decoding(struct dec_audio *d_audio)
{
    if (d_audio->ad_driver)
        d_audio->ad_driver->control(d_audio, ADCTRL_RESET, NULL);
    d_audio->pts = MP_NOPTS_VALUE;
    d_audio->pts_reset = false;
    talloc_free(d_audio->current_frame);
    d_audio->current_frame = NULL;
    talloc_free(d_audio->packet);
    d_audio->packet = NULL;
}

static void fix_audio_pts(struct dec_audio *da)
{
    if (!da->current_frame)
        return;

    if (da->current_frame->pts != MP_NOPTS_VALUE) {
        double newpts = da->current_frame->pts;
        // Keep the interpolated timestamp if it doesn't deviate more
        // than 1 ms from the real one. (MKV rounded timestamps.)
        if (da->pts == MP_NOPTS_VALUE || fabs(da->pts - newpts) > 0.001) {
            // Attempt to detect jumps in PTS. Even for the lowest
            // sample rates and with worst container rounded timestamp,
            // this should be a margin more than enough.
            if (da->pts != MP_NOPTS_VALUE && fabs(newpts - da->pts) > 0.1) {
                MP_WARN(da, "Invalid audio PTS: %f -> %f\n",
                        da->pts, newpts);
                da->pts_reset = true;
            }
            da->pts = da->current_frame->pts;
        }
    }

    if (da->pts == MP_NOPTS_VALUE && da->header->missing_timestamps)
        da->pts = 0;

    da->current_frame->pts = da->pts;

    if (da->pts != MP_NOPTS_VALUE)
        da->pts += da->current_frame->samples / (double)da->current_frame->rate;
}

void audio_work(struct dec_audio *da)
{
    if (da->current_frame)
        return;

    if (!da->packet && demux_read_packet_async(da->header, &da->packet) == 0) {
        da->current_state = AUDIO_WAIT;
        return;
    }

    bool had_packet = !!da->packet;

    int ret = da->ad_driver->decode_packet(da, da->packet, &da->current_frame);
    if (ret < 0 || (da->packet && da->packet->len == 0)) {
        talloc_free(da->packet);
        da->packet = NULL;
    }

    if (da->current_frame && !mp_audio_config_valid(da->current_frame)) {
        talloc_free(da->current_frame);
        da->current_frame = NULL;
    }

    da->current_state = AUDIO_OK;
    if (!da->current_frame) {
        da->current_state = AUDIO_EOF;
        if (had_packet)
            da->current_state = AUDIO_SKIP;
    }

    fix_audio_pts(da);
}

// Fetch an audio frame decoded with audio_work(). Returns one of:
//  AUDIO_OK:   *out_frame is set to a new image
//  AUDIO_WAIT: waiting for demuxer; will receive a wakeup signal
//  AUDIO_EOF:  end of file, no more frames to be expected
//  AUDIO_SKIP: dropped frame or something similar
int audio_get_frame(struct dec_audio *da, struct mp_audio **out_frame)
{
    *out_frame = NULL;
    if (da->current_frame) {
        *out_frame = da->current_frame;
        da->current_frame = NULL;
        return AUDIO_OK;
    }
    if (da->current_state == AUDIO_OK)
        return AUDIO_SKIP;
    return da->current_state;
}
