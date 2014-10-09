#ifndef __COMMON_H__
#define __COMMON_H__

//#define PROFILE_GBI

#define LOG_NONE	0
#define LOG_ERROR   1
#define LOG_MINIMAL	2
#define LOG_WARNING 3
#define LOG_VERBOSE 4

#define LOG_LEVEL LOG_WARNING

# ifndef min
#  define min(a,b) ((a) < (b) ? (a) : (b))
# endif
# ifndef max
#  define max(a,b) ((a) > (b) ? (a) : (b))
# endif


#if LOG_LEVEL>0
#ifdef ANDROID_EDITION
#include <android/log.h>

#define LOG(A, ...) \
    if (A <= LOG_LEVEL) \
    { \
        __android_log_print(ANDROID_LOG_DEBUG, "gln64", __VA_ARGS__); \
    }
#else
#define LOG(A, ...) \
	if (A <= LOG_LEVEL) \
		{ \
			if (A == LOG_WARNING) printf("(WW) "); \
			if (A == LOG_ERROR) printf("(EE) "); \
			printf(__VA_ARGS__); \
			fflush(stdout); \
		}
#endif
#else

#define LOG(A, ...)

#endif

#endif
