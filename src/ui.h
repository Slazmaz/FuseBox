#ifndef FUSEBOX_UI_H
#define FUSEBOX_UI_H

#include <stdbool.h>
#include <SDL3/SDL_stdinc.h>

/* 2D drawing for the UI over the grid: rounded boxes and text in Outfit
 * (fonts/ next to the exe). Glyphs are baked per font and pixel size on
 * first use (stb_truetype), Latin-1 and common punctuation; others show
 * as '?'. Sizes and positions in drawable pixels. Needs the GL context. */

/* The font's capital height, in em: text sizes are em sizes, so a capital
 * height h takes a size of h / UI_FONT_CAP. */
#define UI_FONT_CAP 0.7f

enum ui_font
{
   UI_MEDIUM,
   UI_BOLD
};

enum ui_align
{
   UI_LEFT,
   UI_CENTER,
   UI_RIGHT
};

bool ui_init(const char *font_dir);
void ui_quit(void);

/* Between these, the UI's program is bound and blending on; other
 * drawing (the grid's) must not happen in between. False (draw nothing)
 * before ui_init. */
bool ui_begin(int width, int height);
void ui_end(void);

void ui_rect(float x, float y, float w, float h, float radius,
      float r, float g, float b, float a);
/* The box's outline only, thick pixels wide, inside its edge. */
void ui_ring(float x, float y, float w, float h, float radius, float thick,
      float r, float g, float b, float a);
/* A list's scrollbar, as last drawn, and whether its thumb is held. */
struct ui_scrollbar
{
   float x, y, w, h, thumb, at, range, grab;
   bool held;
};
/* The bar, w wide down the box: a faint track, the thumb where the first
 * of its shown rows (or pixels, of total) is. Nothing when they all fit. */
void ui_scrollbar(struct ui_scrollbar *bar, float x, float y, float w, float h, float first,
      float shown, float total, float r, float g, float b);
/* Whether drawable pixel (px, py) is on the bar. */
bool ui_scrollbar_over(const struct ui_scrollbar *bar, float px, float py);
/* A press there: true when on the bar, which is then held (dragged until
 * held is set false, on the button's release). */
bool ui_scrollbar_press(struct ui_scrollbar *bar, float px, float py);
/* While held: where the mouse at py puts the first row (0 to total -
 * shown, unrounded). False when not held. */
bool ui_scrollbar_drag(const struct ui_scrollbar *bar, float py, float *first);
/* The next rings' pattern (ring_glsl.h) until set back to style 0: the
 * second colour and how long it has been running. */
void ui_ring_pattern(float style, float r, float g, float b, float seconds);
/* Width of text at size px (its em size in pixels). */
float ui_text_width(enum ui_font font, float px, const char *text);
/* Text with its capitals centred on cy, from x as aligned; cut short with
 * "..." to fit max_w (0 for no limit). Returns the width drawn. */
float ui_text(enum ui_font font, float px, float x, float cy, enum ui_align align,
      float max_w, float r, float g, float b, float a, const char *text);
/* Text put on whole pixels (true, the default: sharp) or where it falls
 * (false: for text riding something moving smoothly, which would otherwise
 * hop a pixel at a time). */
void ui_text_snap(bool snap);
/* One character with its own outline centred on (cx, cy): a symbol ('+')
 * that doesn't sit on the capitals' line. */
void ui_char(enum ui_font font, float px, float cx, float cy,
      float r, float g, float b, float a, Uint32 cp);
/* A coverage texture (GL_R8, its red the alpha) over the box, in the
 * colour: an icon. */
void ui_mask(unsigned texture, float x, float y, float w, float h,
      float r, float g, float b, float a);
/* An RGBA texture over the box (its corners radius round), at alpha a: a
 * picture. */
void ui_image(unsigned texture, float x, float y, float w, float h, float radius, float a);
/* Part of it (texture coordinates u0,v0 to u1,v1): a crop that fills the
 * box. */
void ui_image_uv(unsigned texture, float x, float y, float w, float h, float radius, float a,
      float u0, float v0, float u1, float v1);

#endif
