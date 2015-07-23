/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * Redistribution and use of the below source code, in whole or in
 * part must include this copyright notice, and must include and
 * accord with the terms set forth in the associated LICENSE file.
 *
 * mq_rwlock.h  2015apr13  daniel.pouzzner@megaqueue.com
 * 
 * public prototypes and macros portion of:
 *
 * LWP locks and signals with parsimonious userspace-only logic in
 * uncontended scenarios, reliable order-of-arrival queueing of
 * waiters (optionally, priority-ordered), mutex-weight W and R locks,
 * error checking on all locks, priority inheritance on RW locks,
 * integrated usage statistics, optimized contended scenario handling
 * with parsimonious syscalling, and integrated cond signalling with
 * reliable argument passing and reliable control of number of woken
 * threads
 */

#ifndef MQ_RWLOCK_H
#define MQ_RWLOCK_H

#if (! defined(MQ_RWLOCK_DEBUGGING_SUPPORT)) && (defined(MQ_RWLOCK_REPORTINVERSIONS_SUPPORT) || defined(MQ_RWLOCK_AVERTINVERSIONS_SUPPORT))
#define MQ_RWLOCK_DEBUGGING_SUPPORT
#endif

struct MQ_Lock_LFList_Ent;
struct MQ_Lock_Wait_Ent;

#ifndef MQ_RWLOCK_ADAPTIVE_MAX_SPINS
#define MQ_RWLOCK_ADAPTIVE_MAX_SPINS 400 /* a syscall costs about 750 cycles on Linux, and a spin costs two cycles, so 400 is about as high as we'd ever want to go */
#endif
#define MQ_RWLOCK_ADAPTIVE_INIT_SPINS (MQ_RWLOCK_ADAPTIVE_MAX_SPINS>>1)

#define MQ_RWLOCK_INTELRTM_MAX_RETRIES 3

#define MQ_RWLOCK_MAX_TRACKED 32

union MQ_LockCore {
  uint64_t EnBloc;
  struct {
#define MQ_RWLOCK_COUNT_INITIALIZER 0xffff0000U /* -0x40000000 */
#define MQ_RWLOCK_COUNT_VALID_P(x) (x<MQ_RWLOCK_COUNT_INITIALIZER)
#if __BYTE_ORDER == __LITTLE_ENDIAN
    int32_t Owner;
    uint32_t Count;
#define MQ_LockCore_MkEnBloc(owner,count) ((((uint64_t)(owner))<<32UL) | ((uint64_t)(count)))
#define MQ_RWLOCK_COUNT_MASK (0xffffffffUL<<32UL)
#define MQ_RWLOCK_COUNT_TO_ENBLOC(x) ((uint64_t)(x)<<32UL)
#define MQ_MUTEX_CORE_INITIALIZER (((uint64_t)((uint32_t)MQ_TSA_INVALIDOWNER))|(((uint64_t)MQ_RWLOCK_COUNT_INITIALIZER<<32UL)))
#define MQ_RWLOCK_CORE_INITIALIZER (((uint64_t)((uint32_t)MQ_TSA_INVALIDOWNER))|(((uint64_t)MQ_RWLOCK_COUNT_CONTENDED<<32UL)))
#else
    uint32_t Count;
    int32_t Owner;
#define MQ_LockCore_MkEnBloc(owner,count) ((((uint64_t)(count))<<32UL) | ((uint64_t)(owner)))
#define MQ_RWLOCK_COUNT_MASK 0xffffffffUL
#define MQ_RWLOCK_COUNT_TO_ENBLOC(x) ((uint64_t)(x))
#define MQ_MUTEX_CORE_INITIALIZER ((((uint64_t)MQ_TSA_INVALIDOWNER)<<32UL)|(uint64_t)MQ_RWLOCK_COUNT_INITIALIZER)
#define MQ_RWLOCK_CORE_INITIALIZER ((((uint64_t)MQ_TSA_INVALIDOWNER)<<32UL)|(uint64_t)MQ_RWLOCK_COUNT_CONTENDED)
#endif
  };
};

