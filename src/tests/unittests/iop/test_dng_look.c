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

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#define dt_image_cache_get __wrap_dt_image_cache_get
#define dt_image_cache_read_release __wrap_dt_image_cache_read_release

#include "iop/dng_look.c"
#include "common/iop_order.h"

#undef dt_image_cache_get
#undef dt_image_cache_read_release

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_image_t *cached_image;
static gboolean cache_locked;

dt_image_t *__wrap_dt_image_cache_get(const dt_imgid_t imgid, const char mode)
{
  assert_non_null(cached_image);
  assert_int_equal(imgid, cached_image->id);
  assert_int_equal(mode, 'r');
  assert_false(cache_locked);
  cache_locked = TRUE;
  return cached_image;
}

void __wrap_dt_image_cache_read_release(const dt_image_t *img)
{
  assert_ptr_equal(img, cached_image);
  assert_true(cache_locked);
  cache_locked = FALSE;
}

static void test_interpolation_and_hue_wrap(void **state)
{
  float table[24];
  for(int v = 0; v < 2; v++)
    for(int h = 0; h < 2; h++)
      for(int s = 0; s < 2; s++)
      {
        const int index = 3 * ((v * 2 + h) * 2 + s);
        table[index] = 100.0f * v + 10.0f * h + s;
        table[index + 1] = 1.0f + s;
        table[index + 2] = 1.0f + v;
      }
  const dt_iop_dng_look_data_t d = { .hsm = table, .hue_div = 2, .sat_div = 2, .val_div = 2 };
  dt_aligned_pixel_t hsv = { 0.25f, 0.5f, 0.5f, 0.0f };
  dt_aligned_pixel_t correction;
  _lookup_hsm(&d, hsv, correction);
  assert_float_equal(correction[0], 55.5f, 1e-6f);
  assert_float_equal(correction[1], 1.5f, 1e-6f);
  assert_float_equal(correction[2], 1.5f, 1e-6f);

  hsv[0] = 0.875f;
  _lookup_hsm(&d, hsv, correction);
  assert_float_equal(correction[0], 53.0f, 1e-6f);
  hsv[0] = -0.125f;
  _lookup_hsm(&d, hsv, correction);
  assert_float_equal(correction[0], 53.0f, 1e-6f);

  hsv[0] = 1.0f;
  hsv[1] = 2.0f;
  hsv[2] = -1.0f;
  _lookup_hsm(&d, hsv, correction);
  assert_float_equal(correction[0], 1.0f, 1e-6f);
}

