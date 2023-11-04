#include "GlowRenderer.h"

#include "third-party/imgui/imgui.h"

/*
 * The glow renderer draws a sprite, but only if the center of the sprite is "visible".
 * To determine visibility, we draw a test "probe" at the center of the sprite and see how many
 * pixels of the probe were visible.
 *
 * The inputs to this renderer are the transformed vertices that would be computed on VU1.
 * The convention is that float -> int conversions and scalings are dropped.
 *
 * To detect if this is visible, we do something a little different from the original game:
 * - first copy the depth buffer to a separate texture. It seems like we could eliminate this copy
 *   eventually and always render to a texture.
 *
 * - For each sprite draw a "test probe" using this depth buffer. Write to a separate texture.
 *   This test probe is pretty small. First clear alpha to 0, then draw a test image with alpha = 1
 *   and depth testing on.
 *
 * - Repeatedly sample the result of the probe as a texture and draw it to another texture with half
 *   the size. This effectively averages the alpha values with texture filtering.
 *
 * - Stop once we've reached 2x2. At this point we can sample the center of this "texture" and the
 *   alpha will indicate the fraction of pixels in the original probe that passed depth. Use this
 *   to scale the intensity of the actual sprite draw.
 *
 * There are a number of optimizations made at this point:
 * - Probe clear/drawing are batched.
 * - Instead of doing the "downsampling" one-at-a-time, all probes are copied to a big grid and
 *   the downsampling happens in batches.
 * - The sampling of the final downsampled texture happens inside the vertex shader. On PS2, it's
 *   used as a texture, drawn as alpha-only over the entire region, then blended with the final
 *   draw. But the alpha of the entire first draw is constant, and we can figure it out in the
 *   vertex shader, so there's no need to do this approach.
 *
 * there are a few remaining improvements that could be made:
 *   - The final draws could be bucketed, this would reduce draw calls by a lot.
 *   - The depth buffer copy could likely be eliminated.
 *   - There's a possibility that overlapping probes do the "wrong" thing. This could be solved by
 *     copying from the depth buffer to the grid, then drawing probes on the grid. Currently the
 *     probes are drawn aligned with the framebuffer, then copied back to the grid. This approach
 *     would also lower the vram needed.
 */

