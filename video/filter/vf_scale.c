/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>

#include <libswscale/swscale.h>

#include "config.h"
#include "core/mp_msg.h"
#include "core/options.h"

#include "video/img_format.h"
#include "video/mp_image.h"
#include "vf.h"
#include "video/fmt-conversion.h"
#include "compat/mpbswap.h"

#include "video/sws_utils.h"

#include "video/csputils.h"
#include "video/out/vo.h"

#include "core/m_option.h"

static struct vf_priv_s {
    int w, h;
    int cfg_w, cfg_h;
    int v_chr_drop;
    double param[2];
    struct mp_sws_context *sws;
    int noup;
    int accurate_rnd;
} const vf_priv_dflt = {
    0, 0,
    -1, -1,
    0,
    {SWS_PARAM_DEFAULT, SWS_PARAM_DEFAULT},
};

//===========================================================================//

static const unsigned int outfmt_list[] = {
// YUV:
    IMGFMT_444P,
    IMGFMT_444P16_LE,
    IMGFMT_444P16_BE,
    IMGFMT_444P14_LE,
    IMGFMT_444P14_BE,
    IMGFMT_444P12_LE,
    IMGFMT_444P12_BE,
    IMGFMT_444P10_LE,
    IMGFMT_444P10_BE,
    IMGFMT_444P9_LE,
    IMGFMT_444P9_BE,
    IMGFMT_422P,
    IMGFMT_422P16_LE,
    IMGFMT_422P16_BE,
    IMGFMT_422P14_LE,
    IMGFMT_422P14_BE,
    IMGFMT_422P12_LE,
    IMGFMT_422P12_BE,
    IMGFMT_422P10_LE,
    IMGFMT_422P10_BE,
    IMGFMT_422P9_LE,
    IMGFMT_422P9_BE,
    IMGFMT_420P,
    IMGFMT_420P16_LE,
    IMGFMT_420P16_BE,
    IMGFMT_420P14_LE,
    IMGFMT_420P14_BE,
    IMGFMT_420P12_LE,
    IMGFMT_420P12_BE,
    IMGFMT_420P10_LE,
    IMGFMT_420P10_BE,
    IMGFMT_420P9_LE,
    IMGFMT_420P9_BE,
    IMGFMT_420AP,
    IMGFMT_410P,
    IMGFMT_411P,
    IMGFMT_NV12,
    IMGFMT_NV21,
    IMGFMT_YUYV,
    IMGFMT_UYVY,
    IMGFMT_440P,
// RGB and grayscale (Y8 and Y800):
    IMGFMT_BGR32,
    IMGFMT_RGB32,
    IMGFMT_ABGR,
    IMGFMT_ARGB,
    IMGFMT_BGRA,
    IMGFMT_RGBA,
    IMGFMT_BGR24,
    IMGFMT_RGB24,
    IMGFMT_GBRP,
    IMGFMT_RGB48_LE,
    IMGFMT_RGB48_BE,
    IMGFMT_BGR16,
    IMGFMT_RGB16,
    IMGFMT_BGR15,
    IMGFMT_RGB15,
    IMGFMT_BGR12,
    IMGFMT_RGB12,
    IMGFMT_Y8,
    IMGFMT_BGR8,
    IMGFMT_RGB8,
    IMGFMT_BGR4,
    IMGFMT_RGB4,
    IMGFMT_RGB4_BYTE,
    IMGFMT_BGR4_BYTE,
    IMGFMT_MONO,
    IMGFMT_MONO_W,
    0
};

/**
 * A list of preferred conversions, in order of preference.
 * This should be used for conversions that e.g. involve no scaling
 * or to stop vf_scale from choosing a conversion that has no
 * fast assembler implementation.
 */
