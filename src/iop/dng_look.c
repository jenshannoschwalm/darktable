/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/colorspaces_inline_conversions.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_dng_look_params_t)

#define DNG_LOOK_TONE_SAMPLES 1024

typedef struct dt_iop_dng_look_params_t
{
  int reserved; // $DEFAULT: 0
} dt_iop_dng_look_params_t;

typedef struct dt_iop_dng_look_data_t
{
  float *hsm;
  int hue_div, sat_div, val_div;
  gboolean has_tone_curve;
  float tone_curve[DNG_LOOK_TONE_SAMPLES];
} dt_iop_dng_look_data_t;

const char *name()
{
  return _("dng look");
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_TECHNICAL;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void reload_defaults(dt_iop_module_t *self)
{
  const dt_image_t *img = &self->dev->image_storage;
  // gate on embedded data, not colorin params: defaults and history commit in different orders
  self->default_enabled = img->profile_hsm_data != NULL || img->profile_tone_curve != NULL;
}

static gboolean _build_tone_curve(dt_iop_dng_look_data_t *d,
                                  const float *curve,
                                  const int points)
{
  if(!curve || points < 2 || points > G_MAXINT / 2)
    return FALSE;

  for(int i = 0; i < points; i++)
  {
    const float x = curve[2 * i];
    const float y = curve[2 * i + 1];
    if(!isfinite(x) || !isfinite(y) || x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f
       || (i > 0 && (x <= curve[2 * (i - 1)] || y < curve[2 * (i - 1) + 1])))
      return FALSE;
  }

  int segment = 0;
  for(int i = 0; i < DNG_LOOK_TONE_SAMPLES; i++)
  {
    const float x = (float)i / (DNG_LOOK_TONE_SAMPLES - 1);
    while(segment < points - 2 && x > curve[2 * (segment + 1)])
      segment++;
    const float x0 = curve[2 * segment];
    const float y0 = curve[2 * segment + 1];
    const float x1 = curve[2 * (segment + 1)];
    const float y1 = curve[2 * (segment + 1) + 1];
    const float t = CLAMP((x - x0) / (x1 - x0), 0.0f, 1.0f);
    d->tone_curve[i] = y0 + t * (y1 - y0);
  }
  return TRUE;
}

static void _lookup_hsm(const dt_iop_dng_look_data_t *d,
                        const dt_aligned_pixel_t hsv,
                        dt_aligned_pixel_t correction)
{
  const float h = (hsv[0] - floorf(hsv[0])) * d->hue_div;
  const float s = CLAMP(hsv[1], 0.0f, 1.0f) * (d->sat_div - 1);
  const float v = CLAMP(hsv[2], 0.0f, 1.0f) * (d->val_div - 1);
  const int hi[2] = { MIN((int)h, d->hue_div - 1),
                     (MIN((int)h, d->hue_div - 1) + 1) % d->hue_div };
  const int si[2] = { MIN((int)s, d->sat_div - 1),
                     MIN((int)s + 1, d->sat_div - 1) };
  const int vi[2] = { MIN((int)v, d->val_div - 1),
                     MIN((int)v + 1, d->val_div - 1) };
  const float hf = CLAMP(h - hi[0], 0.0f, 1.0f);
  const float sf = CLAMP(s - si[0], 0.0f, 1.0f);
  const float vf = CLAMP(v - vi[0], 0.0f, 1.0f);

  correction[0] = correction[1] = correction[2] = 0.0f;
  for(int z = 0; z < 2; z++)
    for(int y = 0; y < 2; y++)
      for(int x = 0; x < 2; x++)
      {
        // DNG stores saturation fastest, then hue, then value, with three floats per cell
        const size_t index = 3 * (((size_t)vi[z] * d->hue_div + hi[y]) * d->sat_div + si[x]);
        const float weight = (z ? vf : 1.0f - vf)
                           * (y ? hf : 1.0f - hf)
                           * (x ? sf : 1.0f - sf);
        for(int c = 0; c < 3; c++)
          correction[c] += weight * d->hsm[index + c];
      }
}

static float _apply_tone_curve(const dt_iop_dng_look_data_t *d, const float value)
{
  const float x = CLAMP(value, 0.0f, 1.0f) * (DNG_LOOK_TONE_SAMPLES - 1);
  const int i = MIN((int)x, DNG_LOOK_TONE_SAMPLES - 2);
  return d->tone_curve[i] + (x - i) * (d->tone_curve[i + 1] - d->tone_curve[i]);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_dng_look_data_t *d = piece->data;
  const dt_image_t *img = &self->dev->image_storage;
  g_free(d->hsm);
  d->hsm = NULL;
  d->hue_div = d->sat_div = d->val_div = 0;
  piece->process_cl_ready = FALSE;

  if(img->profile_hsm_data && img->profile_hsm_hue_div >= 1
     && img->profile_hsm_sat_div >= 2 && img->profile_hsm_val_div >= 1
     && img->profile_hsm_hue_div <= G_MAXINT / 3 / img->profile_hsm_sat_div / img->profile_hsm_val_div)
  {
    const size_t count = (size_t)img->profile_hsm_hue_div * img->profile_hsm_sat_div
                       * img->profile_hsm_val_div * 3;
    gboolean valid = TRUE;
    for(size_t i = 0; i < count; i++)
      if(!isfinite(img->profile_hsm_data[i])
         || (i % 3 != 0 && img->profile_hsm_data[i] < 0.0f))
      {
        valid = FALSE;
        break;
      }

    if(valid)
    {
      d->hsm = g_try_malloc_n(count, sizeof(float));
      if(d->hsm)
      {
        memcpy(d->hsm, img->profile_hsm_data, count * sizeof(float));
        d->hue_div = img->profile_hsm_hue_div;
        d->sat_div = img->profile_hsm_sat_div;
        d->val_div = img->profile_hsm_val_div;
      }
    }
  }
  d->has_tone_curve = _build_tone_curve(d, img->profile_tone_curve, img->profile_tone_curve_points);
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
    return;

  const dt_iop_dng_look_data_t *d = piece->data;
  if(!d->hsm && !d->has_tone_curve)
  {
    memcpy(ovoid, ivoid, (size_t)4 * roi_out->width * roi_out->height * sizeof(float));
    return;
  }

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = (const float *)ivoid + (size_t)4 * roi_in->width * j;
    float *out = (float *)ovoid + (size_t)4 * roi_out->width * j;
    for(int i = 0; i < roi_out->width; i++, in += 4, out += 4)
    {
      dt_aligned_pixel_t rgb = { in[0], in[1], in[2], in[3] };
      if(isfinite(rgb[0]) && isfinite(rgb[1]) && isfinite(rgb[2]))
      {
        if(d->hsm)
        {
          dt_aligned_pixel_t hsv;
          dt_aligned_pixel_t correction;
          dt_RGB_2_HSV(rgb, hsv);
          if(!isfinite(hsv[0]) || !isfinite(hsv[1]) || !isfinite(hsv[2]))
          {
            copy_pixel(out, rgb);
            continue;
          }
          _lookup_hsm(d, hsv, correction);
          hsv[0] += correction[0] / 360.0f;
          hsv[0] -= floorf(hsv[0]);
          hsv[1] = CLAMP(hsv[1] * correction[1], 0.0f, 1.0f);
          hsv[2] = CLAMP(hsv[2] * correction[2], 0.0f, 1.0f);
          dt_HSV_2_RGB(hsv, rgb);
        }
        if(d->has_tone_curve)
          for(int c = 0; c < 3; c++)
            rgb[c] = _apply_tone_curve(d, rgb[c]);
      }
      copy_pixel(out, rgb);
    }
  }
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_dng_look_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_dng_look_data_t *d = piece->data;
  g_free(d->hsm);
  free(d);
  piece->data = NULL;
}

void gui_init(dt_iop_module_t *self)
{
  self->widget = dt_gui_vbox(dt_ui_label_new(_("automatically derived from the embedded DNG profile")));
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
