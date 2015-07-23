/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_threadutil.c  2015may24  daniel.pouzzner@megaqueue.com
 */

#include "mq.h"

/* assemble a linked list of struct __MQ_ThreadStates using CAS append.  only extrication needs an exclusive lock.
 * use a separate, simplistic sharable lock for reads.
 * extrication at the tail is accomplished by first CAS-appending a global skip entry,
 * which needs to first be deleted from the head/middle of the list.
 *
 * to avoid priority inversion, the LL is always walked from the tail end, which is guaranteed to be fully connected clear to the head.
 * walkers are never blocked.  instead, each walker increments a walker count, and _Extricate is blocked on ent recycle/deallocation until the walker count is zero.
 * a walker performing an asynchronous census can remember the tail from the start of its walk, and check at the end of its walk if the tail has changed.
 * if it needs a count that is accurate for at least a moment, it can rewalk until the start and end tail match.
 */

__thread_scoped struct __MQ_ThreadState __MQ_ThreadState = { .tid = MQ_TSA_INITIALIZER, ._next = 0 };
__thread_scoped struct __MQ_ThreadState *__MQ_ThreadState_Pointer = 0;

volatile int __MQ_ThreadState_Walker_Count = 0;
static MQ_TSA_t __MQ_ThreadState_Write_Lock = MQ_TSA_INITIALIZER;
static struct __MQ_ThreadState __MQ_ThreadState_Dummy = { .tid = MQ_TSA_INITIALIZER, .RWLock_ThreadState = 0, ._prev = 0, ._next = 0 };
struct __MQ_ThreadState * volatile __MQ_ThreadState_Head = &__MQ_ThreadState_Dummy, * volatile __MQ_ThreadState_Tail = &__MQ_ThreadState_Dummy;
#define __MQ_TID_TO_THREADSTATE_TABLE_ENTS 256
static struct __MQ_ThreadState * volatile __MQ_TID_To_ThreadState_Table[__MQ_TID_TO_THREADSTATE_TABLE_ENTS] = { 0 };

union __MQ_SchedState __PeakSchedState = { .EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(0,0,0) };

/* xxx need to retool these (and move them to mq_threadutil.c) so that union __MQ_SchedulingState includes 48 bits of pointer to the source (usually a lock),
   8 bits of policy, and 8 bits of priority, and exchange all of it atomically.
   and need to have special values signifying base value (all-zeros) and use of __PeakSchedState (can be just a pointer to __PeakSchedState).

note that PI calculations can be omitted for any thread with pri equal to the Peak.
*/

int _MQ_ThreadState_Assoc_ByTID(pid_t tid, struct __MQ_ThreadState **ts) {
  MQ_THREADSTATE_WALK_START();

  *ts = MQ_SyncInt_Get(__MQ_TID_To_ThreadState_Table[tid % __MQ_TID_TO_THREADSTATE_TABLE_ENTS]);
  if (*ts && ((*ts)->tid == tid))
    return 0;

  for (struct __MQ_ThreadState *i = MQ_SyncInt_Get(__MQ_ThreadState_Tail); i; i=i->_prev) {
    if (i->tid == tid) {
      *ts = i;
      return 0;
    }
  }

  MQ_THREADSTATE_WALK_END();
  MQ_returnerrno(ESRCH);
}

int _MQ_ThreadState_Assoc_ByTID_prelocked(pid_t tid, struct __MQ_ThreadState **ts) {
  *ts = MQ_SyncInt_Get(__MQ_TID_To_ThreadState_Table[tid % __MQ_TID_TO_THREADSTATE_TABLE_ENTS]);
  if (*ts && ((*ts)->tid == tid))
    return 0;

  for (struct __MQ_ThreadState *i = MQ_SyncInt_Get(__MQ_ThreadState_Tail); i; i=i->_prev) {
    if (i->tid == tid) {
      *ts = i;
      return 0;
    }
  }

  MQ_returnerrno(ESRCH);
}

int _MQ_ThreadState_Assoc_ByPthID(pthread_t pth_id, struct __MQ_ThreadState **ts) {
  MQ_THREADSTATE_WALK_START();

  for (struct __MQ_ThreadState *i = MQ_SyncInt_Get(__MQ_ThreadState_Tail); i; i=i->_prev) {
    if (i->pth_id == pth_id) {
      *ts = i;
      return 0;
    }
  }

  MQ_THREADSTATE_WALK_END();
  MQ_returnerrno(ESRCH);
}