typedef struct MQ_RWLock {
#define MQ_RWLOCK_RECURSION (1UL<<0) /* allow one TID to hold multiple mutex or write locks on a single lock */
#define MQ_RWLOCK_ADAPTIVE (1UL<<1) /* Kaz Kylheku's algorithm for opportunistic dynamically tuned spin mutex acquisition in lieu of FUTEX_WAIT */
#define MQ_RWLOCK_READOPTIMAL (1UL<<2) /* TODO -- obtain uncontended concurrent read locks thread-locally, while write lockers have to walk the thread list, inspired by Pedro Ramalhete at concurrencyfreaks.blogspot.com */
#define MQ_RWLOCK_PRIORITYORDER (1UL<<3) /* grant contended read and write locks and cond waits in order of thread priority rather than FIFO */
#define MQ_RWLOCK_INHERITPRIORITY (1UL<<4) /* lock holders temporarily take on the highest priority of the threads blocked on them, to eliminate priority inversion */
#define MQ_RWLOCK_INHERITPEAKPRIORITY (1UL<<5) /* lock holders take on the highest priority of any thread, to eliminate core starvation in critical sections */
#define MQ_RWLOCK_EXTRAERRORCHECKING (1UL<<6) /* adds entry point assertions of lock _INITED and !_CORRUPTED, and MQ_Mutex_Lock() EBUSY when W/R locked (not runtime switchable) */
#define MQ_RWLOCK_NOCOSTLYERRORCHECKING (1UL<<7) /* omits checks for unlock validity (not runtime-switchable) */
#define MQ_RWLOCK_ROBUST (1UL<<8) /* mostly TODO -- automatically detect and try to recover from threads holding MQ_RWLock resources at their termination */
#define MQ_RWLOCK_PROCESS_SHARED (1UL<<9) /* TODO -- work correctly between distinct memory contexts (across heavyweight process boundaries) */
#define MQ_RWLOCK_STATISTICS (1UL<<10) /* maintain futex_trips and the Reqs/Waits/Fails counts (not compatible with _INTELRTM) */
#define MQ_RWLOCK_DEBUGGING (1UL<<11) /* track client code entry points */
#define MQ_RWLOCK_REPORTINVERSIONS (1UL<<12) /* TODO -- detect and report out of order lock access (print to stderr) */
#define MQ_RWLOCK_AVERTINVERSIONS (1UL<<13) /* TODO -- detect and prevent out of order lock access (return EDEADLK) */
#define MQ_RWLOCK_ABORTWHENCORRUPTED (1UL<<14) /* rather than ENOTRECOVERABLE, SIGABRT preserving the stack(s) at the point of discovery */
#define MQ_RWLOCK_INTELRTM (1UL<<15) /* use Intel Restricted Transactional Memory in uncontended scenarios */
#define MQ_RWLOCK_NOLOCKTRACKING (1UL<<16) /* very fast, but incompatible with _INHERIT*PRIORITY, _*INVERSIONS, and _INTELRTM, and makes MQ_R_HaveLock_p() always false */
#define MQ_RWLOCK_ABORTONMISUSE (1UL<<17) /* abort instead of returning EINVAL, EPERM, ENOLCK, or ENOTSUP, or EBUSY in MQ_RWLock_Destroy() (preserve stack at point of detection) */
#define MQ_RWLOCK_ABORTONEDEADLK (1UL<<18) /* abort instead of returning EDEADLK (preserve stack at point of detection) */
#define MQ_RWLOCK_MAXUSERFLAG (1UL<<18)
#define MQ_RWLOCK_DUMMY (1UL<<58) /* this is not actually a lock, only the flags slot is present -- used for tracking non-inherited peak priority */
#define MQ_RWLOCK_WAITLIST_DIRTY (1UL<<59) /* tracks lifecycle of Wait_List updates */
#define MQ_RWLOCK_WAITLIST_CORRUPTED (1UL<<60) /* set when an internal routine leaves the main Wait_List in an unreliable state (untimely thread termination) */
#define MQ_RWLOCK_LFLIST_CORRUPTED (1UL<<61) /* set when an internal routine leaves the LFList in an unreliable state (untimely thread termination) */
#define MQ_RWLOCK_INACTIVE (1UL<<62) /* lock inactive -- lock attempts will produce ECANCELED until the inactivater reactivates the lock */
#define MQ_RWLOCK_INITED (1UL<<63) /* try to track if lock is initialized */
#define MQ_RWLOCK_DEFAULTFLAGS (~0UL) /* never makes sense to pass in the internal flags, so this works fine. */
  volatile uint64_t Flags;
  const uint64_t *BuildFlags;

  volatile uint64_t ID;

  volatile union __MQ_SchedState SchedState; /* an elevated priority, if any, associated with this lock and its holder(s), to be relinquished by each holder (and
					      * if applicable transferred to the new holder(s)) at unlock time
					      */

  volatile union MQ_LockCore MutEx;
  struct MQ_Lock_LFList_Ent * volatile LFList_Head, * volatile LFList_Tail;
  volatile pid_t LFList_Head_Waiter, LFList_Tail_Waiter;
  volatile MQ_TSA_t LFList_Head_Lock;
  volatile int MutEx_Spin_Estimator; /* for MQ_RWLOCK_ADAPTIVE */
  volatile int MutEx_Recursion_Depth; /* for MQ_RWLOCK_RECURSION */

#define MQ_RWLOCK_COUNT_CONTENDED (1U<<31)
#define MQ_RWLOCK_COUNT_READ (1U<<30)
#define MQ_RWLOCK_COUNT_MINFLAG (1U<<30)
  volatile union MQ_LockCore RW;
  struct MQ_Lock_Wait_Ent * volatile Wait_List_Head, * volatile Wait_List_Tail;
  volatile int Wait_List_Length;
  volatile int Shared_Futex_Granted, Shared_Futex_RefCount;

#if defined(MQ_RWLOCK_STATISTICS_SUPPORT) || defined(MQ_RWLOCK_DEBUGGING_SUPPORT)

  volatile uint64_t futex_trips;
  volatile uint64_t M_Reqs, M_Adaptives, M_Waits, M_Fails, R_Reqs, R_Waits, R_Fails, W_Reqs, W_Waits, W_Fails, R2W_Reqs, R2W_Waits, R2W_Fails, C_Reqs, C_Fails, S_Reqs, S_Misses, S_Fails;
  double M_Wait_Time_Cum, R_Wait_Time_Cum, W_Wait_Time_Cum, R2W_Wait_Time_Cum;
  struct MQ_RWLock * volatile by_thread_prev, * volatile by_thread_next, * volatile by_lockinst_prev, * volatile by_lockinst_next;
  const char *init_file;
  int init_line;
  const char *init_function;
  const char *last_M_Locker_file;
  int last_M_Locker_line;
  const char *last_M_Locker_function;
  pid_t last_M_Locker_tid;
  const char *last_M_Unlocker_file;
  int last_M_Unlocker_line;
  const char *last_M_Unlocker_function;
  pid_t last_M_Unlocker_tid;
  const char *last_W_Locker_file;
  int last_W_Locker_line;
  const char *last_W_Locker_function;
  pid_t last_W_Locker_tid;
  const char *last_W_Unlocker_file;
  int last_W_Unlocker_line;
  const char *last_W_Unlocker_function;
  pid_t last_W_Unlocker_tid;
  const char *last_fresh_R_Locker_file;
  int last_fresh_R_Locker_line;
  const char *last_fresh_R_Locker_function;
  pid_t last_fresh_R_Locker_tid;
  const char *last_R_Locker_file;
  int last_R_Locker_line;
  const char *last_R_Locker_function;
  pid_t last_R_Locker_tid;
  const char *last_R_Unlocker_file;
  int last_R_Unlocker_line;
  const char *last_R_Unlocker_function;
  pid_t last_R_Unlocker_tid;

#endif

} MQ_RWLock_t;