static int preferred_conversions[][2] = {
    {IMGFMT_YUYV, IMGFMT_UYVY},
    {IMGFMT_YUYV, IMGFMT_422P},
    {IMGFMT_UYVY, IMGFMT_YUYV},
    {IMGFMT_UYVY, IMGFMT_422P},
    {IMGFMT_422P, IMGFMT_YUYV},
    {IMGFMT_422P, IMGFMT_UYVY},
    {IMGFMT_420P10, IMGFMT_420P},
    {IMGFMT_GBRP, IMGFMT_BGR24},
    {IMGFMT_GBRP, IMGFMT_RGB24},
    {IMGFMT_GBRP, IMGFMT_BGR32},
    {IMGFMT_GBRP, IMGFMT_RGB32},
    {IMGFMT_PAL8, IMGFMT_BGR32},
    {IMGFMT_XYZ12, IMGFMT_RGB48},
    {0, 0}
};

static int check_outfmt(vf_instance_t *vf, int outfmt)
{
    enum AVPixelFormat pixfmt = imgfmt2pixfmt(outfmt);
    if (pixfmt == PIX_FMT_NONE || sws_isSupportedOutput(pixfmt) < 1)
        return 0;
    return vf_next_query_format(vf, outfmt);
}

static unsigned int find_best_out(vf_instance_t *vf, int in_format)
{
    unsigned int best = 0;
    int i = -1;
    int j = -1;
    int format = 0;

    // find the best outfmt:
    while (1) {
        int ret;
        if (j < 0) {
            format = in_format;
            j = 0;
        } else if (i < 0) {
            while (preferred_conversions[j][0] &&
                   preferred_conversions[j][0] != in_format)
                j++;
            format = preferred_conversions[j++][1];
            // switch to standard list
            if (!format)
                i = 0;
        }
        if (i >= 0)
            format = outfmt_list[i++];
        if (!format)
            break;

        ret = check_outfmt(vf, format);

        mp_msg(MSGT_VFILTER, MSGL_DBG2, "scale: query(%s) -> %d\n",
               vo_format_name(
                   format), ret & 3);
        if (ret & VFCAP_CSP_SUPPORTED_BY_HW) {
            best = format; // no conversion -> bingo!
            break;
        }
        if (ret & VFCAP_CSP_SUPPORTED && !best)
            best = format;  // best with conversion
    }
    if (!best) {
        // Try anything else. outfmt_list is just a list of preferred formats.
        for (int format = IMGFMT_START; format < IMGFMT_END; format++) {
            int ret = check_outfmt(vf, format);

            if (ret & VFCAP_CSP_SUPPORTED_BY_HW) {
                best = format; // no conversion -> bingo!
                break;
            }
            if (ret & VFCAP_CSP_SUPPORTED && !best)
                best = format;  // best with conversion
        }
    }
    return best;
}