void _MQ_ThreadState_Assoc_Release(void) {
  MQ_THREADSTATE_WALK_END();
}

static int MQ_SchedState_Get(pid_t tid, int *policy, int *priority, void **from, int base_p) {
  int ret;

  if (! __MQ_ThreadState_Pointer)
    if ((ret=MQ_ThreadState_Init())<0)
      return ret;

  struct __MQ_ThreadState *ts;

  if (tid && (tid != __MQ_ThreadState.tid)) {
    if ((ret=_MQ_ThreadState_Assoc_ByTID(tid, &ts))<0)
      return ret;
  } else {
    ts = __MQ_ThreadState_Pointer;
    ret = 0;
  }

  union __MQ_SchedState ss = { .EnBloc = base_p ? ts->BaseSchedState.EnBloc : ts->CurSchedState.EnBloc };

  if (tid && (tid != __MQ_ThreadState.tid))
    _MQ_ThreadState_Assoc_Release();

  *policy = (int)ss.policy;
  *priority = (int)ss.priority;

  if (from)
    *from = MQ_SCHEDSTATE_POINTER(ss);

  return ret;
}

int MQ_SchedState_GetBase(pid_t tid, int *policy, int *priority) {
  return MQ_SchedState_Get(tid, policy, priority, 0, 1);
}

int MQ_SchedState_GetCur(pid_t tid, int *policy, int *priority, void **from) {
  return MQ_SchedState_Get(tid, policy, priority, from, 0);
}

int _MQ_SchedState_Elevate(struct __MQ_ThreadState *thr, const union __MQ_SchedState new_ss) {
  union __MQ_SchedState ref_ss = { .EnBloc = MQ_SyncInt_Get(thr->CurSchedState.EnBloc) };
  if (! (((ref_ss.policy == SCHED_OTHER) && (new_ss.policy != SCHED_OTHER)) ||
	 (ref_ss.priority < new_ss.priority)))
    return 0; /* MQ_returnerrno(EALREADY); */

  for (;;) {
    if (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(thr->CurSchedState.EnBloc,ref_ss.EnBloc,new_ss.EnBloc))
      break;
    if (! (((ref_ss.policy == SCHED_OTHER) && (new_ss.policy != SCHED_OTHER)) ||
	   (ref_ss.priority < new_ss.priority)))
      return 0; /* MQ_returnerrno(EALREADY); */
  }

  return _MQ_SchedState_Set_1(thr->tid, &thr->CurSchedState);
}

int _MQ_SchedState_Elevate_EvenIfEqual(struct __MQ_ThreadState *thr, const union __MQ_SchedState new_ss) {
  union __MQ_SchedState ref_ss = { .EnBloc = MQ_SyncInt_Get(thr->CurSchedState.EnBloc) };

  if (new_ss.EnBloc == ref_ss.EnBloc)
    return 0;

  if (! (((ref_ss.policy == SCHED_OTHER) && (new_ss.policy != SCHED_OTHER)) ||
	 (ref_ss.priority <= new_ss.priority)))
    return 0; /* MQ_returnerrno(EALREADY); */

  for (;;) {
    if (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(thr->CurSchedState.EnBloc,ref_ss.EnBloc,new_ss.EnBloc))
      break;
    if (! (((ref_ss.policy == SCHED_OTHER) && (new_ss.policy != SCHED_OTHER)) ||
	   (ref_ss.priority <= new_ss.priority)))
      return 0; /* MQ_returnerrno(EALREADY); */
  }

  if (ref_ss.policy_and_priority != new_ss.policy_and_priority)
    return _MQ_SchedState_Set_1(thr->tid, &thr->CurSchedState);
  else
    return 0;
}

/* used both for reversions and to carry through updates to the parent schedstate.
 * fails and updates ref_ss with current value if caller-supplied value doesn't match changed.
 */
int _MQ_SchedState_Update(struct __MQ_ThreadState *thr, union __MQ_SchedState *ref_ss, const union __MQ_SchedState new_ss) {
  if (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(thr->CurSchedState.EnBloc,ref_ss->EnBloc,new_ss.EnBloc)) {
    if (new_ss.policy_and_priority != ref_ss->policy_and_priority)
      return _MQ_SchedState_Set_1(thr->tid, &thr->CurSchedState);
    else
      return 0;
  } else
    MQ_returnerrno(EALREADY);
}