extern int _MQ_RWLock_LockState_Initialize(MQ_RWLock_t *l);
extern void _MQ_RWLock_LockState_Extricate(MQ_RWLock_t *l);

extern volatile uint64_t __MQ_RWLock_GlobalDefaultFlags;

#define MQ_RWLOCK_SUPPORTNEEDED_FLAGS (MQ_RWLOCK_INHERITPEAKPRIORITY|MQ_RWLOCK_RECURSION|MQ_RWLOCK_EXTRAERRORCHECKING|MQ_RWLOCK_DEBUGGING|MQ_RWLOCK_REPORTINVERSIONS|MQ_RWLOCK_AVERTINVERSIONS|MQ_RWLOCK_STATISTICS|MQ_RWLOCK_INTELRTM|MQ_RWLOCK_NOLOCKTRACKING)

#define MQ_RWLOCK_NOSET_FLAGS (MQ_RWLOCK_INHERITPEAKPRIORITY|MQ_RWLOCK_EXTRAERRORCHECKING|MQ_RWLOCK_NOCOSTLYERRORCHECKING|MQ_RWLOCK_INTELRTM|MQ_RWLOCK_NOLOCKTRACKING|~((MQ_RWLOCK_MAXUSERFLAG<<1)-1UL))
#define MQ_RWLOCK_NOCLEAR_FLAGS (MQ_RWLOCK_INHERITPRIORITY|MQ_RWLOCK_INHERITPEAKPRIORITY|MQ_RWLOCK_EXTRAERRORCHECKING|MQ_RWLOCK_NOCOSTLYERRORCHECKING|MQ_RWLOCK_INTELRTM|MQ_RWLOCK_NOLOCKTRACKING|~((MQ_RWLOCK_MAXUSERFLAG<<1)-1UL))

#if defined(MQ_RWLOCK_NOLOCKTRACKING_SUPPORT) && (defined(MQ_RWLOCK_INHERITPRIORITY) || defined(MQ_RWLOCK_INHERITPEAKPRIORITY) || defined(MQ_RWLOCK_REPORTINVERSIONS_SUPPORT) || defined(MQ_RWLOCK_AVERTINVERSIONS_SUPPORT) || defined(MQ_RWLOCK_INTELRTM_SUPPORT))
#error MQ_RWLOCK_NOLOCKTRACKING_SUPPORT is incompatible with _INHERITPRIORITY, _INHERITPEAKPRIORITY, _REPORTINVERSIONS, _AVERTINVERSIONS, and _INTELRTM
#endif

#if defined(MQ_RWLOCK_STATISTICS_SUPPORT) && defined(MQ_RWLOCK_INTELRTM_SUPPORT)
#error MQ_RWLOCK_STATISTICS_SUPPORT is incompatible with MQ_RWLOCK_INTELRTM_SUPPORT
#endif

#if defined(MQ_RWLOCK_INTELRTM_SUPPORT) && (! defined(__RTM__))
#error MQ_RWLOCK_INTELRTM_SUPPORT without RTM target support (need -mrtm?)
#endif