GlowRenderer::GlowRenderer() {
  m_vertex_buffer.resize(kMaxVertices);
  m_sprite_data_buffer.resize(kMaxSprites);
  m_index_buffer.resize(kMaxIndices);

  // dynamic buffer: this will hold vertices that are generated by the game and updated each frame.
  // the most important optimization here is to have as few uploads as possible. The size of the
  // upload isn't too important - we only have at most 256 sprites so the overhead of uploading
  // anything dominates. So we use a single vertex format for all of our draws.
  {
    glGenBuffers(1, &m_ogl.vertex_buffer);
    glGenVertexArrays(1, &m_ogl.vao);
    glBindVertexArray(m_ogl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
    auto bytes = kMaxVertices * sizeof(Vertex);
    glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,                          // location 0 in the shader
                          4,                          // 4 floats per vert
                          GL_FLOAT,                   // floats
                          GL_TRUE,                    // normalized, ignored,
                          sizeof(Vertex),             //
                          (void*)offsetof(Vertex, x)  // offset in array
    );

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,                          // location 1 in the shader
                          4,                          // 4 color components
                          GL_FLOAT,                   // floats
                          GL_TRUE,                    // normalized, ignored,
                          sizeof(Vertex),             //
                          (void*)offsetof(Vertex, r)  // offset in array
    );

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2,                          // location 2 in the shader
                          2,                          // 2 uv
                          GL_FLOAT,                   // floats
                          GL_TRUE,                    // normalized, ignored,
                          sizeof(Vertex),             //
                          (void*)offsetof(Vertex, u)  // offset in array (why is this a pointer...)
    );

    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3,                           // location 2 in the shader
                          2,                           // 2 uv
                          GL_FLOAT,                    // floats
                          GL_TRUE,                     // normalized, ignored,
                          sizeof(Vertex),              //
                          (void*)offsetof(Vertex, uu)  // offset in array (why is this a pointer...)
    );

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glGenBuffers(1, &m_ogl.index_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, kMaxIndices * sizeof(u32), nullptr, GL_STREAM_DRAW);
    glBindVertexArray(0);
  }

  // static buffer: this will hold draws for downsampling. Because everything is normalized, we can
  // reuse the same vertices for all downsampling!
  // Note that we can't do a single giant square - otherwise it would "bleed" over the edges.
  // the boundary between cells needs to be preserved.
  {
    glGenBuffers(1, &m_ogl_downsampler.vertex_buffer);
    glGenVertexArrays(1, &m_ogl_downsampler.vao);
    glBindVertexArray(m_ogl_downsampler.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_ogl_downsampler.vertex_buffer);
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    vertices.resize(kMaxSprites * 4);
    indices.resize(kMaxSprites * 5);
    for (int i = 0; i < kMaxSprites; i++) {
      int x = i / kDownsampleBatchWidth;
      int y = i % kDownsampleBatchWidth;
      float step = 1.f / kDownsampleBatchWidth;
      Vertex* vtx = &vertices.at(i * 4);
      for (int j = 0; j < 4; j++) {
        vtx[j].r = 0.f;  // debug
        vtx[j].g = 0.f;
        vtx[j].b = 0.f;
        vtx[j].a = 0.f;
        vtx[j].x = x * step;
        vtx[j].y = y * step;
        vtx[j].z = 0;
        vtx[j].w = 0;
      }
      vtx[1].x += step;
      vtx[2].y += step;
      vtx[3].x += step;
      vtx[3].y += step;
      indices.at(i * 5 + 0) = i * 4;
      indices.at(i * 5 + 1) = i * 4 + 1;
      indices.at(i * 5 + 2) = i * 4 + 2;
      indices.at(i * 5 + 3) = i * 4 + 3;
      indices.at(i * 5 + 4) = UINT32_MAX;
    }

    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,                          // location 0 in the shader
                          4,                          // 4 floats per vert
                          GL_FLOAT,                   // floats
                          GL_TRUE,                    // normalized, ignored,
                          sizeof(Vertex),             //
                          (void*)offsetof(Vertex, x)  // offset in array
    );

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,                          // location 1 in the shader
                          4,                          // 4 color components
                          GL_FLOAT,                   // floats
                          GL_TRUE,                    // normalized, ignored,
                          sizeof(Vertex),             //
                          (void*)offsetof(Vertex, r)  // offset in array
    );

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2,                          // location 2 in the shader
                          3,                          // 4 color components
                          GL_FLOAT,                   // floats
                          GL_TRUE,                    // normalized, ignored,
                          sizeof(Vertex),             //
                          (void*)offsetof(Vertex, u)  // offset in array (why is this a pointer...)
    );

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glGenBuffers(1, &m_ogl_downsampler.index_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl_downsampler.index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(u32), indices.data(),
                 GL_STATIC_DRAW);
    glBindVertexArray(0);
  }

  // probe fbo setup: this fbo will hold the result of drawing probes.
  glGenFramebuffers(1, &m_ogl.probe_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_ogl.probe_fbo);
  glGenTextures(1, &m_ogl.probe_fbo_rgba_tex);
  glGenTextures(1, &m_ogl.probe_fbo_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_ogl.probe_fbo_rgba_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_ogl.probe_fbo_w, m_ogl.probe_fbo_h, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         m_ogl.probe_fbo_rgba_tex, 0);

  glBindTexture(GL_TEXTURE_2D, m_ogl.probe_fbo_depth_tex);
  glTexImage2D(GL_TEXTURE_2D,        // target
               0,                    // level
               GL_DEPTH24_STENCIL8,  // internalformat
               m_ogl.probe_fbo_w,    // width
               m_ogl.probe_fbo_h,    // height
               0,                    // border
               GL_DEPTH_STENCIL,     // format
               GL_UNSIGNED_INT_24_8, NULL);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                         m_ogl.probe_fbo_depth_tex, 0);

  GLenum render_targets[1] = {GL_COLOR_ATTACHMENT0};
  glDrawBuffers(1, render_targets);
  auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  ASSERT(status == GL_FRAMEBUFFER_COMPLETE);

  // downsample fbo setup: each will hold a grid of probes.
  // there's one fbo for each size.
  int ds_size = kFirstDownsampleSize;
  for (int i = 0; i < kDownsampleIterations; i++) {
    m_ogl.downsample_fbos[i].size = ds_size * kDownsampleBatchWidth;
    glGenFramebuffers(1, &m_ogl.downsample_fbos[i].fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ogl.downsample_fbos[i].fbo);
    glGenTextures(1, &m_ogl.downsample_fbos[i].tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ogl.downsample_fbos[i].tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ds_size * kDownsampleBatchWidth,
                 ds_size * kDownsampleBatchWidth, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    if (i == 0) {
      glGenRenderbuffers(1, &m_ogl.first_ds_depth_rb);
      glBindRenderbuffer(GL_RENDERBUFFER, m_ogl.first_ds_depth_rb);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, ds_size * kDownsampleBatchWidth,
                            ds_size * kDownsampleBatchWidth);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                m_ogl.first_ds_depth_rb);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_ogl.downsample_fbos[i].tex, 0);
    glDrawBuffers(1, render_targets);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    ASSERT(status == GL_FRAMEBUFFER_COMPLETE);
    ds_size /= 2;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // set up a default draw mode for sprites. If they don't set values, they will get this
  // from the giftag
  m_default_draw_mode.set_ab(true);

  //  ;; (new 'static 'gs-test :ate 1 :afail 1 :zte 1 :ztst 2)
  //  (new 'static 'gs-adcmd :cmds (gs-reg64 test-1) :x #x51001)
  m_default_draw_mode.set_at(true);
  m_default_draw_mode.set_alpha_fail(GsTest::AlphaFail::FB_ONLY);
  m_default_draw_mode.set_zt(true);
  m_default_draw_mode.set_depth_test(GsTest::ZTest::GEQUAL);
  m_default_draw_mode.set_alpha_test(DrawMode::AlphaTest::NEVER);

  //  ;; (new 'static 'gs-zbuf :zbp 304 :psm 1 :zmsk 1)
  //  (new 'static 'gs-adcmd :cmds (gs-reg64 zbuf-1) :x #x1000130 :y #x1)
  m_default_draw_mode.disable_depth_write();

  glGenTextures(1, &m_ogl.depth_texture);
}