static void _PeakSchedulingState_MaybeUpdate(union __MQ_SchedState this_base_ss, int drop_p) {
  union __MQ_SchedState start_peak_ss = { .EnBloc = __PeakSchedState.EnBloc };
  union __MQ_SchedState new_peak_ss;
  union __MQ_SchedState inher_peak_ss;

 again:

  if (MQ_SCHEDSTATE_POINTER(this_base_ss) == MQ_SCHEDSTATE_POINTER(start_peak_ss)) {
    if ((! drop_p) && (this_base_ss.EnBloc == start_peak_ss.EnBloc))
      return;
    MQ_THREADSTATE_WALK_START();
    struct __MQ_ThreadState *new_peak_thread = 0;
    for (struct __MQ_ThreadState *i = MQ_SyncInt_Get(__MQ_ThreadState_Tail); i; i=i->_prev) {
      if (MQ_SCHEDSTATE_CMP(i->BaseSchedState,start_peak_ss) <= 0) {
	new_peak_thread = i;
	break;
      }
      if ((! new_peak_thread) ||
	  (MQ_SCHEDSTATE_CMP(i->BaseSchedState,new_peak_thread->BaseSchedState) <= 0))
	new_peak_thread = i;
    }

    if (! new_peak_thread) {
      MQ_THREADSTATE_WALK_END();
      return;
    }

    new_peak_ss.EnBloc = new_peak_thread->BaseSchedState.EnBloc;

    if (! MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(__PeakSchedState.EnBloc,start_peak_ss.EnBloc,new_peak_ss.EnBloc)) {
      MQ_THREADSTATE_WALK_END();
      goto again;
    }

    goto maybe_propagate;

  }

  if (drop_p)
    return;

  if (MQ_SCHEDSTATE_CMP(this_base_ss,start_peak_ss) < 0) {
    for (;;) {
      if (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(__PeakSchedState.EnBloc,start_peak_ss.EnBloc,this_base_ss.EnBloc))
	break;
      if (MQ_SCHEDSTATE_CMP(this_base_ss,start_peak_ss) >= 0)
	return;
    }
  }

  new_peak_ss.EnBloc = this_base_ss.EnBloc;

  MQ_THREADSTATE_WALK_START();

 maybe_propagate:

  /* the peak changed, so now we have to walk the thread list to
   * update any currently at peak.  if a thread is at peak but its
   * pol/pri differs from the pol/pri in start_peak_ss, only update
   * it if new_peak_ss pol/pri is higher than the thread's current
   * pri.  This resolves races between concurrent callers to
   * _PeakSchedulingState_MaybeUpdate() by assuring the call
   * resulting in the highest peak pri is the one that drives the
   * applied pri.  When one+ of those callers is drop_p, this can
   * result in some threads transiently retaining or running at pri
   * higher than the fully reconciled end state of
   * __PeakSchedState, but that is a thoroughly acceptable
   * price to pay for a hardware-lock-only internal mechanism immune
   * to priority inversions.
   */

  inher_peak_ss.EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(&__PeakSchedState, new_peak_ss.policy, new_peak_ss.priority);

  for (struct __MQ_ThreadState *i = MQ_SyncInt_Get(__MQ_ThreadState_Tail); i; i=i->_prev) {
    union __MQ_SchedState i_ss = { .EnBloc = i->CurSchedState.EnBloc };
  again_same_i:
    if (MQ_SCHEDSTATE_POINTER(i_ss) == &__PeakSchedState) {
      if ((i_ss.policy_and_priority == new_peak_ss.policy_and_priority) ||
	  (MQ_SCHEDSTATE_CMP(i_ss,new_peak_ss) < 0)) {
	if (_MQ_SchedState_Update(i, &i_ss, inher_peak_ss) == -EAGAIN)
	  goto again_same_i;
      }
    }
  }

  MQ_THREADSTATE_WALK_END();
  return;
}


static void _MQ_ThreadState_Extricate(__unused void *arg);