static const uint64_t __MQ_RWLock_LocalBuildFlags =
  (0UL
#ifdef MQ_RWLOCK_PRIORITYORDER_SUPPORT
   | MQ_RWLOCK_PRIORITYORDER
#endif
#ifdef MQ_RWLOCK_INHERITPRIORITY_SUPPORT
   | MQ_RWLOCK_INHERITPRIORITY
#endif
#ifdef MQ_RWLOCK_INHERITPEAKPRIORITY_SUPPORT
   | MQ_RWLOCK_INHERITPEAKPRIORITY
#endif
#ifdef MQ_RWLOCK_RECURSION_SUPPORT
   | MQ_RWLOCK_RECURSION
#endif
#ifdef MQ_RWLOCK_ADAPTIVE_SUPPORT
   | MQ_RWLOCK_ADAPTIVE
#endif
#ifdef MQ_RWLOCK_EXTRAERRORCHECKING_SUPPORT
   | MQ_RWLOCK_EXTRAERRORCHECKING
#endif
#ifdef MQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT
   | MQ_RWLOCK_NOCOSTLYERRORCHECKING
#endif
#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
   | MQ_RWLOCK_DEBUGGING
#endif
#ifdef MQ_RWLOCK_ABORTWHENCORRUPTED_SUPPORT
   | MQ_RWLOCK_ABORTWHENCORRUPTED
#endif
#ifdef MQ_RWLOCK_ABORTONMISUSE_SUPPORT
   | MQ_RWLOCK_ABORTONMISUSE
#endif
#ifdef MQ_RWLOCK_ABORTONEDEADLK_SUPPORT
   | MQ_RWLOCK_ABORTONEDEADLK
#endif
#ifdef MQ_RWLOCK_REPORTINVERSIONS_SUPPORT
   | MQ_RWLOCK_REPORTINVERSIONS
#endif
#ifdef MQ_RWLOCK_AVERTINVERSIONS_SUPPORT
   | MQ_RWLOCK_AVERTINVERSIONS
#endif
#ifdef MQ_RWLOCK_STATISTICS_SUPPORT
   | MQ_RWLOCK_STATISTICS
#endif
#ifdef MQ_RWLOCK_ROBUST_SUPPORT
   | MQ_RWLOCK_ROBUST
#endif
#ifdef MQ_RWLOCK_INTELRTM_SUPPORT
   | MQ_RWLOCK_INTELRTM
#endif
#ifdef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
   | MQ_RWLOCK_NOLOCKTRACKING
#endif
  );

#ifndef MQ_RWLOCK_LOCALDEFAULTFLAGS
#define MQ_RWLOCK_LOCALDEFAULTFLAGS __MQ_RWLock_LocalBuildFlags
#endif

#define __MQ_RWLock_LocalBuildCapMask ((((MQ_RWLOCK_MAXUSERFLAG<<1)-1UL) & ~MQ_RWLOCK_SUPPORTNEEDED_FLAGS) | __MQ_RWLock_LocalBuildFlags)

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT
#  define MQ_RWLOCK_INITIALIZER(flags) { .ID = 0, .MutEx.EnBloc = MQ_MUTEX_CORE_INITIALIZER, .MutEx_Recursion_Depth = 0, .MutEx_Spin_Estimator = MQ_RWLOCK_ADAPTIVE_INIT_SPINS, .RW.EnBloc = MQ_RWLOCK_CORE_INITIALIZER, .LFList_Head = 0, .LFList_Tail = 0, .LFList_Head_Waiter = 0, .LFList_Tail_Waiter = 0, .LFList_Head_Lock = 0, .Wait_List_Head = 0, .Wait_List_Tail = 0, .Wait_List_Length = 0, .Flags = MQ_RWLOCK_INITED|(((flags) == MQ_RWLOCK_DEFAULTFLAGS) ? (MQ_RWLOCK_LOCALDEFAULTFLAGS|(__MQ_RWLock_GlobalDefaultFlags&__MQ_RWLock_LocalBuildCapMask)) : ((flags)&__MQ_RWLock_LocalBuildCapMask)), .BuildFlags = &__MQ_RWLock_LocalBuildFlags, .Shared_Futex_Granted = 0, .Shared_Futex_RefCount = 0, .init_file = __FILE__, .init_line = __LINE__, .init_function = __FUNCTION__ }
#else
#  define MQ_RWLOCK_INITIALIZER(flags) { .ID = 0, .MutEx.EnBloc = MQ_MUTEX_CORE_INITIALIZER, .MutEx_Recursion_Depth = 0, .MutEx_Spin_Estimator = MQ_RWLOCK_ADAPTIVE_INIT_SPINS, .RW.EnBloc = MQ_RWLOCK_CORE_INITIALIZER, .LFList_Head = 0, .LFList_Tail = 0, .LFList_Head_Waiter = 0, .LFList_Tail_Waiter = 0, .LFList_Head_Lock = 0, .Wait_List_Head = 0, .Wait_List_Tail = 0, .Wait_List_Length = 0, .Flags = MQ_RWLOCK_INITED|(((flags) == MQ_RWLOCK_DEFAULTFLAGS) ? (MQ_RWLOCK_LOCALDEFAULTFLAGS|(__MQ_RWLock_GlobalDefaultFlags&__MQ_RWLock_LocalBuildCapMask)) : ((flags)&__MQ_RWLock_LocalBuildCapMask)), .BuildFlags = &__MQ_RWLock_LocalBuildFlags, .Shared_Futex_Granted = 0, .Shared_Futex_RefCount = 0 }
#endif

#ifdef MQ_RWLOCK_INLINE
#define _MQ_RWLOCK_MAYBE_INLINE static __always_inline
#else
#define _MQ_RWLOCK_MAYBE_INLINE extern
#endif

typedef enum { MQ_LOCK_WAIT_R = 1, MQ_WAIT_TYPE_MIN = 1, MQ_LOCK_WAIT_W = 2, MQ_LOCK_WAIT_R2W = 4, MQ_COND_WAIT_R = 8, MQ_COND_WAIT_R_RETUNLOCKED = 16, MQ_COND_WAIT_W = 32, MQ_COND_WAIT_W_RETUNLOCKED = 64, MQ_COND_WAIT_MUTEX = 128, MQ_COND_WAIT_MUTEX_RETUNLOCKED = 256, MQ_WAIT_TYPE_MAX = 256 } MQ_Wait_Type;

