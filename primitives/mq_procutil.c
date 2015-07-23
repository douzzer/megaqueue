/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_procutil.c  2015mar20  daniel.pouzzner@megaqueue.com
 * 
 * routines for IPC, and accessing and manipulating process attributes
 */

#include "mq.h"

#include <sys/syscall.h>
#include <sched.h>
#include <sys/resource.h>

#ifndef MQ_NO_PTHREADS
__thread_scoped pid_t __my_tid = -1;
#endif

__wur pid_t _gettid(void) {
#ifdef MQ_NO_PTHREADS
  return (pid_t)syscall((uint64_t)__NR_gettid);
#else
  if (__my_tid == -1)
    __my_tid = (pid_t)syscall((uint64_t)__NR_gettid);
  return __my_tid;
#endif
}

int _MQ_SchedState_Get_1(pid_t tid, union __MQ_SchedState *ss) {
  int64_t ret;

  if ((ret = sched_getscheduler(tid)) < 0)
    MQ_returnerrno(errno);
  ss->policy = (uint8_t)ret;

  if (ss->policy == (uint8_t)SCHED_OTHER) {
    if ((ret = syscall((uint64_t)__NR_getpriority,PRIO_PROCESS,tid))<0) /* indirect syscall to avoid nonsense with negative integers being valid return values */
      MQ_returnerrno(errno);
    ss->priority = (uint8_t)ret;
  } else {
    struct sched_param sp;
    if (sched_getparam(tid,&sp)<0)
      MQ_returnerrno(errno);
    ss->priority = (uint8_t)sp.sched_priority;
  }
  return 0;
}

static int _MQ_SchedState_Set_Raw(pid_t tid, const union __MQ_SchedState ss) {
  if (ss.policy == SCHED_OTHER) {
    if (setpriority(PRIO_PROCESS,(id_t)tid,20-(int)ss.priority)<0)
      MQ_returnerrno(errno);
  }

  struct sched_param sp;
  if (ss.policy == SCHED_OTHER)
    sp.sched_priority = 0;
  else
    sp.sched_priority = (int)ss.priority;

  if (sched_setscheduler(tid, (int)ss.policy, &sp)<0)
    MQ_returnerrno(errno); /* there's no plausible way setting the scheduler to OTHER can fail if
			    * setpriority() succeeded above, so no need to code to undo the setpriority().
			    */

  return 0;
}

/* thread safety provided:
 * concurrent and conflicting updates to *ss are guaranteed to settle
 * with the last setting being the one that determines the actual
 * kernel schedstate.
 */
int _MQ_SchedState_Set_1(pid_t tid, const volatile union __MQ_SchedState *ss) {
  union __MQ_SchedState new_ss = { .EnBloc = ss->EnBloc };
  int ret;

  for (;;) {
    if ((ret=_MQ_SchedState_Set_Raw(tid, new_ss))<0)
      return ret;

    union __MQ_SchedState check_ss = { .EnBloc = MQ_SyncInt_Get(ss->EnBloc) };

    if (check_ss.EnBloc == new_ss.EnBloc)
      return 0;

    if ((check_ss.policy == new_ss.policy) &&
	((check_ss.priority == new_ss.priority)))
      return 0;

    new_ss.EnBloc = check_ss.EnBloc;

    if ((ret=_MQ_SchedState_Get_1(tid, &check_ss))<0)
      return ret;

    if ((check_ss.policy == new_ss.policy) && (check_ss.priority == new_ss.priority))
      return 0;
  }

  __unreachable;
}