static void test_full_saturation_and_unchanged_value(void **state)
{
  float table[] = { 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
  dt_iop_dng_look_data_t d = { .hsm = table, .hue_div = 1, .sat_div = 2, .val_div = 1 };
  dt_iop_module_t module = { 0 };
  dt_iop_order_iccprofile_info_t profile = { .type = DT_COLORSPACE_FORWARD_MATRIX };
  dt_dev_pixelpipe_t pipe = { .input_profile_info = &profile };
  dt_dev_pixelpipe_iop_t piece = { .data = &d, .colors = 4, .pipe = &pipe };
  const dt_iop_roi_t roi = { .width = 1, .height = 1, .scale = 1.0f };
  const float DT_ALIGNED_ARRAY colored[] = { 0.5f, 0.25f, 0.25f, 1.0f };
  const float DT_ALIGNED_ARRAY gray[] = { 0.4f, 0.4f, 0.4f, 1.0f };
  float DT_ALIGNED_ARRAY out[4];
  const float sat_scales[] = { -0.1f, 1.0f, 1.3f, 1.31f, 2.0f };
  const float expected_green[] = { 0.5f, 0.25f, 0.175f, 0.1725f, 0.0f };
  const float val_scales[] = { -0.1f, 1.0f, 1.2f, 1.21f, 2.0f };
  for(int i = 0; i < 5; i++)
  {
    table[1] = table[4] = sat_scales[i];
    table[2] = table[5] = 1.0f;
    process(&module, &piece, colored, out, &roi, &roi);
    assert_float_equal(out[1], expected_green[i], 1e-6f);
    table[1] = table[4] = 1.0f;
    table[2] = table[5] = val_scales[i];
    process(&module, &piece, gray, out, &roi, &roi);
    assert_float_equal(out[0], gray[0], 1e-6f);
  }
  const float DT_ALIGNED_ARRAY bright[] = { 0.9f, 0.9f, 0.9f, 1.0f };
  process(&module, &piece, bright, out, &roi, &roi);
  assert_float_equal(out[0], bright[0], 1e-6f);
}

static void test_processing(void **state)
{
  float table[] = { 120.0f, 0.5f, 0.5f, 120.0f, 0.5f, 0.5f };
  dt_iop_dng_look_data_t d = { .hsm = table, .hue_div = 1, .sat_div = 2, .val_div = 1 };
  dt_iop_module_t module = { 0 };
  dt_iop_order_iccprofile_info_t profile = { .type = DT_COLORSPACE_FORWARD_MATRIX };
  dt_dev_pixelpipe_t pipe = { .input_profile_info = &profile };
  dt_dev_pixelpipe_iop_t piece = { .data = &d, .colors = 4, .pipe = &pipe };
  const dt_iop_roi_t roi = { .width = 1, .height = 2, .scale = 1.0f };
  const float DT_ALIGNED_ARRAY in[] = { 1.0f, 0.0f, 0.0f, 0.37f, 0.4f, 0.4f, 0.4f, 0.81f };
  float DT_ALIGNED_ARRAY out[8];
  process(&module, &piece, in, out, &roi, &roi);
  assert_float_equal(out[0], 0.5f, 1e-6f);
  assert_float_equal(out[1], 1.0f, 1e-6f);
  assert_float_equal(out[3], in[3], 1e-6f);
  assert_float_equal(out[4], 0.4f, 1e-6f);
  assert_float_equal(out[7], in[7], 1e-6f);

  profile.type = DT_COLORSPACE_DNG_LOOK;
  process(&module, &piece, in, out, &roi, &roi);
  assert_float_equal(out[0], 0.5f, 1e-6f);
  assert_float_equal(out[1], 1.0f, 1e-6f);
  assert_float_equal(out[3], in[3], 1e-6f);

  profile.type = DT_COLORSPACE_EMBEDDED_MATRIX;
  process(&module, &piece, in, out, &roi, &roi);
  assert_memory_equal(out, in, sizeof(in));
  pipe.input_profile_info = NULL;
  process(&module, &piece, in, out, &roi, &roi);
  assert_memory_equal(out, in, sizeof(in));
  profile.type = DT_COLORSPACE_FORWARD_MATRIX;
  pipe.input_profile_info = &profile;

  d.hsm = NULL;
  process(&module, &piece, in, out, &roi, &roi);
  assert_memory_equal(out, in, sizeof(in));
}

static void test_commit_and_defaults(void **state)
{
  dt_develop_t dev = { 0 };
  dt_iop_module_t module = { .dev = &dev };
  dt_dev_pixelpipe_t pipe = { 0 };
  dt_dev_pixelpipe_iop_t piece = { .colors = 4 };
  init_pipe(&module, &pipe, &piece);
  dt_iop_dng_look_data_t *d = piece.data;
  reload_defaults(&module);
  assert_false(module.default_enabled);

  float table[] = { 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
  dev.image_storage.profile_hsm_data = table;
  dev.image_storage.profile_hsm_hue_div = 1;
  dev.image_storage.profile_hsm_sat_div = 2;
  dev.image_storage.profile_hsm_val_div = 1;
  reload_defaults(&module);
  assert_false(module.default_enabled);
  commit_params(&module, NULL, &pipe, &piece);
  assert_memory_equal(d->hsm, table, sizeof(table));
  assert_false(piece.process_cl_ready);
  table[0] = 60.0f;
  assert_float_equal(d->hsm[0], 0.0f, 1e-6f);

  dev.image_storage.profile_hsm_hue_div = G_MAXINT;
  commit_params(&module, NULL, &pipe, &piece);
  assert_null(d->hsm);
  dev.image_storage.profile_hsm_hue_div = 1;
  table[1] = NAN;
  commit_params(&module, NULL, &pipe, &piece);
  assert_null(d->hsm);
  table[1] = -1.0f;
  commit_params(&module, NULL, &pipe, &piece);
  assert_null(d->hsm);

  dev.image_storage.profile_hsm_data = NULL;
  commit_params(&module, NULL, &pipe, &piece);
  cleanup_pipe(&module, &pipe, &piece);
}

static void *_colorin_get_p(const void *params, const char *name)
{
  assert_string_equal(name, "type");
  return (void *)params;
}

static void test_automatic_enablement(void **state)
{
  dt_colorspaces_color_profile_type_t type = DT_COLORSPACE_FORWARD_MATRIX;
  dt_colorspaces_color_profile_type_t stale_type = DT_COLORSPACE_EMBEDDED_MATRIX;
  dt_iop_module_so_t colorin_so = { .op = "colorin" };
  dt_iop_module_t colorin = { .so = &colorin_so, .get_p = _colorin_get_p,
                             .default_params = &type, .params = &stale_type,
                             .default_enabled = TRUE };
  dt_develop_t dev = { 0 };
  dev.iop = g_list_append(NULL, &colorin);
  dt_iop_dng_look_params_t defaults = { 0 };
  dt_iop_dng_look_params_t history_params = { 0 };
  dt_iop_module_t module = { .op = "dng_look", .dev = &dev, .default_params = &defaults };
  dt_dev_pixelpipe_t pipe = { 0 };
  dt_dev_pixelpipe_iop_t piece = { 0 };
  init_pipe(&module, &pipe, &piece);

  float table[] = { 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
  dev.image_storage.profile_hsm_hue_div = 1;
  dev.image_storage.profile_hsm_sat_div = 2;
  dev.image_storage.profile_hsm_val_div = 1;

  const dt_colorspaces_color_profile_type_t types[] =
    { DT_COLORSPACE_FORWARD_MATRIX, DT_COLORSPACE_DNG_LOOK,
      DT_COLORSPACE_EMBEDDED_MATRIX, DT_COLORSPACE_LIN_REC709 };
  for(int matrix = 0; matrix < 2; matrix++)
    for(int profile = 0; profile < (int)G_N_ELEMENTS(types); profile++)
      for(int data = 0; data < 2; data++)
      {
        if(matrix)
          dev.image_storage.dng_forward_matrix[0] = 1.0f;
        else
          dt_mark_colormatrix_invalid(&dev.image_storage.dng_forward_matrix[0]);
        type = types[profile];
        stale_type = type == DT_COLORSPACE_EMBEDDED_MATRIX
          ? DT_COLORSPACE_FORWARD_MATRIX : DT_COLORSPACE_EMBEDDED_MATRIX;
        dev.image_storage.profile_hsm_data = data & 1 ? table : NULL;
        module.default_enabled = TRUE;
        reload_defaults(&module);
        commit_params(&module, module.default_params, &pipe, &piece);
        assert_int_equal(piece.enabled,
                         ((matrix && type == DT_COLORSPACE_FORWARD_MATRIX)
                          || type == DT_COLORSPACE_DNG_LOOK) && data != 0);
      }

  type = DT_COLORSPACE_FORWARD_MATRIX;
  dt_colorspaces_color_profile_type_t embedded = DT_COLORSPACE_EMBEDDED_MATRIX;
  dt_dev_history_item_t hist1 = { .module = &colorin, .params = &embedded, .enabled = TRUE };
  dt_dev_history_item_t hist2 = { .module = &colorin, .params = &type, .enabled = TRUE };
  dev.history = g_list_append(NULL, &hist1);
  dev.history = g_list_append(dev.history, &hist2);
  dev.history_end = 1;
  commit_params(&module, module.default_params, &pipe, &piece);
  assert_false(piece.enabled);
  dev.history_end = 2;
  commit_params(&module, module.default_params, &pipe, &piece);
  assert_true(piece.enabled);
  hist2.enabled = FALSE;
  commit_params(&module, module.default_params, &pipe, &piece);
  assert_false(piece.enabled);
  hist2.enabled = TRUE;

  piece.enabled = FALSE;
  commit_params(&module, &history_params, &pipe, &piece);
  assert_false(piece.enabled);
  piece.enabled = TRUE;
  commit_params(&module, &history_params, &pipe, &piece);
  assert_true(piece.enabled);

  dev.module_filter_out = g_list_append(NULL, "exposure");
  commit_params(&module, module.default_params, &pipe, &piece);
  assert_true(piece.enabled);
  dev.module_filter_out = g_list_append(dev.module_filter_out, "dng_look");
  commit_params(&module, module.default_params, &pipe, &piece);
  assert_false(piece.enabled);
  g_list_free(dev.module_filter_out);
  dev.module_filter_out = g_list_append(NULL, "colorin");
  commit_params(&module, module.default_params, &pipe, &piece);
  assert_false(piece.enabled);
  g_list_free(dev.module_filter_out);
  dev.module_filter_out = NULL;

  colorin.get_p = NULL;
  commit_params(&module, module.default_params, &pipe, &piece);
  assert_false(piece.enabled);
  g_list_free(dev.iop);
  dev.iop = NULL;
  commit_params(&module, module.default_params, &pipe, &piece);
  assert_false(piece.enabled);

  assert_true(flags() & IOP_FLAGS_HIDDEN);
  cleanup_pipe(&module, &pipe, &piece);
  g_list_free(dev.history);
}

static void test_profile_switch_tracking(void **state)
{
  dt_colorspaces_color_profile_type_t default_type = DT_COLORSPACE_EMBEDDED_MATRIX;
  dt_colorspaces_color_profile_type_t type = DT_COLORSPACE_FORWARD_MATRIX;
  dt_iop_module_so_t colorin_so = { .op = "colorin" };
  dt_iop_module_t colorin = { .so = &colorin_so, .get_p = _colorin_get_p,
                             .default_params = &default_type, .default_enabled = TRUE };
  dt_develop_t dev = { 0 };
  dev.iop = g_list_append(NULL, &colorin);
  dt_iop_dng_look_params_t defaults = { 0 };
  dt_iop_dng_look_params_t history_params = { 0 };
  dt_iop_module_t module = { .op = "dng_look", .dev = &dev, .default_params = &defaults };
  dt_dev_pixelpipe_t pipe = { 0 };
  dt_dev_pixelpipe_iop_t piece = { 0 };
  init_pipe(&module, &pipe, &piece);
  reload_defaults(&module);

  float table[] = { 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
  dev.image_storage.profile_hsm_hue_div = 1;
  dev.image_storage.profile_hsm_sat_div = 2;
  dev.image_storage.profile_hsm_val_div = 1;

  // the combobox rewrites colorin's top history item in place
  dt_dev_history_item_t colorin_hist = { .module = &colorin, .params = &type, .enabled = TRUE };
  dev.history = g_list_append(NULL, &colorin_hist);
  dev.history_end = 1;
  dt_dev_history_item_t look_hist = { .module = &module, .params = &history_params, .enabled = TRUE };
  const dt_colorspaces_color_profile_type_t switches[] =
    { DT_COLORSPACE_FORWARD_MATRIX, DT_COLORSPACE_EMBEDDED_MATRIX, DT_COLORSPACE_DNG_LOOK,
      DT_COLORSPACE_STANDARD_MATRIX, DT_COLORSPACE_DNG_LOOK, DT_COLORSPACE_FORWARD_MATRIX,
      DT_COLORSPACE_DNG_LOOK, DT_COLORSPACE_EMBEDDED_MATRIX, DT_COLORSPACE_FORWARD_MATRIX };

  for(int own_history = 0; own_history < 2; own_history++)
  {
    if(own_history)
    {
      dev.history = g_list_append(dev.history, &look_hist);
      dev.history_end = 2;
    }
    for(int matrix = 0; matrix < 2; matrix++)
    {
      if(matrix)
        dev.image_storage.dng_forward_matrix[0] = 1.0f;
      else
        dt_mark_colormatrix_invalid(&dev.image_storage.dng_forward_matrix[0]);
      for(int i = 0; i < (int)G_N_ELEMENTS(switches); i++)
      {
        type = switches[i];
        const gboolean expected = (matrix && type == DT_COLORSPACE_FORWARD_MATRIX)
                                  || type == DT_COLORSPACE_DNG_LOOK;
        dt_iop_params_t *params = own_history ? look_hist.params : module.default_params;

        // missing data disables the piece, but a full sync must recover once data is present
        dev.image_storage.profile_hsm_data = NULL;
        piece.enabled = own_history ? look_hist.enabled : module.default_enabled;
        commit_params(&module, params, &pipe, &piece);
        assert_false(piece.enabled);
        dev.image_storage.profile_hsm_data = table;
        for(int previous_enabled = 0; previous_enabled < 2; previous_enabled++)
        {
          piece.enabled = previous_enabled;
          // history replay restores the user's enabled flag before committing the piece
          if(own_history) piece.enabled = look_hist.enabled;
          commit_params(&module, params, &pipe, &piece);
          assert_int_equal(piece.enabled, expected);
          commit_params(&module, params, &pipe, &piece);
          assert_int_equal(piece.enabled, expected);
        }
      }
    }
  }

  // an explicit disable in the look's history wins for both look-capable profiles
  dev.image_storage.dng_forward_matrix[0] = 1.0f;
  for(int i = 0; i < 2; i++)
  {
    type = i ? DT_COLORSPACE_DNG_LOOK : DT_COLORSPACE_FORWARD_MATRIX;
    for(int enabled = 0; enabled < 2; enabled++)
    {
      look_hist.enabled = enabled;
      piece.enabled = look_hist.enabled;
      commit_params(&module, look_hist.params, &pipe, &piece);
      assert_int_equal(piece.enabled, enabled);
    }
  }

  cleanup_pipe(&module, &pipe, &piece);
  g_list_free(dev.history);
  g_list_free(dev.iop);
}

static void test_module_order(void **state)
{
  for(int version = DT_IOP_ORDER_LEGACY; version < DT_IOP_ORDER_LAST; version++)
  {
    GList *list = dt_ioppr_get_iop_order_list_version(version);
    gboolean found = FALSE;
    for(const GList *l = list; l; l = l->next)
    {
      const dt_iop_order_entry_t *entry = l->data;
      if(!strcmp(entry->operation, "colorin"))
      {
        assert_non_null(l->next);
        entry = l->next->data;
        assert_string_equal(entry->operation, "dng_look");
        found = TRUE;
        break;
      }
    }
    assert_true(found);
    g_list_free_full(list, free);
  }
}

static void test_cache_snapshot(void **state)
{
  float table[] = { 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
  dt_image_t image = { .id = 42, .profile_hsm_data = table,
                      .profile_hsm_hue_div = 1, .profile_hsm_sat_div = 2, .profile_hsm_val_div = 1 };
  dt_develop_t dev = { 0 };
  dev.image_storage.id = image.id;
  dt_iop_dng_look_params_t params = { 0 };
  dt_iop_module_t module = { .dev = &dev };
  dt_dev_pixelpipe_t pipe = { 0 };
  dt_dev_pixelpipe_iop_t piece = { .colors = 4 };
  cached_image = &image;
  cache_locked = FALSE;
  init_pipe(&module, &pipe, &piece);
  commit_params(&module, &params, &pipe, &piece);
  dt_iop_dng_look_data_t *d = piece.data;
  assert_false(cache_locked);
  assert_memory_equal(d->hsm, table, sizeof(table));
  table[0] = 90.0f;
  assert_float_equal(d->hsm[0], 0.0f, 1e-6f);
  cleanup_pipe(&module, &pipe, &piece);
  cached_image = NULL;
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_interpolation_and_hue_wrap),
    cmocka_unit_test(test_full_saturation_and_unchanged_value),
    cmocka_unit_test(test_processing),
    cmocka_unit_test(test_commit_and_defaults),
    cmocka_unit_test(test_automatic_enablement),
    cmocka_unit_test(test_profile_switch_tracking),
    cmocka_unit_test(test_module_order),
    cmocka_unit_test(test_cache_snapshot),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