_MQ_RWLOCK_MAYBE_INLINE int MQ_RWLock_Destroy(MQ_RWLock_t *l);
_MQ_RWLOCK_MAYBE_INLINE int MQ_RWLock_SetDefaultFlags(uint64_t SetFlags);
_MQ_RWLOCK_MAYBE_INLINE int MQ_RWLock_ClearDefaultFlags(uint64_t ClearFlags);
_MQ_RWLOCK_MAYBE_INLINE int MQ_RWLock_SetFlags(MQ_RWLock_t *l,uint64_t SetFlags);
_MQ_RWLOCK_MAYBE_INLINE int MQ_RWLock_ClearFlags(MQ_RWLock_t *l,uint64_t ClearFlags);

#ifdef MQ_RWLOCK_DEBUGGING_SUPPORT

#define MQ_RWLock_Init(l,flags) _MQ_RWLock_Init(l,flags,1,__FILE__,__LINE__,__FUNCTION__)
#define MQ_RWLock_Init_PreZeroed(l,flags) _MQ_RWLock_Init(l,flags,0,__FILE__,__LINE__,__FUNCTION__)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_RWLock_Init(MQ_RWLock_t *l, uint64_t flags, int zero_p, const char *file, const int line, const char *func);

#define MQ_Mutex_Lock(l,wait_nsecs) _MQ_Mutex_Lock(l, wait_nsecs, __FILE__, __LINE__, __FUNCTION__)
__wur _MQ_RWLOCK_MAYBE_INLINE int64_t _MQ_Mutex_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func);

#define MQ_Mutex_Unlock(l) _MQ_Mutex_Unlock(l, __FILE__, __LINE__, __FUNCTION__)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_Mutex_Unlock(MQ_RWLock_t *l, const char *file, const int line, const char *func);

#define MQ_Mutex_Lock_Fast(l,wait_nsecs) _MQ_Mutex_Lock_Fast(l, wait_nsecs, __FILE__, __LINE__, __FUNCTION__)
__wur _MQ_RWLOCK_MAYBE_INLINE int64_t _MQ_Mutex_Lock_Fast(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func);

#define MQ_Mutex_Unlock_Fast(l) _MQ_Mutex_Unlock_Fast(l, __FILE__, __LINE__, __FUNCTION__)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_Mutex_Unlock_Fast(MQ_RWLock_t *l, const char *file, const int line, const char *func);

#define MQ_R_Lock(l,wait_nsecs) _MQ_R_Lock(l, wait_nsecs,__FILE__,__LINE__,__FUNCTION__)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_R_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func);

#define MQ_W_Lock(l,wait_nsecs) _MQ_W_Lock(l, wait_nsecs,__FILE__,__LINE__,__FUNCTION__)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_W_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func);

#define MQ_W2R_Lock(l) _MQ_W2R_Lock(l,__FILE__,__LINE__,__FUNCTION__)
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_W2R_Lock(MQ_RWLock_t *l, const char *file, const int line, const char *func);

#define MQ_R2W_Lock(l,wait_nsecs) _MQ_R2W_Lock(l, wait_nsecs,__FILE__,__LINE__,__FUNCTION__)
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_R2W_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs, const char *file, const int line, const char *func);

#define MQ_W_Unlock(l) _MQ_W_Unlock(l,__FILE__,__LINE__,__FUNCTION__)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_W_Unlock(MQ_RWLock_t *l, const char *file, const int line, const char *func);

#define MQ_R_Unlock(l) _MQ_R_Unlock(l,__FILE__,__LINE__,__FUNCTION__)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_R_Unlock(MQ_RWLock_t *l, const char *file, const int line, const char *func);

#define MQ_Unlock(l) _MQ_Unlock(l,__FILE__,__LINE__,__FUNCTION__)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_Unlock(MQ_RWLock_t *l, const char *file, const int line, const char *func);

#define MQ_Cond_Signal(cond,maxtowake,arg,flags) _MQ_Cond_Signal(cond, maxtowake, arg, flags, __FILE__,__LINE__,__FUNCTION__)

#define MQ_Cond_Wait(cond, maxwait, arg, flags) _MQ_Cond_Wait(cond, maxwait, arg, flags, __FILE__, __LINE__,__FUNCTION__)

#else

#define MQ_RWLock_Init(l,flags) _MQ_RWLock_Init(l,flags,1)
#define MQ_RWLock_Init_PreZeroed(l,flags) _MQ_RWLock_Init(l,flags,0)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_RWLock_Init(MQ_RWLock_t *l, uint64_t flags, int zero_p);

#define MQ_Mutex_Lock(l,wait_nsecs) _MQ_Mutex_Lock(l, wait_nsecs)
__wur _MQ_RWLOCK_MAYBE_INLINE int64_t _MQ_Mutex_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs);

#define MQ_Mutex_Unlock(l) _MQ_Mutex_Unlock(l)
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_Mutex_Unlock(MQ_RWLock_t *l);

#define MQ_Mutex_Lock_Fast(l,wait_nsecs) _MQ_Mutex_Lock_Fast(l, wait_nsecs)
__wur _MQ_RWLOCK_MAYBE_INLINE int64_t _MQ_Mutex_Lock_Fast(MQ_RWLock_t *l, MQ_Time_t wait_nsecs);

#define MQ_Mutex_Unlock_Fast(l) _MQ_Mutex_Unlock_Fast(l)
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_Mutex_Unlock_Fast(MQ_RWLock_t *l);

#define MQ_R_Lock(l,wait_nsecs) _MQ_R_Lock(l, wait_nsecs)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_R_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs);

