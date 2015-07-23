/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_error.h  2015mar4  daniel.pouzzner@megaqueue.com
 * 
 * error handling and debugging aids
 */

#ifndef MQ_ERROR_H
#define MQ_ERROR_H

#include <errno.h>

#ifdef MQ_DEBUG

extern __thread int MQ_errno;
extern __thread const char *MQ_errno_file;
extern __thread int MQ_errno_line;
extern __thread const char *MQ_errno_function;

#define MQ_seterrno(x) (MQ_errno = (x), MQ_errno_file = __FILE__, MQ_errno_line = __LINE__, MQ_errno_function = __FUNCTION__, errno = (x))
#define MQ_returnerrno(x) do { MQ_errno = (x); MQ_errno_file = __FILE__; MQ_errno_line = __LINE__; MQ_errno_function = __FUNCTION__; return -(x); } while (0)
#define MQ_seterrpoint(x) (MQ_errno = (x), MQ_errno_file = __FILE__, MQ_errno_line = __LINE__, MQ_errno_function = __FUNCTION__, -(x))

#define MQ_seterrno_or_abort(x,a) (MQ_errno = (x), MQ_errno_file = __FILE__, MQ_errno_line = __LINE__, MQ_errno_function = __FUNCTION__, ((a)?MQ_abort():0), errno = (x))
#define MQ_returnerrno_or_abort(x,a) do { MQ_errno = (x); MQ_errno_file = __FILE__; MQ_errno_line = __LINE__; MQ_errno_function = __FUNCTION__; if (a) MQ_abort(); return -(x); } while (0)
#define MQ_seterrpoint_or_abort(x,a) (MQ_errno = (x), MQ_errno_file = __FILE__, MQ_errno_line = __LINE__, MQ_errno_function = __FUNCTION__, ((a)?MQ_abort():0), -(x))

#define MQ_dprintf(fmt, args...) dprintf(STDERR_FILENO, "%s@%d %s(): " fmt, __FILE__, __LINE__, __FUNCTION__, ## args)
#define MQ_dprintf2(file, line, func, fmt, args...) dprintf(STDERR_FILENO, "%s@%d %s(): " fmt, file, line, func, ## args)
#define MQ_dprintf3(fmt, args...) dprintf(STDERR_FILENO, "thrown %s@%d %s() (%d) reported %s@%d %s(): " fmt, MQ_errno_file, MQ_errno_line, MQ_errno_function, MQ_errno, __FILE__, __LINE__, __FUNCTION__, ## args)

#else

#define MQ_seterrno(x) ({errno = (x);})
#define MQ_returnerrno(x) return -(x)
#define MQ_seterrpoint(x) (-(x))

#define MQ_seterrno_or_abort(x,a) (((a)?MQ_abort():0), errno = (x))
#define MQ_returnerrno_or_abort(x,a) do { if (a) MQ_abort(); return -(x); } while(0)
#define MQ_seterrpoint_or_abort(x,a) (((a)?MQ_abort():0), -(x))

#define MQ_dprintf(fmt, args...) dprintf(STDERR_FILENO, fmt, ## args)
#define MQ_dprintf2(file, line, func, fmt, args...) dprintf(STDERR_FILENO, fmt, ## args)
#define MQ_dprintf3(fmt, args...) dprintf(STDERR_FILENO, fmt, ## args)

#endif

#endif
