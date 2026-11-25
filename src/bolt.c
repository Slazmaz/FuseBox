#include "bolt.h"

/* tools/make_icon.py draws the .exe's icon from the same points. */
static const float bolt[][2] = {
   { 0.14f, -0.40f }, { -0.27f, 0.06f }, { -0.01f, 0.06f },
   { -0.14f, 0.40f }, { 0.27f, -0.06f }, { 0.01f, -0.06f },
};

bool bolt_in(float x, float y)
{
   int i, j, n = (int)SDL_arraysize(bolt);
   bool in = false;

   /* Even-odd: a ray to the right crosses the edges an odd number of
    * times from inside. */
   for (i = 0, j = n - 1; i < n; j = i++)
      if ((bolt[i][1] > y) != (bolt[j][1] > y)
            && x < (bolt[j][0] - bolt[i][0]) * (y - bolt[i][1]) / (bolt[j][1] - bolt[i][1])
               + bolt[i][0])
         in = !in;
   return in;
}

void bolt_icon(Uint8 *rgba, int size)
{
   enum { SUB = 4 };
   /* C_ACCENT (main.c). */
   const float radius = 0.22f, blue[3] = { 0.30f, 0.56f, 1.00f };
   int x, y, sx, sy;

   for (y = 0; y < size; y++)
      for (x = 0; x < size; x++)
      {
         float r = 0, g = 0, b = 0, a = 0;
         Uint8 *p = rgba + (y * size + x) * 4;
         for (sy = 0; sy < SUB; sy++)
            for (sx = 0; sx < SUB; sx++)
            {
               float fx = (x + (sx + 0.5f) / SUB) / size - 0.5f;
               float fy = (y + (sy + 0.5f) / SUB) / size - 0.5f;
               float qx = SDL_max(SDL_fabsf(fx) - (0.5f - radius), 0.0f);
               float qy = SDL_max(SDL_fabsf(fy) - (0.5f - radius), 0.0f);
               if (qx * qx + qy * qy > radius * radius)
                  continue;
               a += 1;
               if (bolt_in(fx, fy))
                  r += 1, g += 1, b += 1;
               else
                  r += blue[0], g += blue[1], b += blue[2];
            }
         /* Colour averaged over the covered samples, coverage as alpha. */
         p[0] = a ? (Uint8)(r / a * 255) : 0;
         p[1] = a ? (Uint8)(g / a * 255) : 0;
         p[2] = a ? (Uint8)(b / a * 255) : 0;
         p[3] = (Uint8)(a * 255 / (SUB * SUB));
      }
}
