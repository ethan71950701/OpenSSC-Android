/* Host-test stand-in for <android/log.h>. */
#pragma once
enum { ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5 };
int __android_log_write(int prio, const char *tag, const char *text);