#define MQ_W_Lock(l,wait_nsecs) _MQ_W_Lock(l, wait_nsecs)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_W_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs);

#define MQ_W2R_Lock(l) _MQ_W2R_Lock(l)
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_W2R_Lock(MQ_RWLock_t *l);

#define MQ_R2W_Lock(l,wait_nsecs) _MQ_R2W_Lock(l, wait_nsecs)
__wur _MQ_RWLOCK_MAYBE_INLINE int _MQ_R2W_Lock(MQ_RWLock_t *l, MQ_Time_t wait_nsecs);

#define MQ_W_Unlock(l) _MQ_W_Unlock(l)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_W_Unlock(MQ_RWLock_t *l);

#define MQ_R_Unlock(l) _MQ_R_Unlock(l)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_R_Unlock(MQ_RWLock_t *l);

#define MQ_Unlock(l) _MQ_Unlock(l)
_MQ_RWLOCK_MAYBE_INLINE int _MQ_Unlock(MQ_RWLock_t *l);

#define MQ_Cond_Signal(cond,maxtowake,arg,flags) _MQ_Cond_Signal(cond, maxtowake, arg, flags, 0, 0, 0)

#define MQ_Cond_Wait(cond, maxwait, arg, flags) _MQ_Cond_Wait(cond, maxwait, arg, flags, 0, 0, 0)
#endif

/* cond must be locked, R or W or mutex, on entry to _MQ_Cond_Wait(),
 * and will be locked with the same mode on return or cancellation
 * (unless flags & MQ_COND_RETURN_UNLOCKED)
 */
#define MQ_COND_RETURN_UNLOCKED (1UL<<0)

extern __wur int _MQ_Cond_Wait(MQ_RWLock_t *cond, MQ_Time_t maxwait, void **arg, uint64_t flags, const char *file, const int line, const char *func);
extern int _MQ_Cond_Signal(MQ_RWLock_t *l, int maxtowake, void *arg, uint64_t flags, const char *file, const int line, const char *func);

#define MQ_RWLOCK_R_COUNT(l) (((l)->RW.Count&MQ_RWLOCK_COUNT_READ) ? ((l)->RW.Count & (MQ_RWLOCK_COUNT_MINFLAG-1)) : 0)
#define MQ_RWLOCK_W_COUNT(l) (((l)->RW.Count&MQ_RWLOCK_COUNT_READ) ? 0 : ((l)->RW.Count & (MQ_RWLOCK_COUNT_MINFLAG-1)))
#define MQ_RWLOCK_R_LOCKED_P(l) ((l)->RW.Count & MQ_RWLOCK_COUNT_READ)
#define MQ_RWLOCK_W_LOCKED_P(l) (((! (l)->RW.Count) & MQ_RWLOCK_COUNT_READ) && ((l)->RW.Count & (MQ_RWLOCK_COUNT_MINFLAG-1)))
#define MQ_RWLOCK_R_NOTLOCKED_P(l) (! ((l)->RW.Count & MQ_RWLOCK_COUNT_READ))
#define MQ_RWLOCK_W_NOTLOCKED_P(l) ((! ((l)->RW.Count & (MQ_RWLOCK_COUNT_MINFLAG-1))) || (((l)->RW.Count) & MQ_RWLOCK_COUNT_READ))
#define MQ_RWLOCK_RW_LOCKED_P(l) ((l)->RW.Count & (MQ_RWLOCK_COUNT_MINFLAG-1))
#define MQ_RWLOCK_RW_RAWCOUNT(l) ((l)->RW.Count) /* used as an lvalue */
#define MQ_RWLOCK_RW_COUNT(l) ((l)->RW.Count & (MQ_RWLOCK_COUNT_MINFLAG-1))
#define MQ_RWLOCK_RW_RAWTOCOUNT(x) ((x) & (MQ_RWLOCK_COUNT_MINFLAG-1))

#ifndef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT

#define MQ_RWLOCK_TRACK_DUMMY (0UL<<62) /* not a real lock behind this; only a uint64_t (for flags) is behind the pointer */
#define MQ_RWLOCK_TRACK_MUTEX (1UL<<62)
#define MQ_RWLOCK_TRACK_WRITE (2UL<<62)
#define MQ_RWLOCK_TRACK_READ (3UL<<62)
#define MQ_RWLOCK_TRACK_TYPEMASK (3UL<<62)
#define MQ_RWLOCK_TRACK_RTM (1UL<<61) /* this is a virtual lock */
#define MQ_RWLOCK_TRACK_ACCESSLOCK (1UL<<60) /* used in .BlockedOnLock to mitigate deallocation races in MQ_SchedState_UpdateInheritances() */
#define MQ_RWLOCK_TRACK_READOPTIMAL_UNCOUNTED (1UL<<59) /* for MQ_RWLOCK_READOPTIMAL, set this initially, speculatively */
#define MQ_RWLOCK_TRACK_DEFERRED_COUNTED (1UL<<58) /* resolve accounting race between read and write locker on _READOPTIMAL locks; only settable by mutex owner */
#define MQ_RWLOCK_TRACK_IDMASK ((1UL<<58)-1UL)
#define MQ_RWLOCK_TRACK_ID(x) ((x).ID_with_status&MQ_RWLOCK_TRACK_IDMASK)
#define MQ_RWLOCK_TRACK_POINTER(x) ((MQ_RWLock_t *)((x).ID_with_status&MQ_RWLOCK_TRACK_IDMASK))
#define MQ_RWLOCK_TRACK_RTM_P(x) ((x).ID_with_status & MQ_RWLOCK_TRACK_RTM)
#define MQ_RWLOCK_TRACK_TYPE(x) ((x).ID_with_status & MQ_RWLOCK_TRACK_TYPEMASK)