namespace {
void copy_to_vertex(GlowRenderer::Vertex* vtx, const Vector4f& xyzw) {
  vtx->x = xyzw.x();
  vtx->y = xyzw.y();
  vtx->z = xyzw.z();
  // ignore the w computed by the game, and just use 1. The game's VU program ignores this value,
  // and we need to set w = 1 to get the correct opengl clipping behavior
  vtx->w = 1;
}
}  // namespace

SpriteGlowOutput* GlowRenderer::alloc_sprite() {
  ASSERT(m_next_sprite < m_sprite_data_buffer.size());
  return &m_sprite_data_buffer[m_next_sprite++];
}

void GlowRenderer::cancel_sprite() {
  ASSERT(m_next_sprite);
  m_next_sprite--;
}

// vertex addition is done in passes, so the "pass1" for all sprites is before any "pass2" vertices.
// But, we still do a single big upload for all passes.
// this way pass1 can be a single giant draw.

/*!
 * Add pass 1 vertices, for drawing the probe.
 */
void GlowRenderer::add_sprite_pass_1(const SpriteGlowOutput& data) {
  {  // first draw is a GS sprite to clear the alpha. This is faster than glClear, and the game
     // computes these for us and gives it a large z that always passes.
     // We need to convert to triangle strip.
    u32 idx_start = m_next_vertex;
    Vertex* vtx = alloc_vtx(4);
    for (int i = 0; i < 4; i++) {
      vtx[i].r = 1.f;  // red for debug
      vtx[i].g = 0.f;
      vtx[i].b = 0.f;
      vtx[i].a = 0.f;  // clearing alpha
    }
    copy_to_vertex(vtx, data.first_clear_pos[0]);
    copy_to_vertex(vtx + 1, data.first_clear_pos[0]);
    vtx[1].x = data.first_clear_pos[1].x();
    copy_to_vertex(vtx + 2, data.first_clear_pos[0]);
    vtx[2].y = data.first_clear_pos[1].y();
    copy_to_vertex(vtx + 3, data.first_clear_pos[1]);

    u32* idx = alloc_index(5);
    idx[0] = idx_start;
    idx[1] = idx_start + 1;
    idx[2] = idx_start + 2;
    idx[3] = idx_start + 3;
    idx[4] = UINT32_MAX;
  }

  {  // second draw is the actual probe, using the real Z, and setting alpha to 1.
    u32 idx_start = m_next_vertex;
    Vertex* vtx = alloc_vtx(4);
    for (int i = 0; i < 4; i++) {
      vtx[i].r = 0.f;
      vtx[i].g = 1.f;  // green for debug
      vtx[i].b = 0.f;
      vtx[i].a = 1.f;  // setting alpha
    }
    copy_to_vertex(vtx, data.second_clear_pos[0]);
    copy_to_vertex(vtx + 1, data.second_clear_pos[0]);
    vtx[1].x = data.second_clear_pos[1].x();
    copy_to_vertex(vtx + 2, data.second_clear_pos[0]);
    vtx[2].y = data.second_clear_pos[1].y();
    copy_to_vertex(vtx + 3, data.second_clear_pos[1]);

    u32* idx = alloc_index(5);
    idx[0] = idx_start;
    idx[1] = idx_start + 1;
    idx[2] = idx_start + 2;
    idx[3] = idx_start + 3;
    idx[4] = UINT32_MAX;
  }
}