static int reconfig(struct vf_instance *vf, struct mp_image_params *p, int flags)
{
    int width = p->w, height = p->h, d_width = p->d_w, d_height = p->d_h;
    unsigned int outfmt = p->imgfmt;
    unsigned int best = find_best_out(vf, outfmt);
    int round_w = 0, round_h = 0;
    struct mp_image_params input = *p;

    if (!best) {
        mp_msg(MSGT_VFILTER, MSGL_WARN,
               "SwScale: no supported outfmt found :(\n");
        return -1;
    }

    vf->next->query_format(vf->next, best);

    vf->priv->w = vf->priv->cfg_w;
    vf->priv->h = vf->priv->cfg_h;

    if (vf->priv->w <= -8) {
        vf->priv->w += 8;
        round_w = 1;
    }
    if (vf->priv->h <= -8) {
        vf->priv->h += 8;
        round_h = 1;
    }

    if (vf->priv->w < -3 || vf->priv->h < -3 ||
        (vf->priv->w < -1 && vf->priv->h < -1))
    {
        // TODO: establish a direct connection to the user's brain
        // and find out what the heck he thinks MPlayer should do
        // with this nonsense.
        mp_msg(MSGT_VFILTER, MSGL_ERR,
               "SwScale: EUSERBROKEN Check your parameters, they make no sense!\n");
        return -1;
    }

    if (vf->priv->w == -1)
        vf->priv->w = width;
    if (vf->priv->w == 0)
        vf->priv->w = d_width;

    if (vf->priv->h == -1)
        vf->priv->h = height;
    if (vf->priv->h == 0)
        vf->priv->h = d_height;

    if (vf->priv->w == -3)
        vf->priv->w = vf->priv->h * width / height;
    if (vf->priv->w == -2)
        vf->priv->w = vf->priv->h * d_width / d_height;

    if (vf->priv->h == -3)
        vf->priv->h = vf->priv->w * height / width;
    if (vf->priv->h == -2)
        vf->priv->h = vf->priv->w * d_height / d_width;

    if (round_w)
        vf->priv->w = ((vf->priv->w + 8) / 16) * 16;
    if (round_h)
        vf->priv->h = ((vf->priv->h + 8) / 16) * 16;

    // check for upscaling, now that all parameters had been applied
    if (vf->priv->noup) {
        if ((vf->priv->w > width) + (vf->priv->h > height) >= vf->priv->noup) {
            vf->priv->w = width;
            vf->priv->h = height;
        }
    }

    mp_msg(MSGT_VFILTER, MSGL_DBG2, "SwScale: scaling %dx%d %s to %dx%d %s  \n",
           width, height, vo_format_name(outfmt), vf->priv->w, vf->priv->h,
           vo_format_name(best));

    // Compute new d_width and d_height, preserving aspect
    // while ensuring that both are >= output size in pixels.
    if (vf->priv->h * d_width > vf->priv->w * d_height) {
        d_width = vf->priv->h * d_width / d_height;
        d_height = vf->priv->h;
    } else {
        d_height = vf->priv->w * d_height / d_width;
        d_width = vf->priv->w;
    }
    //d_width=d_width*vf->priv->w/width;
    //d_height=d_height*vf->priv->h/height;
    p->w = vf->priv->w;
    p->h = vf->priv->h;
    p->d_w = d_width;
    p->d_h = d_height;
    p->imgfmt = best;

    // Second-guess what libswscale is going to output and what not.
    // It depends what libswscale supports for in/output, and what makes sense.
    struct mp_imgfmt_desc s_fmt = mp_imgfmt_get_desc(input.imgfmt);
    struct mp_imgfmt_desc d_fmt = mp_imgfmt_get_desc(p->imgfmt);
    // keep colorspace settings if the data stays in yuv
    if (!(s_fmt.flags & MP_IMGFLAG_YUV) || !(d_fmt.flags & MP_IMGFLAG_YUV)) {
        p->colorspace = MP_CSP_AUTO;
        p->colorlevels = MP_CSP_LEVELS_AUTO;
    }
    mp_image_params_guess_csp(p);

    mp_sws_set_from_cmdline(vf->priv->sws);
    vf->priv->sws->flags |= vf->priv->v_chr_drop << SWS_SRC_V_CHR_DROP_SHIFT;
    vf->priv->sws->flags |= vf->priv->accurate_rnd * SWS_ACCURATE_RND;
    vf->priv->sws->src = input;
    vf->priv->sws->dst = *p;

    if (mp_sws_reinit(vf->priv->sws) < 0) {
        // error...
        mp_msg(MSGT_VFILTER, MSGL_WARN,
               "Couldn't init libswscale for this setup\n");
        return -1;
    }

    // In particular, fix up colorspace/levels if YUV<->RGB conversion is
    // performed.

    return vf_next_reconfig(vf, p, flags);
}

static struct mp_image *filter(struct vf_instance *vf, struct mp_image *mpi)
{
    struct mp_image *dmpi = vf_alloc_out_image(vf);
    mp_image_copy_attributes(dmpi, mpi);

    mp_sws_scale(vf->priv->sws, dmpi, mpi);

    talloc_free(mpi);
    return dmpi;
}