int MQ_ThreadState_Init(void) {
  if (__MQ_ThreadState_Pointer)
    MQ_returnerrno(EALREADY);

  int ret = 0;
  /* use the pthread thread-specific data facility for its cleanup hook */
  static pthread_key_t __MQ_ThreadState_Key;
  static uint64_t initialized_p = 0;
  if (! MQ_SyncInt_PostIncrement(initialized_p,1)) {
    if ((ret=pthread_key_create(&__MQ_ThreadState_Key,_MQ_ThreadState_Extricate)))
      MQ_returnerrno(ret);
  }
  if ((ret=pthread_setspecific(__MQ_ThreadState_Key,&__MQ_ThreadState)))
    MQ_returnerrno(ret);

  if ((ret=_MQ_RWLock_ThreadState_Init())<0)
    return ret;

  __MQ_ThreadState.tid = gettid();
  __MQ_ThreadState.pth_id = pthread_self();  
  _MQ_SchedState_Get_1(0,(union __MQ_SchedState *)&__MQ_ThreadState.BaseSchedState);
  __MQ_ThreadState.CurSchedState = __MQ_ThreadState.BaseSchedState;
//  __MQ_ThreadState.InheritedSchedulingState.policy = __MQ_ThreadState.InheritedSchedulingState.priority = -1;

  __MQ_ThreadState_Pointer = &__MQ_ThreadState;
  __MQ_ThreadState.long_tsa = (uint64_t)__MQ_ThreadState_Pointer | ((uint64_t)gettid() << 48UL);

  __MQ_ThreadState._next = 0;
  __MQ_ThreadState._prev = __MQ_ThreadState_Tail; /* set ._prev first to atomically link in the element */
  for (;;)
    if (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(__MQ_ThreadState_Tail,__MQ_ThreadState._prev,&__MQ_ThreadState))
      break;
  __MQ_ThreadState._prev->_next = &__MQ_ThreadState; /* potential priority inversion while an exiting thread waits for this to be set; not worth worrying about */

  MQ_SyncInt_ExchangeIfEq(__MQ_TID_To_ThreadState_Table[__MQ_ThreadState.tid % __MQ_TID_TO_THREADSTATE_TABLE_ENTS], (struct __MQ_ThreadState *)0, __MQ_ThreadState_Pointer);

  union __MQ_SchedState init_ss = { .EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(__MQ_ThreadState_Pointer,__MQ_ThreadState.BaseSchedState.policy,__MQ_ThreadState.BaseSchedState.priority) };
  _PeakSchedulingState_MaybeUpdate(init_ss,0);

  return 0;
}

int _MQ_ThreadState_Count(void) {
  int ret = 0;
  MQ_THREADSTATE_WALK_START();

  struct __MQ_ThreadState *start_tail = __MQ_ThreadState_Tail;
  for (struct __MQ_ThreadState *i = start_tail; i; i=i->_prev) {
    if (i->tid == MQ_TSA_INITIALIZER)
      continue;
    ++ret;
  }

  MQ_THREADSTATE_WALK_END();
  return ret;
}

static void _MQ_ThreadState_Extricate(__unused void *arg) {
  MQ_SyncInt_ExchangeIfEq(__MQ_TID_To_ThreadState_Table[__MQ_ThreadState.tid % __MQ_TID_TO_THREADSTATE_TABLE_ENTS], __MQ_ThreadState_Pointer, (struct __MQ_ThreadState *)0);

  if (__MQ_ThreadState.CurSchedState.policy_and_priority != __PeakSchedState.policy_and_priority)
    (void)MQ_SchedState_AdoptPeak(); /* don't allow priority inversions to cause a thread to stick around unnecessarily */

  _MQ_RWLock_ThreadCleanup();

  while (! MQ_SyncInt_ExchangeIfEq(__MQ_ThreadState_Write_Lock,MQ_TSA_INITIALIZER,MQ_TSA)) {
    START_NOCANCELS;
    usleep(1000); /* this thread is done working, so we're in no hurry */
    END_NOCANCELS;
  }
  /* if we're still at the tail, (carefully) move the dummy ent to the tail before extricating */
  if (__MQ_ThreadState_Tail == &__MQ_ThreadState) {
    while (! __MQ_ThreadState_Dummy._next) {
      START_NOCANCELS;
      usleep(1000); /* still in no hurry */
      END_NOCANCELS;
    }
    __MQ_ThreadState_Dummy._next->_prev = __MQ_ThreadState_Dummy._prev;
    if (__MQ_ThreadState_Dummy._prev)
      __MQ_ThreadState_Dummy._prev->_next = __MQ_ThreadState_Dummy._next;
    else
      __MQ_ThreadState_Head = __MQ_ThreadState_Dummy._next;
    while (MQ_SyncInt_Get(__MQ_ThreadState_Walker_Count)) {
      START_NOCANCELS;
      usleep(1000); /* still in no hurry */
      END_NOCANCELS;
    }
    __MQ_ThreadState_Dummy._next = 0;
    __MQ_ThreadState_Dummy._prev = __MQ_ThreadState_Tail; /* we may not be at the tail anymore at this point, but we're committed to move the dummy ent */
    for (;;)
      if (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(__MQ_ThreadState_Tail,__MQ_ThreadState_Dummy._prev,&__MQ_ThreadState_Dummy))
	break;
    __MQ_ThreadState_Dummy._prev->_next = &__MQ_ThreadState_Dummy; /* __MQ_ThreadState_Dummy._prev does not necessarily equal &__MQ_ThreadState (see prev. comment) */
  }

  /* mitigate race to assign forward link */
  while (! __MQ_ThreadState._next) {
    START_NOCANCELS;
    usleep(1000); /* still in no hurry */
    END_NOCANCELS;
  }

  __MQ_ThreadState._next->_prev = __MQ_ThreadState._prev;
  if (__MQ_ThreadState._prev)
    __MQ_ThreadState._prev->_next = __MQ_ThreadState._next;
  else
    __MQ_ThreadState_Head = __MQ_ThreadState._next;

  union __MQ_SchedState end_ss = { .EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(__MQ_ThreadState_Pointer,__MQ_ThreadState.BaseSchedState.policy,__MQ_ThreadState.BaseSchedState.priority) };
  _PeakSchedulingState_MaybeUpdate(end_ss,1);

  MQ_SyncInt_Fence_Full(); /* be perfectly sure the above LL updates are visible to all other threads before checking and waiting for concurrent walkers */

  while (MQ_SyncInt_Get(__MQ_ThreadState_Walker_Count)) {
    START_NOCANCELS;
    usleep(1000); /* still in no hurry */
    END_NOCANCELS;
  }

  MQ_SyncInt_Put(__MQ_ThreadState_Write_Lock,MQ_TSA_INITIALIZER);
}




