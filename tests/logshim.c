// A libretro log callback that formats its printf-style arguments before handing the
// line to Python (ctypes cannot take variadic callbacks). Built by tests/retro_log.py.
#include <stdarg.h>
#include <stdio.h>

typedef void (*sink_t)(int level, const char *message);
static sink_t sink;

void shim_set_sink(sink_t s) {
	sink = s;
}

void shim_log(int level, const char *fmt, ...) {
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (sink) sink(level, buf);
}
