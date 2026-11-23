#include "ui.h"
#include "ring_glsl.h"
#include "gl.h"

#include <SDL3/SDL.h>
#include <stb_truetype.h>

/* Sizes baked at once (font and pixel size); the oldest goes when full. */
/* More than the sizes on screen at once: one that doesn't fit is made again
 * every time it's drawn, 10 ms each. */
#define ATLASES 24

/* What's baked: Latin-1, dashes and quotes, the trademark sign. */
static const struct { int first, count; } ranges[] = {
   { 32, 95 }, { 160, 96 }, { 0x2010, 0x17 }, { 0x2122, 1 },
};
#define RANGES ((int)SDL_arraysize(ranges))
#define GLYPHS (95 + 96 + 0x17 + 1)

struct atlas
{
   enum ui_font font;
   int px;
   GLuint texture;
   int w, h;
   stbtt_packedchar chars[GLYPHS];
   /* Capital height, font units to pixels, and when it was last used. */
   float cap, scale;
   Uint64 used;
};

/* A box (u_mode 0, rounded, edges antialiased from its distance field) or
 * a glyph (u_mode 1, the atlas' coverage at u_uv). */
static const char *vs_src =
   "#version 330 core\n"
   "uniform vec4 u_rect;\n"
   "uniform vec4 u_uv;\n"
   "uniform vec2 u_screen;\n"
   "out vec2 v_pos;\n"
   "out vec2 v_uv;\n"
   "void main()\n"
   "{\n"
   "   vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
   "   vec2 px = u_rect.xy + p * u_rect.zw;\n"
   "   v_pos = px - (u_rect.xy + u_rect.zw * 0.5);\n"
   "   v_uv = mix(u_uv.xy, u_uv.zw, p);\n"
   "   vec2 ndc = px / u_screen * 2.0 - 1.0;\n"
   "   gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);\n"
   "}\n";

static const char *fs_src =
   "#version 330 core\n"
   "uniform vec4 u_rect;\n"
   "uniform vec4 u_color;\n"
   "uniform float u_radius;\n"
   "uniform float u_mode;\n"
   "uniform float u_ring;\n"
   "uniform float u_style;\n"
   "uniform vec3 u_ring2;\n"
   "uniform float u_time;\n"
   "uniform sampler2D u_texture;\n"
   "in vec2 v_pos;\n"
   "in vec2 v_uv;\n"
   "out vec4 frag;\n"
   RING_GLSL
   "void main()\n"
   "{\n"
   "   float a;\n"
   "   if (u_mode > 1.5)\n"
   "   {\n"
   "      vec4 t = texture(u_texture, v_uv);\n"
   "      vec2 q = abs(v_pos) - u_rect.zw * 0.5 + u_radius;\n"
   "      float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_radius;\n"
   "      frag = vec4(t.rgb * u_color.rgb, t.a * u_color.a * clamp(0.5 - d, 0.0, 1.0));\n"
   "      return;\n"
   "   }\n"
   "   if (u_mode > 0.5)\n"
   "      a = texture(u_texture, v_uv).r;\n"
   "   else\n"
   "   {\n"
   "      vec2 q = abs(v_pos) - u_rect.zw * 0.5 + u_radius;\n"
   "      float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_radius;\n"
   "      a = clamp(0.5 - d, 0.0, 1.0);\n"
   "      if (u_ring > 0.0)\n"
   "         a *= clamp(d + u_ring + 0.5, 0.0, 1.0);\n"
   "   }\n"
   "   vec3 rgb = u_style > 0.5 ? ring_style(v_pos, u_color.rgb, u_ring2, u_style, u_time)\n"
   "      : u_color.rgb;\n"
   "   frag = vec4(rgb, u_color.a * a);\n"
   "}\n";

static struct
{
   GLuint program, vao;
   GLint u_rect, u_uv, u_screen, u_color, u_radius, u_mode, u_ring, u_style, u_ring2, u_time;
   /* The fonts' files, as stb_truetype reads them in place. */
   void *data[2];
   stbtt_fontinfo info[2];
   bool loaded[2];
   struct atlas atlases[ATLASES];
   int width, height;
} ui;

