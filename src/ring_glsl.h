#ifndef FUSEBOX_RING_GLSL_H
#define FUSEBOX_RING_GLSL_H

/* The selection ring's patterns (Settings > Display), for the grid's and
 * the UI's shaders alike. p: from the ring's centre; a and b its colours;
 * style as enum settings_style; t in seconds. Style + RING_FLOW: for what
 * isn't a ring (an underline, an icon), the gradient and the rainbow flow
 * left to right across it instead of round its centre. */
#define RING_FLOW 10.0f
#define RING_GLSL \
   "vec3 ring_hue(float h)\n" \
   "{\n" \
   "   return clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);\n" \
   "}\n" \
   "vec3 ring_style(vec2 p, vec3 a, vec3 b, float style, float t)\n" \
   "{\n" \
   "   float turn = atan(p.y, p.x);\n" \
   "   if (style > 9.5)\n" \
   "   {\n" \
   "      style -= 10.0;\n" \
   "      turn = -p.x / 24.0;\n" \
   "   }\n" \
   "   if (style < 1.5)\n" \
   "   {\n" \
   "      float g = 0.5 + 0.5 * cos(t * 3.14159 - turn);\n" \
   "      return mix(a, b, g * g * (3.0 - 2.0 * g));\n" \
   "   }\n" \
   "   if (style < 2.5)\n" \
   "   {\n" \
   "      float s = fract((p.x + p.y) / 28.0 + t * 0.5);\n" \
   "      return mix(a, b, smoothstep(0.46, 0.5, s) - smoothstep(0.96, 1.0, s));\n" \
   "   }\n" \
   "   if (style < 3.5)\n" \
   "      return mix(a, b, 0.5 + 0.5 * sin(t * 5.0));\n" \
   "   return ring_hue(t * 0.4 + turn / 6.2831853);\n" \
   "}\n"

#endif