/*!
 * Add pass 2 vertices, for copying from the probe fbo to the biggest grid.
 */
void GlowRenderer::add_sprite_pass_2(const SpriteGlowOutput& data, int sprite_idx) {
  // output is a grid of kBatchWidth * kBatchWidth.
  // for simplicity, we'll map to (0, 1) here, and the shader will convert to (-1, 1) for opengl.
  int x = sprite_idx / kDownsampleBatchWidth;
  int y = sprite_idx % kDownsampleBatchWidth;
  float step = 1.f / kDownsampleBatchWidth;

  u32 idx_start = m_next_vertex;
  Vertex* vtx = alloc_vtx(4);
  for (int i = 0; i < 4; i++) {
    vtx[i].r = 1.f;  // debug
    vtx[i].g = 0.f;
    vtx[i].b = 0.f;
    vtx[i].a = 0.f;
    vtx[i].x = x * step;  // start of our cell
    vtx[i].y = y * step;
    vtx[i].z = 0;
    vtx[i].w = 0;
  }
  vtx[1].x += step;
  vtx[2].y += step;
  vtx[3].x += step;
  vtx[3].y += step;

  // transformation code gives us these coordinates for where to sample probe fbo
  vtx[0].u = data.offscreen_uv[0][0];
  vtx[0].v = data.offscreen_uv[0][1];
  vtx[1].u = data.offscreen_uv[1][0];
  vtx[1].v = data.offscreen_uv[0][1];
  vtx[2].u = data.offscreen_uv[0][0];
  vtx[2].v = data.offscreen_uv[1][1];
  vtx[3].u = data.offscreen_uv[1][0];
  vtx[3].v = data.offscreen_uv[1][1];

  u32* idx = alloc_index(5);
  idx[0] = idx_start;
  idx[1] = idx_start + 1;
  idx[2] = idx_start + 2;
  idx[3] = idx_start + 3;
  idx[4] = UINT32_MAX;
}

void GlowRenderer::add_sprite_new(const SpriteGlowOutput& data, int sprite_idx) {
  // output is a grid of kBatchWidth * kBatchWidth.
  // for simplicity, we'll map to (0, 1) here, and the shader will convert to (-1, 1) for opengl.
  int x = sprite_idx / kDownsampleBatchWidth;
  int y = sprite_idx % kDownsampleBatchWidth;
  float step = 1.f / kDownsampleBatchWidth;

  u32 idx_start = m_next_vertex;
  Vertex* vtx = alloc_vtx(4);
  for (int i = 0; i < 4; i++) {
    vtx[i].r = 1.f;  // debug
    vtx[i].g = 0.f;
    vtx[i].b = 0.f;
    vtx[i].a = 0.f;
    vtx[i].x = x * step;  // start of our cell
    vtx[i].y = y * step;
    vtx[i].z = data.second_clear_pos[0].z();
    vtx[i].w = 0;
  }
  vtx[1].x += step;
  vtx[2].y += step;
  vtx[3].x += step;
  vtx[3].y += step;

  // transformation code gives us these coordinates for where to sample probe fbo
  vtx[0].u = data.offscreen_uv[0][0];
  vtx[0].v = data.offscreen_uv[0][1];
  vtx[1].u = data.offscreen_uv[1][0];
  vtx[1].v = data.offscreen_uv[0][1];
  vtx[2].u = data.offscreen_uv[0][0];
  vtx[2].v = data.offscreen_uv[1][1];
  vtx[3].u = data.offscreen_uv[1][0];
  vtx[3].v = data.offscreen_uv[1][1];

  u32* idx = alloc_index(5);
  idx[0] = idx_start;
  idx[1] = idx_start + 1;
  idx[2] = idx_start + 2;
  idx[3] = idx_start + 3;
  idx[4] = UINT32_MAX;
}

