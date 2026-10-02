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

#pragma once

#include "common/colorspaces_inline_conversions.h"

// for normalized Rec.709 RGB: pivot at 0.5, unity strengths are neutral
// saturation zero desaturates; alpha is unchanged
static inline void dt_iop_apply_contrast_saturation(dt_aligned_pixel_t rgb,
                                                   const float contrast,
                                                   const float saturation)
{
  for(int c = 0; c < 3; c++)
    rgb[c] = CLIP(0.5f + (rgb[c] - 0.5f) * contrast);

  const float luma = srgb_to_xyz_d65[3] * rgb[0]
                   + srgb_to_xyz_d65[4] * rgb[1]
                   + srgb_to_xyz_d65[5] * rgb[2];
  for(int c = 0; c < 3; c++)
    rgb[c] = CLIP(luma + (rgb[c] - luma) * saturation);
}