struct HeldLock {
  uint64_t ID_with_status;
};
#define MQ_RWLOCK_HELDLOCK_INITIALIZER { .ID_with_status = 0 }

struct __MQ_RWLock_ThreadState {
  volatile struct HeldLock BlockedOnLock; /* reflects only RW blockage, for computing PI */
  volatile struct HeldLock HeldLocks[MQ_RWLOCK_MAX_TRACKED];
  volatile int HeldLocks_Index;
  MQ_RWLock_t *InheritedSchedState_From;
};

extern __thread_scoped struct __MQ_RWLock_ThreadState __MQ_RWLock_ThreadState;

extern int _MQ_SchedState_Inherit(MQ_RWLock_t *lock);
extern int _MQ_SchedState_Inherit_ByThr(struct __MQ_ThreadState *thr, MQ_RWLock_t *lock);
extern int _MQ_SchedState_Inherit_ByTID(pid_t tid, MQ_RWLock_t *lock);
extern int _MQ_SchedState_Disinherit(MQ_RWLock_t *lock);
extern int _MQ_SchedState_Disinherit_ByThr(struct __MQ_ThreadState *thr, MQ_RWLock_t *l);
extern int _MQ_SchedState_UpdateLockForBlockedThread(struct __MQ_ThreadState *thr);
extern int _MQ_SchedState_UpdateLockForUnblockedThread(struct __MQ_ThreadState *thr, MQ_RWLock_t *l);
extern int MQ_SchedState_AdoptPeak(void);
extern int MQ_SchedState_ReleasePeak(void);

#endif

#ifdef MQ_RWLOCK_NOLOCKTRACKING_SUPPORT
#define _MQ_RWLock_LockHeld_p(l,locktype) 0
#else
#define _MQ_RWLock_LockHeld_p(l,locktype) ((((l)->Flags & MQ_RWLOCK_NOLOCKTRACKING) || (MQ_TSA == MQ_TSA_INITIALIZER)) ? 0 : ({int lock_index; for (lock_index=__MQ_RWLock_ThreadState.HeldLocks_Index-1; lock_index >= 0; --lock_index) { if ((__MQ_RWLock_ThreadState.HeldLocks[lock_index].ID_with_status & (MQ_RWLOCK_TRACK_TYPEMASK|MQ_RWLOCK_TRACK_IDMASK)) == ((uint64_t)(l)|(locktype))) break; } (lock_index<0) ? 0 : 1;}))
#endif

#if defined(MQ_RWLOCK_INTELRTM_SUPPORT) && defined(__RTM__)
#define MQ_RWLock_HaveMutex(l) (((l)->MutEx.Owner == MQ_TSA_AUTOINIT) || _MQ_RWLock_LockHeld_p(l,MQ_RWLOCK_TRACK_MUTEX))
#else
#define MQ_RWLock_HaveMutex(l) ((l)->MutEx.Owner == MQ_TSA_AUTOINIT)
#endif

extern int _MQ_X_AddLock(MQ_RWLock_t *l, uint64_t locktype);
extern int _MQ_X_DropLock(MQ_RWLock_t *l, uint64_t locktype);
extern void _MQ_RWLock_ThreadCleanup(void);

/* lock handoffs are hopeless RTM-wise because of the intrinsic context switch */
#if defined(MQ_RWLOCK_INTELRTM_SUPPORT) && defined(__RTM__)
#define _MQ_RWLock_AbortRTM4Handoff() _xabort(2)
#else
#define _MQ_RWLock_AbortRTM4Handoff() ((void)0)
#endif

#define _MQ_Mutex_AddLock(l) _MQ_X_AddLock(l,MQ_RWLOCK_TRACK_MUTEX)
#define _MQ_Mutex_DropLock(l) _MQ_X_DropLock(l,MQ_RWLOCK_TRACK_MUTEX)

#define MQ_Mutex_BequeathLock(l) (_MQ_RWLock_AbortRTM4Handoff(),(MQ_SyncInt_ExchangeIfEq((l)->MutEx.Owner, MQ_TSA, MQ_TSA_BEQUEATHED_VALUE) ? _MQ_X_DropLock(l,MQ_RWLOCK_TRACK_MUTEX) : ((! MQ_TSA_ValidP((l)->MutEx.Owner)) ? MQ_seterrpoint_or_abort(EINVAL,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE) : MQ_seterrpoint_or_abort(EPERM,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE))))

#define MQ_Mutex_InheritLock(l) (_MQ_RWLock_AbortRTM4Handoff(),MQ_SyncInt_ExchangeIfEq((l)->MutEx.Owner, MQ_TSA_BEQUEATHED_VALUE, MQ_TSA_AUTOINIT) ? _MQ_X_AddLock(l,MQ_RWLOCK_TRACK_MUTEX) : MQ_seterrpoint_or_abort(EPERM,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE))