/*!
 * Add pass 3 vertices and update sprite records. This is the final draw.
 */
void GlowRenderer::add_sprite_pass_3(const SpriteGlowOutput& data, int sprite_idx) {
  // figure out our cell, we'll need to read from this to see if we're visible or not.
  int x = sprite_idx / kDownsampleBatchWidth;
  int y = sprite_idx % kDownsampleBatchWidth;
  float step = 1.f / kDownsampleBatchWidth;

  u32 idx_start = m_next_vertex;
  Vertex* vtx = alloc_vtx(4);
  for (int i = 0; i < 4; i++) {
    // include the color, used by the shader
    vtx[i].r = data.flare_draw_color[0];
    vtx[i].g = data.flare_draw_color[1];
    vtx[i].b = data.flare_draw_color[2];
    vtx[i].a = data.flare_draw_color[3];
    copy_to_vertex(&vtx[i], data.flare_xyzw[i]);
    vtx[i].u = 0;
    vtx[i].v = 0;
    // where to sample from to see probe result
    // offset by step/2 to sample the middle.
    // we use 2x2 for the final resolution and sample the middle - should be the same as
    // going to a 1x1, but saves a draw.
    vtx[i].uu = x * step + step / 2;
    vtx[i].vv = y * step + step / 2;
  }
  // texture uv's hardcoded to corners
  vtx[1].u = 1;
  vtx[3].v = 1;
  vtx[2].u = 1;
  vtx[2].v = 1;

  // get a record
  auto& record = m_sprite_records[sprite_idx];
  record.draw_mode = m_default_draw_mode;
  record.tbp = 0;
  record.idx = m_next_index;

  u32* idx = alloc_index(5);
  // flip first two - fan -> strip
  idx[0] = idx_start + 1;
  idx[1] = idx_start + 0;
  idx[2] = idx_start + 2;
  idx[3] = idx_start + 3;
  idx[4] = UINT32_MAX;

  // handle adgif stuff
  {
    // don't check upper bits: ps2 GS ignores them and ND uses them as flags.
    ASSERT((u8)data.adgif.tex0_addr == (u8)GsRegisterAddress::TEX0_1);
    GsTex0 reg(data.adgif.tex0_data);
    record.tbp = reg.tbp0();
    record.draw_mode.set_tcc(reg.tcc());
    // shader is hardcoded for this right now.
    ASSERT(reg.tcc() == 1);
    ASSERT(reg.tfx() == GsTex0::TextureFunction::MODULATE);
  }

  {
    ASSERT((u8)data.adgif.tex1_addr == (u8)GsRegisterAddress::TEX1_1);
    GsTex1 reg(data.adgif.tex1_data);
    record.draw_mode.set_filt_enable(reg.mmag());
  }

  {
    ASSERT(data.adgif.mip_addr == (u32)GsRegisterAddress::MIPTBP1_1);
    // ignore
  }

  // clamp or zbuf
  if (GsRegisterAddress(data.adgif.clamp_addr) == GsRegisterAddress::ZBUF_1) {
    GsZbuf x(data.adgif.clamp_data);
    record.draw_mode.set_depth_write_enable(!x.zmsk());
  } else if (GsRegisterAddress(data.adgif.clamp_addr) == GsRegisterAddress::CLAMP_1) {
    u32 val = data.adgif.clamp_data;
    if (!(val == 0b101 || val == 0 || val == 1 || val == 0b100)) {
      ASSERT_MSG(false, fmt::format("clamp: 0x{:x}", val));
    }
    record.draw_mode.set_clamp_s_enable(val & 0b001);
    record.draw_mode.set_clamp_t_enable(val & 0b100);
  } else {
    ASSERT(false);
  }

  // alpha
  ASSERT(data.adgif.alpha_addr == (u32)GsRegisterAddress::ALPHA_1);  // ??

  // ;; a = 0, b = 2, c = 1, d = 1
  // Cv = (Cs - 0) * Ad + D
  // leaving out the multiply by Ad.
  record.draw_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_FIX_DST);
}

/*!
 * Blit the depth buffer from the default rendering buffer to the depth buffer of the probe fbo.
 */
