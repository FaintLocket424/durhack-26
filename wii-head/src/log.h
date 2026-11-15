//---------------------------------------------------------------------------------
// log.h - small ring buffer of timestamped events (connects, drops, errors) that
// the status screen shows under the node table. Safe to call from any thread.
//---------------------------------------------------------------------------------
#ifndef LOG_H
#define LOG_H

#define LOG_COLS	74					// characters per line, timestamp included
#define LOG_LINES	32					// how many events are kept

void log_init(void);					// call once, before any worker starts
void log_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Copies up to max of the newest events, oldest first. Returns how many.
int  log_copy(char out[][LOG_COLS + 1], int max);

#endif
