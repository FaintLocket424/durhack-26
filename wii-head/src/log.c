#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "log.h"

static mutex_t  lock;
static char     lines[LOG_LINES][LOG_COLS + 1];
static unsigned count;					// total events ever logged
static u64      t0;

//---------------------------------------------------------------------------------
void log_init(void) {
//---------------------------------------------------------------------------------
	LWP_MutexInit(&lock, false);
	t0 = gettime();
}

//---------------------------------------------------------------------------------
void log_event(const char *fmt, ...) {
//---------------------------------------------------------------------------------
	char msg[LOG_COLS + 1];
	unsigned ms = (unsigned)ticks_to_millisecs(gettime() - t0);

	int n = snprintf(msg, sizeof msg, "%4u.%01us ", ms / 1000, (ms % 1000) / 100);
	if (n < 0) n = 0;
	if (n < (int)sizeof msg) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(msg + n, sizeof msg - (size_t)n, fmt, ap);
		va_end(ap);
	}

	LWP_MutexLock(lock);
	memcpy(lines[count % LOG_LINES], msg, sizeof msg);
	count++;
	LWP_MutexUnlock(lock);
}

//---------------------------------------------------------------------------------
int log_copy(char out[][LOG_COLS + 1], int max) {
//---------------------------------------------------------------------------------
	LWP_MutexLock(lock);
	unsigned avail = count < LOG_LINES ? count : LOG_LINES;
	unsigned n = (unsigned)max < avail ? (unsigned)max : avail;
	for (unsigned i = 0; i < n; i++)
		memcpy(out[i], lines[(count - n + i) % LOG_LINES], LOG_COLS + 1);
	LWP_MutexUnlock(lock);
	return (int)n;
}