void GlowRenderer::blit_depth(SharedRenderState* render_state) {
  if (m_ogl.probe_fbo_w != render_state->render_fb_w ||
      m_ogl.probe_fbo_h != render_state->render_fb_h) {
    m_ogl.probe_fbo_w = render_state->render_fb_w;
    m_ogl.probe_fbo_h = render_state->render_fb_h;

    glBindTexture(GL_TEXTURE_2D, m_ogl.probe_fbo_rgba_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_ogl.probe_fbo_w, m_ogl.probe_fbo_h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindTexture(GL_TEXTURE_2D, m_ogl.probe_fbo_depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, m_ogl.probe_fbo_w, m_ogl.probe_fbo_h, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_state->render_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ogl.probe_fbo);

  glBlitFramebuffer(0,                          // srcX0
                    0,                          // srcY0
                    render_state->render_fb_w,  // srcX1
                    render_state->render_fb_h,  // srcY1
                    0,                          // dstX0
                    0,                          // dstY0
                    m_ogl.probe_fbo_w,          // dstX1
                    m_ogl.probe_fbo_h,          // dstY1
                    GL_DEPTH_BUFFER_BIT,        // mask
                    GL_NEAREST                  // filter
  );
}

void GlowRenderer::draw_debug_window() {
  ImGui::Checkbox("Show Probes", &m_debug.show_probes);
  ImGui::Checkbox("Show Copy", &m_debug.show_probe_copies);
  ImGui::Checkbox("Enable Glow Boost", &m_debug.enable_glow_boost);
  ImGui::SliderFloat("Boost Glow", &m_debug.glow_boost, 0, 10);
  ImGui::Text("Count: %d", m_debug.num_sprites);
}

/*!
 * Do all downsample draws to make 2x2 textures.
 */
void GlowRenderer::downsample_chain(SharedRenderState* render_state,
                                    ScopedProfilerNode& prof,
                                    u32 num_sprites) {
  glBindVertexArray(m_ogl_downsampler.vao);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  GLint old_viewport[4];
  glGetIntegerv(GL_VIEWPORT, old_viewport);
  render_state->shaders[ShaderId::GLOW_PROBE_DOWNSAMPLE].activate();
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  for (int i = 0; i < kDownsampleIterations - 1; i++) {
    auto* source = &m_ogl.downsample_fbos[i];
    auto* dest = &m_ogl.downsample_fbos[i + 1];
    glBindTexture(GL_TEXTURE_2D, source->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, dest->fbo);
    glViewport(0, 0, dest->size, dest->size);
    prof.add_draw_call();
    prof.add_tri(num_sprites * 4);
    // the grid fill order is the same as the downsample order, so we don't need to do all cells
    // if we aren't using all sprites.
    glDrawElements(GL_TRIANGLE_STRIP, num_sprites * 5, GL_UNSIGNED_INT, nullptr);
  }
  glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
}

void GlowRenderer::setup_buffers_for_draws() {
  glBindFramebuffer(GL_FRAMEBUFFER, m_ogl.probe_fbo);
  glBindVertexArray(m_ogl.vao);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  // don't want to write to the depth buffer we just copied, just test against it.
  glDepthMask(GL_FALSE);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, m_next_vertex * sizeof(Vertex), m_vertex_buffer.data(),
               GL_STREAM_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_next_index * sizeof(u32), m_index_buffer.data(),
               GL_STREAM_DRAW);
}

/*!
 * Draw probes (including the clear) to the probe fbo. Also copies vertex/index buffer.
 */
void GlowRenderer::draw_probes(SharedRenderState* render_state,
                               ScopedProfilerNode& prof,
                               u32 idx_start,
                               u32 idx_end) {
  render_state->shaders[ShaderId::GLOW_PROBE].activate();
  GLint old_viewport[4];
  glGetIntegerv(GL_VIEWPORT, old_viewport);
  glViewport(0, 0, m_ogl.probe_fbo_w, m_ogl.probe_fbo_h);
  prof.add_draw_call();
  prof.add_tri(m_next_sprite * 4);
  glDisable(GL_BLEND);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);
  glDrawElements(GL_TRIANGLE_STRIP, idx_end - idx_start, GL_UNSIGNED_INT,
                 (void*)(idx_start * sizeof(u32)));
  glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
}

/*!
 * Draw red/green probes to the framebuffer for debugging.
 */
