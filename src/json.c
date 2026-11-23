#include "json.h"

#include <SDL3/SDL.h>

struct parser
{
   const char *p;
   int depth;
};

static void skip_space(struct parser *ps)
{
   while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
      ps->p++;
}

static int hex_digit(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

static bool read_hex4(const char *p, Uint32 *out)
{
   int i;

   *out = 0;
   for (i = 0; i < 4; i++)
   {
      int d = hex_digit(p[i]);
      if (d < 0)
         return false;
      *out = *out << 4 | (Uint32)d;
   }
   return true;
}

static char *put_utf8(char *dst, Uint32 cp)
{
   if (cp < 0x80)
      *dst++ = (char)cp;
   else if (cp < 0x800)
   {
      *dst++ = (char)(0xC0 | cp >> 6);
      *dst++ = (char)(0x80 | (cp & 0x3F));
   }
   else if (cp < 0x10000)
   {
      *dst++ = (char)(0xE0 | cp >> 12);
      *dst++ = (char)(0x80 | (cp >> 6 & 0x3F));
      *dst++ = (char)(0x80 | (cp & 0x3F));
   }
   else
   {
      *dst++ = (char)(0xF0 | cp >> 18);
      *dst++ = (char)(0x80 | (cp >> 12 & 0x3F));
      *dst++ = (char)(0x80 | (cp >> 6 & 0x3F));
      *dst++ = (char)(0x80 | (cp & 0x3F));
   }
   return dst;
}

/* At the opening quote. The unescaped text is never longer than the
 * escaped one, so one allocation of that length holds it. */
static char *parse_string(struct parser *ps)
{
   const char *start = ++ps->p, *end = start;
   char *out, *dst;

   while (*end && *end != '"')
      end += *end == '\\' && end[1] ? 2 : 1;
   if (*end != '"' || !(out = SDL_malloc((size_t)(end - start) + 1)))
      return NULL;
   dst = out;
   while (ps->p < end)
   {
      char c = *ps->p++;
      Uint32 cp, lo;

      if (c != '\\')
      {
         *dst++ = c;
         continue;
      }
      switch (c = *ps->p++)
      {
         case 'b': *dst++ = '\b'; break;
         case 'f': *dst++ = '\f'; break;
         case 'n': *dst++ = '\n'; break;
         case 'r': *dst++ = '\r'; break;
         case 't': *dst++ = '\t'; break;
         case 'u':
            if (end - ps->p < 4 || !read_hex4(ps->p, &cp))
               goto fail;
            ps->p += 4;
            /* A surrogate pair is one character. */
            if (cp >= 0xD800 && cp < 0xDC00 && end - ps->p >= 6 && ps->p[0] == '\\'
                  && ps->p[1] == 'u' && read_hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000)
            {
               cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
               ps->p += 6;
            }
            dst = put_utf8(dst, cp);
            break;
         default: *dst++ = c; break;
      }
   }
   *dst = '\0';
   ps->p = end + 1;
   return out;
fail:
   SDL_free(out);
   return NULL;
}

static struct json *parse_value(struct parser *ps);

/* Items of an array or members of an object, up to close. */
static bool parse_items(struct parser *ps, struct json *node, char close)
{
   struct json **tail = &node->child;

   ps->p++;
   skip_space(ps);
   if (*ps->p == close)
   {
      ps->p++;
      return true;
   }
   while (true)
   {
      char *key = NULL;
      struct json *item;

      skip_space(ps);
      if (close == '}')
      {
         if (*ps->p != '"' || !(key = parse_string(ps)))
            return false;
         skip_space(ps);
         if (*ps->p++ != ':')
         {
            SDL_free(key);
            return false;
         }
      }
      if (!(item = parse_value(ps)))
      {
         SDL_free(key);
         return false;
      }
      item->key = key;
      *tail = item;
      tail = &item->next;
      skip_space(ps);
      if (*ps->p == ',')
         ps->p++;
      else if (*ps->p == close)
      {
         ps->p++;
         return true;
      }
      else
         return false;
   }
}

static struct json *parse_value(struct parser *ps)
{
   struct json *node;

   /* Deep nesting is only ever an attack or garbage. */
   if (++ps->depth > 64 || !(node = SDL_calloc(1, sizeof(*node))))
      return NULL;
   skip_space(ps);
   switch (*ps->p)
   {
      case '{':
         node->type = JSON_OBJECT;
         if (!parse_items(ps, node, '}'))
            goto fail;
         break;
      case '[':
         node->type = JSON_ARRAY;
         if (!parse_items(ps, node, ']'))
            goto fail;
         break;
      case '"':
         node->type = JSON_STRING;
         if (!(node->str = parse_string(ps)))
            goto fail;
         break;
      case 't':
      case 'f':
         node->type = JSON_BOOL;
         node->b = *ps->p == 't';
         if (SDL_strncmp(ps->p, node->b ? "true" : "false", node->b ? 4 : 5) != 0)
            goto fail;
         ps->p += node->b ? 4 : 5;
         break;
      case 'n':
         if (SDL_strncmp(ps->p, "null", 4) != 0)
            goto fail;
         ps->p += 4;
         break;
      default:
      {
         char *end;
         node->type = JSON_NUMBER;
         node->num = SDL_strtod(ps->p, &end);
         if (end == ps->p)
            goto fail;
         ps->p = end;
         break;
      }
   }
   ps->depth--;
   return node;
fail:
   json_free(node);
   return NULL;
}

struct json *json_parse(const char *text)
{
   struct parser ps = { text, 0 };

   return text ? parse_value(&ps) : NULL;
}

void json_free(struct json *node)
{
   while (node)
   {
      struct json *next = node->next;
      json_free(node->child);
      SDL_free(node->key);
      SDL_free(node->str);
      SDL_free(node);
      node = next;
   }
}

struct json *json_get(const struct json *node, const char *key)
{
   struct json *c;

   if (!node || node->type != JSON_OBJECT)
      return NULL;
   for (c = node->child; c; c = c->next)
      if (c->key && SDL_strcmp(c->key, key) == 0)
         return c;
   return NULL;
}

const char *json_str(const struct json *node, const char *key, const char *fallback)
{
   struct json *v = json_get(node, key);
   return v && v->type == JSON_STRING ? v->str : fallback;
}

double json_num(const struct json *node, const char *key, double fallback)
{
   struct json *v = json_get(node, key);
   return v && v->type == JSON_NUMBER ? v->num : fallback;
}

bool json_bool(const struct json *node, const char *key, bool fallback)
{
   struct json *v = json_get(node, key);
   return v && v->type == JSON_BOOL ? v->b : fallback;
}

void json_write_string(SDL_IOStream *io, const char *text)
{
   const unsigned char *p = (const unsigned char *)(text ? text : "");

   SDL_WriteU8(io, '"');
   for (; *p; p++)
   {
      if (*p == '"' || *p == '\\')
         SDL_IOprintf(io, "\\%c", *p);
      else if (*p == '\n')
         SDL_IOprintf(io, "\\n");
      else if (*p < 0x20)
         SDL_IOprintf(io, "\\u%04x", *p);
      else
         SDL_WriteU8(io, *p);
   }
   SDL_WriteU8(io, '"');
}
