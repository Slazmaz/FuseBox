#ifndef FUSEBOX_JSON_H
#define FUSEBOX_JSON_H

#include <stdbool.h>
#include <SDL3/SDL_iostream.h>

/* A small JSON reader (a tree of nodes) and writer helpers, for
 * SteamGridDB's answers, launchers' manifests and library.json. */

enum json_type
{
   JSON_NULL,
   JSON_BOOL,
   JSON_NUMBER,
   JSON_STRING,
   JSON_ARRAY,
   JSON_OBJECT
};

struct json
{
   enum json_type type;
   /* The member's name inside an object, else NULL. */
   char *key;
   /* JSON_STRING's text (UTF-8, unescaped). */
   char *str;
   double num;
   bool b;
   /* An array's or object's first item; each item's next. */
   struct json *child, *next;
};

/* NULL when text isn't JSON. Free with json_free. */
struct json *json_parse(const char *text);
void json_free(struct json *node);

/* The object's member, NULL if missing (or node isn't an object). */
struct json *json_get(const struct json *node, const char *key);
/* The member's string, or fallback when missing or not a string. */
const char *json_str(const struct json *node, const char *key, const char *fallback);
double json_num(const struct json *node, const char *key, double fallback);
bool json_bool(const struct json *node, const char *key, bool fallback);

/* Writes text as a quoted JSON string. */
void json_write_string(SDL_IOStream *io, const char *text);

#endif
