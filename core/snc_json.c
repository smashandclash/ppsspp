// Reading the API's JSON with jsmn.
#include "jsmn.h"  // the implementation (before snc_json.h pulls in the declarations)
#include "snc_json.h"

#include <stdlib.h>
#include <string.h>

int sj_parse(sj *j, const char *js, size_t len, jsmntok_t *toks, unsigned max) {
	jsmn_parser p;
	jsmn_init(&p);
	j->js = js;
	j->t = toks;
	j->n = jsmn_parse(&p, js, len, toks, max);
	return j->n;
}

int sj_next(const sj *j, int i) {
	if (i < 0 || i >= j->n) return j->n;
	int end = j->t[i].end;
	i++;
	while (i < j->n && j->t[i].start < end) i++;
	return i;
}

static int tok_eq(const sj *j, int i, const char *s, size_t len) {
	const jsmntok_t *t = &j->t[i];
	return t->type == JSMN_STRING && (size_t)(t->end - t->start) == len && !memcmp(j->js + t->start, s, len);
}

int sj_key(const sj *j, int obj, const char *key) {
	if (obj < 0 || obj >= j->n || j->t[obj].type != JSMN_OBJECT) return -1;
	size_t len = strlen(key);
	int i = obj + 1;
	for (int k = 0; k < j->t[obj].size && i < j->n; k++) {
		if (tok_eq(j, i, key, len)) return i + 1 < j->n ? i + 1 : -1;
		i = sj_next(j, i + 1);  // skip the key, then its value
	}
	return -1;
}

int sj_path(const sj *j, int root, const char *path) {
	char part[48];
	int i = root;
	while (*path && i >= 0) {
		size_t n = strcspn(path, ".");
		if (n >= sizeof part) return -1;
		memcpy(part, path, n);
		part[n] = 0;
		i = sj_key(j, i, part);
		path += n;
		if (*path == '.') path++;
	}
	return i;
}

int sj_at(const sj *j, int arr, int k) {
	if (arr < 0 || arr >= j->n || j->t[arr].type != JSMN_ARRAY || k < 0 || k >= j->t[arr].size) return -1;
	int i = arr + 1;
	while (k-- > 0) i = sj_next(j, i);
	return i < j->n ? i : -1;
}

int sj_size(const sj *j, int i) {
	return (i >= 0 && i < j->n) ? j->t[i].size : 0;
}

int sj_is_array(const sj *j, int i) {
	return i >= 0 && i < j->n && j->t[i].type == JSMN_ARRAY;
}

int sj_is_object(const sj *j, int i) {
	return i >= 0 && i < j->n && j->t[i].type == JSMN_OBJECT;
}

int sj_is_null(const sj *j, int i) {
	return i < 0 || i >= j->n || (j->t[i].type == JSMN_PRIMITIVE && j->js[j->t[i].start] == 'n');
}

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static size_t put_utf8(char *d, unsigned cp) {
	if (cp < 0x80) {
		d[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		d[0] = (char)(0xC0 | (cp >> 6));
		d[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		d[0] = (char)(0xE0 | (cp >> 12));
		d[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		d[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	d[0] = (char)(0xF0 | (cp >> 18));
	d[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	d[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	d[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

int sj_str(const sj *j, int i, char *dst, size_t cap) {
	if (cap) dst[0] = 0;
	if (i < 0 || i >= j->n || j->t[i].type != JSMN_STRING || cap == 0) return -1;
	const char *s = j->js + j->t[i].start, *e = j->js + j->t[i].end;
	size_t n = 0;
	while (s < e) {
		char c = *s++;
		char tmp[4];
		size_t w = 1;
		tmp[0] = c;
		if (c == '\\' && s < e) {
			char x = *s++;
			switch (x) {
			case 'n': tmp[0] = '\n'; break;
			case 't': tmp[0] = '\t'; break;
			case 'r': tmp[0] = '\r'; break;
			case 'b': tmp[0] = '\b'; break;
			case 'f': tmp[0] = '\f'; break;
			case 'u': {
				unsigned cp = 0;
				for (int k = 0; k < 4 && s < e; k++) cp = cp * 16 + (unsigned)(hexval(*s++) & 15);
				if (cp >= 0xD800 && cp < 0xDC00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
					unsigned lo = 0;
					for (int k = 0; k < 4; k++) lo = lo * 16 + (unsigned)(hexval(s[2 + k]) & 15);
					if (lo >= 0xDC00 && lo < 0xE000) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						s += 6;
					}
				}
				w = put_utf8(tmp, cp);
				break;
			}
			default: tmp[0] = x; break;  // \" \\ \/
			}
		}
		if (n + w >= cap) break;  // truncate (never split a character)
		memcpy(dst + n, tmp, w);
		n += w;
	}
	dst[n] = 0;
	return (int)n;
}

long sj_int(const sj *j, int i, long def) {
	if (i < 0 || i >= j->n || j->t[i].type != JSMN_PRIMITIVE) return def;
	char c = j->js[j->t[i].start];
	if (c != '-' && (c < '0' || c > '9')) return def;
	return (long)strtod(j->js + j->t[i].start, NULL);
}

double sj_num(const sj *j, int i, double def) {
	if (i < 0 || i >= j->n || j->t[i].type != JSMN_PRIMITIVE) return def;
	char c = j->js[j->t[i].start];
	if (c != '-' && (c < '0' || c > '9')) return def;
	return strtod(j->js + j->t[i].start, NULL);
}

int sj_bool(const sj *j, int i, int def) {
	if (i < 0 || i >= j->n || j->t[i].type != JSMN_PRIMITIVE) return def;
	char c = j->js[j->t[i].start];
	return c == 't' ? 1 : c == 'f' ? 0 : def;
}

int sj_eq(const sj *j, int i, const char *s) {
	return i >= 0 && i < j->n && tok_eq(j, i, s, strlen(s));
}

size_t sj_quote(char *dst, size_t cap, const char *s) {
	size_t n = 0;
	if (cap < 3) return 0;
	dst[n++] = '"';
	for (; *s && n + 7 < cap; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			dst[n++] = '\\';
			dst[n++] = (char)c;
		} else if (c < 0x20) {
			static const char hex[] = "0123456789abcdef";
			dst[n++] = '\\', dst[n++] = 'u', dst[n++] = '0', dst[n++] = '0';
			dst[n++] = hex[c >> 4], dst[n++] = hex[c & 15];
		} else {
			dst[n++] = (char)c;
		}
	}
	dst[n++] = '"';
	dst[n] = 0;
	return n;
}
