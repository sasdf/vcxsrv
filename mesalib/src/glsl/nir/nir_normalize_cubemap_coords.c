/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jason Ekstrand <jason@jlekstrand.net>
 */

#include "nir.h"
#include "nir_builder.h"

/**
 * This file implements a NIR lowering pass to perform the normalization of
 * the cubemap coordinates to have the largest magnitude component be -1.0
 * or 1.0.  This is based on the old GLSL IR based pass by Eric.
 */

struct normalize_cubemap_state {
   nir_builder b;
   bool progress;
};

static bool
normalize_cubemap_coords_block(nir_block *block, void *void_state)
{
   struct normalize_cubemap_state *state = void_state;
   nir_builder *b = &state->b;

   nir_foreach_instr(block, instr) {
      if (instr->type != nir_instr_type_tex)
         continue;

      nir_tex_instr *tex = nir_instr_as_tex(instr);
      if (tex->sampler_dim != GLSL_SAMPLER_DIM_CUBE)
         continue;

      b->cursor = nir_before_instr(&tex->instr);

      for (unsigned i = 0; i < tex->num_srcs; i++) {
         if (tex->src[i].src_type != nir_tex_src_coord)
            continue;

         nir_ssa_def *orig_coord =
            nir_ssa_for_src(b, tex->src[i].src, nir_tex_instr_src_size(tex, i));
         assert(orig_coord->num_components >= 3);

         nir_ssa_def *abs = nir_fabs(b, orig_coord);
         nir_ssa_def *norm = nir_fmax(b, nir_channel(b, abs, 0),
                                         nir_fmax(b, nir_channel(b, abs, 1),
                                                     nir_channel(b, abs, 2)));

         nir_ssa_def *normalized = nir_fmul(b, orig_coord, nir_frcp(b, norm));

         /* Array indices don't have to be normalized, so make a new vector
          * with the coordinate's array index untouched.
          */
         if (tex->coord_components == 4) {
            normalized = nir_vec4(b,
                                  nir_channel(b, normalized, 0),
                                  nir_channel(b, normalized, 1),
                                  nir_channel(b, normalized, 2),
                                  nir_channel(b, orig_coord, 3));
         }

         nir_instr_rewrite_src(&tex->instr,
                               &tex->src[i].src,
                               nir_src_for_ssa(normalized));

         state->progress = true;
      }
   }

   return true;
}

static bool
normalize_cubemap_coords_impl(nir_function_impl *impl)
{
   struct normalize_cubemap_state state;
   nir_builder_init(&state.b, impl);
   state.progress = false;

   nir_foreach_block(impl, normalize_cubemap_coords_block, &state);

   nir_metadata_preserve(impl, nir_metadata_block_index |
                               nir_metadata_dominance);

   return state.progress;
}

bool
nir_normalize_cubemap_coords(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         progress = normalize_cubemap_coords_impl(overload->impl) || progress;
   }

   return progress;
}