static GLuint compile(GLenum kind, const char *src)
{
   GLuint s = gl.CreateShader(kind);
   GLint ok;
   gl.ShaderSource(s, 1, &src, NULL);
   gl.CompileShader(s);
   gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok)
   {
      char log[1024];
      gl.GetShaderInfoLog(s, sizeof(log), NULL, log);
      SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "UI shader: %s", log);
   }
   return s;
}

bool ui_init(const char *font_dir)
{
   static const char *files[2] = { "Outfit-Medium.ttf", "Outfit-Bold.ttf" };
   GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
   GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
   GLint ok;
   int i;

   ui.program = gl.CreateProgram();
   gl.AttachShader(ui.program, vs);
   gl.AttachShader(ui.program, fs);
   gl.LinkProgram(ui.program);
   gl.DeleteShader(vs);
   gl.DeleteShader(fs);
   gl.GetProgramiv(ui.program, GL_LINK_STATUS, &ok);
   if (!ok)
      return false;
   ui.u_rect   = gl.GetUniformLocation(ui.program, "u_rect");
   ui.u_uv     = gl.GetUniformLocation(ui.program, "u_uv");
   ui.u_screen = gl.GetUniformLocation(ui.program, "u_screen");
   ui.u_color  = gl.GetUniformLocation(ui.program, "u_color");
   ui.u_radius = gl.GetUniformLocation(ui.program, "u_radius");
   ui.u_mode   = gl.GetUniformLocation(ui.program, "u_mode");
   ui.u_ring   = gl.GetUniformLocation(ui.program, "u_ring");
   ui.u_style  = gl.GetUniformLocation(ui.program, "u_style");
   ui.u_ring2  = gl.GetUniformLocation(ui.program, "u_ring2");
   ui.u_time   = gl.GetUniformLocation(ui.program, "u_time");
   gl.GenVertexArrays(1, &ui.vao);

   /* A missing font only means no text. */
   for (i = 0; i < 2; i++)
   {
      char path[1024];
      size_t size = 0;

      SDL_snprintf(path, sizeof(path), "%s/%s", font_dir, files[i]);
      ui.data[i] = SDL_LoadFile(path, &size);
      ui.loaded[i] = ui.data[i] && stbtt_InitFont(&ui.info[i], ui.data[i],
            stbtt_GetFontOffsetForIndex(ui.data[i], 0));
      if (!ui.loaded[i])
         SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "UI font %s: not loaded", path);
   }
   return true;
}

void ui_quit(void)
{
   int i;

   for (i = 0; i < ATLASES; i++)
      if (ui.atlases[i].texture)
         glDeleteTextures(1, &ui.atlases[i].texture);
   for (i = 0; i < 2; i++)
      SDL_free(ui.data[i]);
   if (ui.vao)
      gl.DeleteVertexArrays(1, &ui.vao);
   if (ui.program)
      gl.DeleteProgram(ui.program);
   SDL_zero(ui);
}

/* The glyphs of font at px, baked now if they aren't yet. NULL without
 * the font. */
static struct atlas *atlas_for(enum ui_font font, int px)
{
   struct atlas *a = NULL;
   stbtt_pack_range pr[RANGES];
   stbtt_pack_context pc;
   unsigned char *pixels = NULL;
   int i, n = 0, size, x0, y0, x1, y1;

   if (!ui.loaded[font] || px < 4)
      return NULL;
   for (i = 0; i < ATLASES; i++)
      if (ui.atlases[i].texture && ui.atlases[i].font == font && ui.atlases[i].px == px)
      {
         ui.atlases[i].used = SDL_GetTicksNS();
         return &ui.atlases[i];
      }
   /* A free slot, else the one used longest ago. */
   for (i = 0; i < ATLASES; i++)
      if (!a || !ui.atlases[i].texture || ui.atlases[i].used < a->used)
      {
         a = &ui.atlases[i];
         if (!a->texture)
            break;
      }
   if (a->texture)
      glDeleteTextures(1, &a->texture);
   SDL_zerop(a);