int MQ_SchedState_Set(pid_t tid, int policy, int priority) {
  int ret, setfailret = 0;

  if (! __MQ_ThreadState_Pointer)
    if ((ret=MQ_ThreadState_Init())<0)
      return ret;

  struct __MQ_ThreadState *ts;

  if (tid && (tid != __MQ_ThreadState.tid)) {
    if ((ret=_MQ_ThreadState_Assoc_ByTID(tid, &ts))<0)
      return ret;
  } else
    ts = __MQ_ThreadState_Pointer;

  union __MQ_SchedState new_bss = { .EnBloc = MQ_SCHEDSTATE_MK_ENBLOC(0 /* ptr */, policy<0?ts->BaseSchedState.policy:policy, policy<0?ts->BaseSchedState.priority:priority) };

  if (ts->BaseSchedState.EnBloc == new_bss.EnBloc)
    return 0;

  union __MQ_SchedState start_ss = { .EnBloc = ts->BaseSchedState.EnBloc };
  MQ_SyncInt_Put(ts->BaseSchedState.EnBloc, new_bss.EnBloc);

  union __MQ_SchedState cur_ss;
 revert:
  cur_ss.EnBloc = ts->CurSchedState.EnBloc;
  if ((! MQ_SCHEDSTATE_POINTER(cur_ss)) || (MQ_SCHEDSTATE_CMP(cur_ss,ts->BaseSchedState) > 0)) {
    for (;;) {
      if (MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(ts->CurSchedState.EnBloc,cur_ss.EnBloc,ts->BaseSchedState.EnBloc)) {
	ret = _MQ_SchedState_Set_1(tid, &__MQ_ThreadState.BaseSchedState);
	if (ret && (ts->BaseSchedState.EnBloc != start_ss.EnBloc)) {
	  setfailret = ret;
	  MQ_SyncInt_Put(ts->BaseSchedState.EnBloc, start_ss.EnBloc);
	  goto revert;
	}

	break;
      }
      if (MQ_SCHEDSTATE_POINTER(cur_ss) && (MQ_SCHEDSTATE_CMP(cur_ss,ts->BaseSchedState) < 0)) {
	ret = 0;
	break;
      }
    }
  } else
    ret = 0;

  if (tid && (tid != __MQ_ThreadState.tid))
    _MQ_ThreadState_Assoc_Release();


  if ((MQ_SCHEDSTATE_POINTER(__PeakSchedState) == ts) ||
      (MQ_SCHEDSTATE_CMP(ts->BaseSchedState,__PeakSchedState)<0)) {
    /* if the current peak is ts and we elevated, or the current peak is not ts, we just elevate the peak if necessary to match ts,
       else we have to walk the thread list for the new peak.
    */       

  }



  if (ret || (! setfailret))
    return ret;
  else
    return setfailret;
}
