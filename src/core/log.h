/* Logging to stderr: "[   1.234] I tag: message". Thread safe. */
#pragma once

enum log_level { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };

void log_set_level(enum log_level level);
enum log_level log_get_level(void);
void log_msg(enum log_level level, const char *tag, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#define LOGE(tag, ...) log_msg(LOG_ERROR, tag, __VA_ARGS__)
#define LOGW(tag, ...) log_msg(LOG_WARN, tag, __VA_ARGS__)
#define LOGI(tag, ...) log_msg(LOG_INFO, tag, __VA_ARGS__)
#define LOGD(tag, ...) log_msg(LOG_DEBUG, tag, __VA_ARGS__)