static int control(struct vf_instance *vf, int request, void *data)
{
    int r;
    vf_equalizer_t *eq;
    struct mp_sws_context *sws = vf->priv->sws;

    switch (request) {
    case VFCTRL_GET_EQUALIZER:
        eq = data;
        if (!strcmp(eq->item, "brightness"))
            eq->value =  ((sws->brightness * 100) + (1 << 15)) >> 16;
        else if (!strcmp(eq->item, "contrast"))
            eq->value = (((sws->contrast  * 100) + (1 << 15)) >> 16) - 100;
        else if (!strcmp(eq->item, "saturation"))
            eq->value = (((sws->saturation * 100) + (1 << 15)) >> 16) - 100;
        else
            break;
        return CONTROL_TRUE;
    case VFCTRL_SET_EQUALIZER:
        eq = data;
        if (!strcmp(eq->item, "brightness"))
            sws->brightness = ((eq->value << 16) + 50) / 100;
        else if (!strcmp(eq->item, "contrast"))
            sws->contrast   = (((eq->value + 100) << 16) + 50) / 100;
        else if (!strcmp(eq->item, "saturation"))
            sws->saturation = (((eq->value + 100) << 16) + 50) / 100;
        else
            break;

        r = mp_sws_reinit(sws);
        if (r < 0)
            break;

        return CONTROL_TRUE;
    default:
        break;
    }

    return vf_next_control(vf, request, data);
}

//===========================================================================//

static int query_format(struct vf_instance *vf, unsigned int fmt)
{
    if (!IMGFMT_IS_HWACCEL(fmt) && imgfmt2pixfmt(fmt) != PIX_FMT_NONE) {
        if (sws_isSupportedInput(imgfmt2pixfmt(fmt)) < 1)
            return 0;
        unsigned int best = find_best_out(vf, fmt);
        int flags;
        if (!best)
            return 0;            // no matching out-fmt
        flags = vf_next_query_format(vf, best);
        if (!(flags & (VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW)))
            return 0;
        if (fmt != best)
            flags &= ~VFCAP_CSP_SUPPORTED_BY_HW;
        return flags;
    }
    return 0;   // nomatching in-fmt
}

static void uninit(struct vf_instance *vf)
{
}

static int vf_open(vf_instance_t *vf, char *args)
{
    vf->reconfig = reconfig;
    vf->filter = filter;
    vf->query_format = query_format;
    vf->control = control;
    vf->uninit = uninit;
    vf->priv->sws = mp_sws_alloc(vf);
    vf->priv->sws->params[0] = vf->priv->param[0];
    vf->priv->sws->params[1] = vf->priv->param[1];

    mp_msg(MSGT_VFILTER, MSGL_V, "SwScale params: %d x %d (-1=no scaling)\n",
           vf->priv->cfg_w, vf->priv->cfg_h);

    return 1;
}

#define OPT_BASE_STRUCT struct vf_priv_s
static const m_option_t vf_opts_fields[] = {
    OPT_INT("w", cfg_w, M_OPT_MIN, .min = -11),
    OPT_INT("h", cfg_h, M_OPT_MIN, .min = -11),
    OPT_DOUBLE("param", param[0], M_OPT_RANGE, .min = 0.0, .max = 100.0),
    OPT_DOUBLE("param2", param[1], M_OPT_RANGE, .min = 0.0, .max = 100.0),
    OPT_INTRANGE("chr-drop", v_chr_drop, 0, 0, 3),
    OPT_INTRANGE("noup", noup, 0, 0, 2),
    OPT_FLAG("arnd", accurate_rnd, 0),
    {0}
};

const vf_info_t vf_info_scale = {
    "software scaling",
    "scale",
    "A'rpi",
    "",
    vf_open,
    .priv_size = sizeof(struct vf_priv_s),
    .priv_defaults = &vf_priv_dflt,
    .options = vf_opts_fields,
};

//===========================================================================//