void GlowRenderer::debug_draw_probes(SharedRenderState* render_state,
                                     ScopedProfilerNode& prof,
                                     u32 idx_start,
                                     u32 idx_end) {
  prof.add_draw_call();
  prof.add_tri(m_next_sprite * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  glDrawElements(GL_TRIANGLE_STRIP, idx_end - idx_start, GL_UNSIGNED_INT,
                 (void*)(idx_start * sizeof(u32)));
}

/*!
 * Copy probes from probe fbo to grid.
 */
void GlowRenderer::draw_probe_copies(SharedRenderState* render_state,
                                     ScopedProfilerNode& prof,
                                     u32 idx_start,
                                     u32 idx_end) {
  // read probe from probe fbo, write it to the first downsample fbo
  GLint old_viewport[4];
  glGetIntegerv(GL_VIEWPORT, old_viewport);
  render_state->shaders[ShaderId::GLOW_PROBE_READ].activate();
  glBindFramebuffer(GL_FRAMEBUFFER, m_ogl.downsample_fbos[0].fbo);
  glDisable(GL_DEPTH_TEST);
  glBindTexture(GL_TEXTURE_2D, m_ogl.probe_fbo_rgba_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glViewport(0, 0, m_ogl.downsample_fbos[0].size, m_ogl.downsample_fbos[0].size);
  prof.add_draw_call();
  prof.add_tri(m_next_sprite * 2);
  glDrawElements(GL_TRIANGLE_STRIP, idx_end - idx_start, GL_UNSIGNED_INT,
                 (void*)(idx_start * sizeof(u32)));
  glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
}

/*!
 * Draw blue squares to show where probes and being copied from, to help debug texture offsets.
 */
void GlowRenderer::debug_draw_probe_copies(SharedRenderState* render_state,
                                           ScopedProfilerNode& prof,
                                           u32 idx_start,
                                           u32 idx_end) {
  render_state->shaders[ShaderId::GLOW_PROBE_READ_DEBUG].activate();
  prof.add_draw_call();
  prof.add_tri(m_next_sprite * 2);
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  glDrawElements(GL_TRIANGLE_STRIP, idx_end - idx_start, GL_UNSIGNED_INT,
                 (void*)(idx_start * sizeof(u32)));
}

void GlowRenderer::probe_and_copy_old(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  // generate vertex/index data for probes
  u32 probe_idx_start = m_next_index;
  for (u32 sidx = 0; sidx < m_next_sprite; sidx++) {
    add_sprite_pass_1(m_sprite_data_buffer[sidx]);
  }

  // generate vertex/index data for copy to downsample buffer
  u32 copy_idx_start = m_next_index;
  for (u32 sidx = 0; sidx < m_next_sprite; sidx++) {
    add_sprite_pass_2(m_sprite_data_buffer[sidx], sidx);
  }
  u32 copy_idx_end = m_next_index;

  // generate vertex/index data for framebuffer draws
  for (u32 sidx = 0; sidx < m_next_sprite; sidx++) {
    add_sprite_pass_3(m_sprite_data_buffer[sidx], sidx);
  }

  // draw probes
  setup_buffers_for_draws();
  draw_probes(render_state, prof, probe_idx_start, copy_idx_start);
  if (m_debug.show_probes) {
    debug_draw_probes(render_state, prof, probe_idx_start, copy_idx_start);
  }

  // copy probes
  draw_probe_copies(render_state, prof, copy_idx_start, copy_idx_end);
  if (m_debug.show_probe_copies) {
    debug_draw_probe_copies(render_state, prof, copy_idx_start, copy_idx_end);
  }
}

void GlowRenderer::probe_and_copy_new(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  u32 idx_start = m_next_index;
  for (u32 sidx = 0; sidx < m_next_sprite; sidx++) {
    add_sprite_new(m_sprite_data_buffer[sidx], sidx);
  }
  u32 idx_end = m_next_index;

  // generate vertex/index data for framebuffer draws
  for (u32 sidx = 0; sidx < m_next_sprite; sidx++) {
    add_sprite_pass_3(m_sprite_data_buffer[sidx], sidx);
  }

  // clear the grid.
  setup_buffers_for_draws();
  GLint old_viewport[4];
  glGetIntegerv(GL_VIEWPORT, old_viewport);
  render_state->shaders[ShaderId::GLOW_PROBE_READ].activate();
  glBindFramebuffer(GL_FRAMEBUFFER, m_ogl.downsample_fbos[0].fbo);
  glBindTexture(GL_TEXTURE_2D, m_ogl.probe_fbo_depth_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glViewport(0, 0, m_ogl.downsample_fbos[0].size, m_ogl.downsample_fbos[0].size);

  // TODO: can probably remove this clear
  glClearColor(0.0, 0.0, 0.0, 0.0);
  glClear(GL_COLOR_BUFFER_BIT);

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_ALWAYS);
  glDepthMask(GL_TRUE);

  render_state->shaders[ShaderId::GLOW_DEPTH_COPY].activate();
  prof.add_draw_call();
  prof.add_tri(m_next_sprite * 2);
  glDrawElements(GL_TRIANGLE_STRIP, idx_end - idx_start, GL_UNSIGNED_INT,
                 (void*)(idx_start * sizeof(u32)));

  render_state->shaders[ShaderId::GLOW_PROBE_ON_GRID].activate();
  glDepthFunc(GL_GREATER);
  prof.add_draw_call();
  prof.add_tri(m_next_sprite * 2);
  glDrawElements(GL_TRIANGLE_STRIP, idx_end - idx_start, GL_UNSIGNED_INT,
                 (void*)(idx_start * sizeof(u32)));

  glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
}

/*!
 * Draw all pending sprites.
 */
void GlowRenderer::flush(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  m_debug.num_sprites = m_next_sprite;
  if (!m_next_sprite) {
    // no sprites submitted.
    return;
  }

  // copy depth from framebuffer to a temporary buffer
  // (this is a bit wasteful)
  blit_depth(render_state);

  if (new_mode) {
    probe_and_copy_new(render_state, prof);
  } else {
    probe_and_copy_old(render_state, prof);
  }

  // downsample probes.
  downsample_chain(render_state, prof, m_next_sprite);

  draw_sprites(render_state, prof);

  m_next_vertex = 0;
  m_next_index = 0;
  m_next_sprite = 0;
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
}

/*!
 * Final drawing of sprites.
 */
void GlowRenderer::draw_sprites(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  glBindVertexArray(m_ogl.vao);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_ogl.downsample_fbos[kDownsampleIterations - 1].tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  render_state->shaders[ShaderId::GLOW_DRAW].activate();
  if (!m_debug.enable_glow_boost && Gfx::g_global_settings.target_fps > 60.0f) {
    // on higher framerates, more glow sprites are drawn, so we scale the boost a bit
    m_debug.glow_boost = 60.0f / Gfx::g_global_settings.target_fps;
  }
  glUniform1f(glGetUniformLocation(render_state->shaders[ShaderId::GLOW_DRAW].id(), "glow_boost"),
              m_debug.glow_boost);

  // on PS2's, it's enabled but all sprite z's are UINT24_MAX, so it always passes.
  // this z-override is done in VU1 code and we don't replicate it here.
  glDisable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);
  glEnable(GL_BLEND);
  // Cv = (Cs - 0) * Ad + D
  glBlendFunc(GL_ONE, GL_ONE);
  glBlendEquation(GL_FUNC_ADD);

  glDepthMask(GL_FALSE);

  for (u32 i = 0; i < m_next_sprite; i++) {
    const auto& record = m_sprite_records[i];
    auto tex = render_state->texture_pool->lookup(record.tbp);
    if (!tex) {
      fmt::print("Failed to find texture at {}, using random (glow)", record.tbp);
      tex = render_state->texture_pool->get_placeholder_texture();
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, *tex);
    if (record.draw_mode.get_clamp_s_enable()) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    }

    if (record.draw_mode.get_clamp_t_enable()) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    if (record.draw_mode.get_filt_enable()) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    prof.add_draw_call();
    prof.add_tri(2);
    glDrawElements(GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_INT, (void*)(record.idx * sizeof(u32)));
  }
  glEnable(GL_DEPTH_TEST);
}

GlowRenderer::Vertex* GlowRenderer::alloc_vtx(int num) {
  ASSERT(m_next_vertex + num <= m_vertex_buffer.size());
  auto* result = &m_vertex_buffer[m_next_vertex];
  m_next_vertex += num;
  return result;
}

u32* GlowRenderer::alloc_index(int num) {
  ASSERT(m_next_index + num <= m_index_buffer.size());
  auto* result = &m_index_buffer[m_next_index];
  m_next_index += num;
  return result;
}
