// Reading the API's JSON with jsmn (tokens point into the text; nothing is copied).
#ifndef SNC_JSON_H
#define SNC_JSON_H

#include <stddef.h>

#define JSMN_HEADER
#include "jsmn.h"

typedef struct {
	const char *js;
	jsmntok_t *t;
	int n;
} sj;

// Parse js[0..len). Returns the token count, or a negative jsmn error.
int sj_parse(sj *j, const char *js, size_t len, jsmntok_t *toks, unsigned max);

int sj_next(const sj *j, int i);                        // the token after i's whole subtree
int sj_key(const sj *j, int obj, const char *key);      // the value of obj[key], or -1
int sj_path(const sj *j, int root, const char *path);   // "game.view.board", or -1
int sj_at(const sj *j, int arr, int k);                 // arr[k], or -1
int sj_size(const sj *j, int i);                        // elements of an array / keys of an object
int sj_is_array(const sj *j, int i);
int sj_is_object(const sj *j, int i);
int sj_is_null(const sj *j, int i);                     // also true for -1 (missing)
int sj_str(const sj *j, int i, char *dst, size_t cap);  // unescaped UTF-8; -1 if not a string
long sj_int(const sj *j, int i, long def);
double sj_num(const sj *j, int i, double def);
int sj_bool(const sj *j, int i, int def);
int sj_eq(const sj *j, int i, const char *s);           // a string token equal to s

// Writes s as a JSON string literal (with quotes) into dst. Returns its length.
size_t sj_quote(char *dst, size_t cap, const char *s);

#endif