   for (i = 0; i < RANGES; i++)
   {
      /* px is the em size (STBTT_POINT_SIZE), not ascent to descent. */
      pr[i].font_size = STBTT_POINT_SIZE((float)px);
      pr[i].first_unicode_codepoint_in_range = ranges[i].first;
      pr[i].array_of_unicode_codepoints = NULL;
      pr[i].num_chars = ranges[i].count;
      pr[i].chardata_for_range = a->chars + n;
      n += ranges[i].count;
   }
   /* Square, as small as holds them all. */
   for (size = 256; size <= 4096; size *= 2)
   {
      bool ok;

      SDL_free(pixels);
      pixels = SDL_calloc(1, (size_t)size * size);
      if (!pixels || !stbtt_PackBegin(&pc, pixels, size, size, 0, 1, NULL))
         break;
      /* Twice as fine both ways, so glyphs sit at fractions of a pixel:
       * spacing stays even, and text moving smoothly (ui_text_snap) stays
       * the same instead of shimmering. */
      stbtt_PackSetOversampling(&pc, 2, 2);
      ok = stbtt_PackFontRanges(&pc, ui.data[font], 0, pr, RANGES) != 0;
      stbtt_PackEnd(&pc);
      if (ok)
         break;
   }
   if (!pixels || size > 4096)
   {
      SDL_free(pixels);
      return NULL;
   }

   glGenTextures(1, &a->texture);
   glBindTexture(GL_TEXTURE_2D, a->texture);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, size, size, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   SDL_free(pixels);

   a->font = font;
   a->px = px;
   a->w = a->h = size;
   a->used = SDL_GetTicksNS();
   a->scale = stbtt_ScaleForMappingEmToPixels(&ui.info[font], (float)px);
   if (stbtt_GetCodepointBox(&ui.info[font], 'H', &x0, &y0, &x1, &y1))
      a->cap = y1 * a->scale;
   else
      a->cap = px * 0.5f;
   return a;
}

/* The glyph's index in the atlas, '?' for one not baked. */
static int glyph_index(Uint32 cp)
{
   int i, n = 0;

   for (i = 0; i < RANGES; i++)
   {
      if ((int)cp >= ranges[i].first && (int)cp < ranges[i].first + ranges[i].count)
         return n + (int)cp - ranges[i].first;
      n += ranges[i].count;
   }
   return '?' - 32;
}

bool ui_begin(int width, int height)
{
   if (!ui.program)
      return false;
   ui.width = width;
   ui.height = height;
   glViewport(0, 0, width, height);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   gl.UseProgram(ui.program);
   gl.BindVertexArray(ui.vao);
   gl.ActiveTexture(GL_TEXTURE0);
   gl.Uniform2f(ui.u_screen, (float)width, (float)height);
   gl.Uniform1f(ui.u_mode, 0.0f);
   return true;
}

void ui_end(void)
{
   glBindTexture(GL_TEXTURE_2D, 0);
   glDisable(GL_BLEND);
}