#define MQ_Mutex_SeizeLock(l) ({_MQ_RWLock_AbortRTM4Handoff(); __typeof__((l)->MutEx.Owner) MutEx_Locker_now = MQ_SyncInt_Get((l)->MutEx.Owner); (! MQ_TSA_ValidP(MutEx_Locker_now)) ? MQ_seterrpoint_or_abort(EINVAL,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE) : (MQ_SyncInt_ExchangeIfEq((l)->RW.Owner, W_Locker_now, MQ_TSA_AUTOINIT) ? _MQ_X_AddLock(l,MQ_RWLOCK_TRACK_MUTEX) : MQ_seterrpoint(EAGAIN));})

#define MQ_R_HaveLock_p(l) _MQ_RWLock_LockHeld_p(l,MQ_RWLOCK_TRACK_READ)

#if defined(MQ_RWLOCK_INTELRTM_SUPPORT) && defined(__RTM__)
#define MQ_W_HaveLock_p(l) (((l)->RW.Owner == MQ_TSA_AUTOINIT) || _MQ_RWLock_LockHeld_p(l,MQ_RWLOCK_TRACK_WRITE))
#else
#define MQ_W_HaveLock_p(l) ((l)->RW.Owner == MQ_TSA_AUTOINIT)
#endif

#define MQ_RWLock_IsActive(l) (! ((l)->Flags & MQ_RWLOCK_INACTIVE))
#define MQ_RWLock_SetInactive(l) ({int64_t mutexret = MQ_Mutex_Lock(l,MQ_NOWAITING); (mutexret>=0) ? (MQ_SyncInt_ExchangeIfEq(MQ_RWLOCK_RW_RAWCOUNT(l),0UL,MQ_RWLOCK_COUNT_CONTENDED) ? (MQ_SyncInt_SetFlags((l)->Flags,MQ_RWLOCK_INACTIVE), 0) : (MQ_Mutex_Unlock(l), MQ_seterrpoint(EBUSY))) : mutexret; })
#define MQ_RWLock_SetActive(l) (((l)->MutEx.Owner != MQ_TSA_AUTOINIT) ? MQ_seterrpoint_or_abort(EPERM,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE) : (! ((l)->Flags & MQ_RWLOCK_INACTIVE)) ? MQ_seterrpoint_or_abort(EINVAL,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE) : (MQ_SyncInt_ClearFlags((l)->Flags,MQ_RWLOCK_INACTIVE), MQ_Mutex_Unlock(l)))

#define MQ_RWLock_Cur_Waiter_Count_Unlocked(l) (((l)->MutEx.Count ? (int)(l)->MutEx.Count - 1 : 0) + (l)->Wait_List_Length)
__wur int MQ_RWLock_Cur_Waiter_Count(MQ_RWLock_t *l, MQ_Wait_Type which);

#define _MQ_W_AddLock(l) _MQ_X_AddLock(l,MQ_RWLOCK_TRACK_WRITE)
#define _MQ_W_DropLock(l) _MQ_X_DropLock(l,MQ_RWLOCK_TRACK_WRITE)
#define MQ_W_BequeathLock(l) (_MQ_RWLock_AbortRTM4Handoff(),(MQ_SyncInt_ExchangeIfEq((l)->RW.Owner, MQ_TSA, MQ_TSA_BEQUEATHED_VALUE) ? _MQ_X_DropLock(l,MQ_RWLOCK_TRACK_WRITE) : ((! MQ_TSA_ValidP((l)->RW.Owner)) ? MQ_seterrpoint_or_abort(EINVAL,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE) : MQ_seterrpoint_or_abort(EPERM,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE))))
#define MQ_W_InheritLock(l) (MQ_SyncInt_ExchangeIfEq((l)->RW.Owner, MQ_TSA_BEQUEATHED_VALUE, MQ_TSA_AUTOINIT) ? _MQ_X_AddLock(l,MQ_RWLOCK_TRACK_WRITE) : MQ_seterrpoint_or_abort(EPERM,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE))

#define MQ_W_SeizeLock(l) ({__typeof__((l)->RW.Owner) W_Locker_now = MQ_SyncInt_Get((l)->RW.Owner); (! MQ_TSA_ValidP(W_Locker_now)) ? MQ_seterrpoint_or_abort(EINVAL,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE) : (MQ_SyncInt_ExchangeIfEq((l)->RW.Owner, W_Locker_now, MQ_TSA_AUTOINIT) ? _MQ_X_AddLock(l,MQ_RWLOCK_TRACK_WRITE) : MQ_seterrpoint(EAGAIN));})

#define _MQ_R_AddLock(l) _MQ_X_AddLock(l,MQ_RWLOCK_TRACK_READ)
#define _MQ_R_DropLock(l) _MQ_X_DropLock(l,MQ_RWLOCK_TRACK_READ)
#define MQ_R_BequeathLock(l) (MQ_RWLOCK_R_NOTLOCKED_P(l) ? MQ_seterrpoint_or_abort(EINVAL,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE) : ({int mq_r_bl_ret = _MQ_R_DropLock(l); (mq_r_bl_ret<0) ? mq_r_bl_ret : (MQ_SyncInt_Fence_Release(),mq_r_bl_ret);}))
#define MQ_R_InheritLock(l) (MQ_SyncInt_Fence_Acquire(),MQ_RWLOCK_R_NOTLOCKED_P(l) ? MQ_seterrpoint_or_abort(EINVAL,(l)->Flags & MQ_RWLOCK_ABORTONMISUSE) : _MQ_R_AddLock(l))

#endif