void ui_rect(float x, float y, float w, float h, float radius,
      float r, float g, float b, float a)
{
   gl.Uniform4f(ui.u_rect, x, y, w, h);
   gl.Uniform4f(ui.u_color, r, g, b, a);
   gl.Uniform1f(ui.u_radius, SDL_min(radius, SDL_min(w, h) / 2));
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void ui_ring_pattern(float style, float r, float g, float b, float seconds)
{
   gl.Uniform1f(ui.u_style, style);
   gl.Uniform3f(ui.u_ring2, r, g, b);
   gl.Uniform1f(ui.u_time, seconds);
}

void ui_ring(float x, float y, float w, float h, float radius, float thick,
      float r, float g, float b, float a)
{
   gl.Uniform1f(ui.u_ring, thick);
   ui_rect(x, y, w, h, radius, r, g, b, a);
   gl.Uniform1f(ui.u_ring, 0.0f);
}

void ui_scrollbar(struct ui_scrollbar *bar, float x, float y, float w, float h, float first,
      float shown, float total, float r, float g, float b)
{
   SDL_Window *window;
   float mx, my;
   bool over;

   bar->x = x;
   bar->y = y;
   bar->w = w;
   bar->h = h;
   bar->range = total - shown;
   if (bar->range <= 0.0f || shown <= 0.0f || h <= 0.0f)
   {
      bar->h = 0.0f;
      bar->held = false;
      return;
   }
   /* Never shorter than a few widths, so it stays easy to see and grab. */
   bar->thumb = SDL_min(SDL_max(SDL_floorf(h * shown / total + 0.5f), 4.0f * w), h);
   bar->at = SDL_floorf(y + (h - bar->thumb) * SDL_clamp(first / bar->range, 0.0f, 1.0f) + 0.5f);
   /* Brighter under the mouse, and more while held. */
   window = SDL_GetKeyboardFocus();
   SDL_GetMouseState(&mx, &my);
   over = window && ui_scrollbar_over(bar, mx * SDL_GetWindowPixelDensity(window),
         my * SDL_GetWindowPixelDensity(window));
   ui_rect(x, y, w, h, w / 2.0f, r, g, b, 0.1f);
   ui_rect(x, bar->at, w, bar->thumb, w / 2.0f, r, g, b, bar->held ? 0.85f : over ? 0.7f : 0.5f);
}

bool ui_scrollbar_over(const struct ui_scrollbar *bar, float px, float py)
{
   /* Wider than it's drawn: it's thin. */
   return bar->h > 0.0f && px >= bar->x - bar->w && px < bar->x + 3.0f * bar->w
      && py >= bar->y && py < bar->y + bar->h;
}

bool ui_scrollbar_press(struct ui_scrollbar *bar, float px, float py)
{
   if (!ui_scrollbar_over(bar, px, py))
      return false;
   /* On the thumb: held where it was taken; on the track: its middle
    * jumps there. */
   bar->grab = py >= bar->at && py < bar->at + bar->thumb ? py - bar->at : bar->thumb / 2.0f;
   bar->held = true;
   return true;
}

bool ui_scrollbar_drag(const struct ui_scrollbar *bar, float py, float *first)
{
   float room = bar->h - bar->thumb;

   if (!bar->held || bar->h <= 0.0f)
      return false;
   *first = room > 0.0f
      ? SDL_clamp((py - bar->grab - bar->y) / room, 0.0f, 1.0f) * bar->range : 0.0f;
   return true;
}

/* The font's kerning between two characters, in pixels. */
static float kern(const struct atlas *a, Uint32 prev, Uint32 cp)
{
   return prev ? stbtt_GetCodepointKernAdvance(&ui.info[a->font], (int)prev, (int)cp) * a->scale
      : 0.0f;
}

static float width_of(const struct atlas *a, const char *text, size_t len)
{
   const char *p = text;
   float w = 0.0f;
   Uint32 cp, prev = 0;

   while ((size_t)(p - text) < len && (cp = SDL_StepUTF8(&p, NULL)) != 0)
   {
      w += kern(a, prev, cp) + a->chars[glyph_index(cp)].xadvance;
      prev = cp;
   }
   return w;
}

float ui_text_width(enum ui_font font, float px, const char *text)
{
   struct atlas *a = atlas_for(font, (int)(px + 0.5f));
   return a ? width_of(a, text, SDL_strlen(text)) : 0.0f;
}

static bool snap_text = true;

void ui_text_snap(bool snap)
{
   snap_text = snap;
}

float ui_text(enum ui_font font, float px, float x, float cy, enum ui_align align,
      float max_w, float r, float g, float b, float a, const char *text)
{
   struct atlas *at = atlas_for(font, (int)(px + 0.5f));
   size_t len = SDL_strlen(text);
   bool cut = false;
   float w, pen, base;
   const char *p;
   Uint32 cp, prev = 0;

   if (!at)
      return 0.0f;
   w = width_of(at, text, len);
   /* Too wide: whole characters off the end until it fits with "...". */
   if (max_w > 0.0f && w > max_w)
   {
      float dots = width_of(at, "...", 3);
      cut = true;
      while (len > 0 && width_of(at, text, len) + dots > max_w)
      {
         len--;
         while (len > 0 && ((Uint8)text[len] & 0xC0) == 0x80)
            len--;
      }
      w = width_of(at, text, len) + dots;
   }

   pen = align == UI_CENTER ? x - w / 2 : align == UI_RIGHT ? x - w : x;
   base = cy + at->cap / 2;
   if (snap_text)
   {
      pen = SDL_floorf(pen + 0.5f);
      base = SDL_floorf(base + 0.5f);
   }
   glBindTexture(GL_TEXTURE_2D, at->texture);
   gl.Uniform1f(ui.u_mode, 1.0f);
   gl.Uniform4f(ui.u_color, r, g, b, a);
   p = text;
   while (true)
   {
      stbtt_aligned_quad q;
      float px0, py0 = base;

      if ((size_t)(p - text) < len)
         cp = SDL_StepUTF8(&p, NULL);
      else if (cut)
      {
         p = "...";
         len = 3;
         text = p;
         cut = false;
         continue;
      }
      else
         break;
      if (!cp)
         break;
      pen += kern(at, prev, cp);
      prev = cp;
      px0 = pen;
      stbtt_GetPackedQuad(at->chars, at->w, at->h, glyph_index(cp), &px0, &py0, &q, 0);
      if (q.x1 > q.x0)
      {
         gl.Uniform4f(ui.u_rect, q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0);
         gl.Uniform4f(ui.u_uv, q.s0, q.t0, q.s1, q.t1);
         glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      }
      pen = px0;
   }
   gl.Uniform1f(ui.u_mode, 0.0f);
   return w;
}

void ui_char(enum ui_font font, float px, float cx, float cy,
      float r, float g, float b, float a, Uint32 cp)
{
   struct atlas *at = atlas_for(font, (int)(px + 0.5f));
   stbtt_aligned_quad q;
   int x0, y0, x1, y1;
   float x, y;

   if (!at)
      return;
   /* The pen put so the outline's middle (not its padded box's) lands on
    * (cx, cy), then on a whole pixel: drawn the same, not blurred between
    * pixels, whatever the size and position. */
   if (!stbtt_GetCodepointBox(&ui.info[font], (int)cp, &x0, &y0, &x1, &y1))
      x0 = y0 = x1 = y1 = 0;
   x = SDL_floorf(cx - (x0 + x1) / 2.0f * at->scale + 0.5f);
   y = SDL_floorf(cy + (y0 + y1) / 2.0f * at->scale + 0.5f);
   stbtt_GetPackedQuad(at->chars, at->w, at->h, glyph_index(cp), &x, &y, &q, 0);
   if (q.x1 <= q.x0)
      return;
   glBindTexture(GL_TEXTURE_2D, at->texture);
   gl.Uniform1f(ui.u_mode, 1.0f);
   gl.Uniform4f(ui.u_color, r, g, b, a);
   gl.Uniform4f(ui.u_rect, q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0);
   gl.Uniform4f(ui.u_uv, q.s0, q.t0, q.s1, q.t1);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   gl.Uniform1f(ui.u_mode, 0.0f);
}

void ui_mask(unsigned texture, float x, float y, float w, float h,
      float r, float g, float b, float a)
{
   glBindTexture(GL_TEXTURE_2D, texture);
   gl.Uniform1f(ui.u_mode, 1.0f);
   gl.Uniform4f(ui.u_color, r, g, b, a);
   gl.Uniform4f(ui.u_rect, x, y, w, h);
   gl.Uniform4f(ui.u_uv, 0.0f, 0.0f, 1.0f, 1.0f);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   gl.Uniform1f(ui.u_mode, 0.0f);
}

void ui_image(unsigned texture, float x, float y, float w, float h, float radius, float a)
{
   ui_image_uv(texture, x, y, w, h, radius, a, 0.0f, 0.0f, 1.0f, 1.0f);
}

void ui_image_uv(unsigned texture, float x, float y, float w, float h, float radius, float a,
      float u0, float v0, float u1, float v1)
{
   glBindTexture(GL_TEXTURE_2D, texture);
   gl.Uniform1f(ui.u_mode, 2.0f);
   gl.Uniform4f(ui.u_color, 1.0f, 1.0f, 1.0f, a);
   gl.Uniform1f(ui.u_radius, SDL_min(radius, SDL_min(w, h) / 2));
   gl.Uniform4f(ui.u_rect, x, y, w, h);
   gl.Uniform4f(ui.u_uv, u0, v0, u1, v1);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   gl.Uniform1f(ui.u_mode, 0.0f);
}
