# MegaQueue
lightweight high performance streaming data kernel

Currently the high performance sharable lock facility is complete,
available in the primitives/ subdirectory.  The primitives library is
released under an MIT license.

Description of the lock facility, from the source (see
primitives/mq_rwlock.h for more details; see particularly the list of
feature flags):

"LWP locks and signals with parsimonious userspace-only logic in
uncontended scenarios, reliable order-of-arrival queueing of waiters
(optionally, priority-ordered), mutex-weight W and R locks, error
checking on all locks, priority inheritance on RW locks, integrated
usage statistics, optimized contended scenario handling with
parsimonious syscalling, and integrated cond signalling with reliable
argument passing and reliable control of number of woken threads"

Currently, build is based on a hand-built Makefile, and requires an
AMD64 CPU, Linux, Gnu make, and gcc 4.8+.  There are provisions and
plans for portability to other 64 bit CPUs and to *BSD.  In general,
this software should be considered experimental/alpha, though
substantial testing has already been performed.

To build and test
=================

	make check

To benchmark, including comparison with the system (pthread) lock
routines

	make benchmark

API documentation: coming soon

Benchmark results on a 2GHz 8 core Opteron workstation
======================================================

	(DEFINES=-DMQ_RWLOCK_PRIORITYORDER_SUPPORT \
	-DMQ_RWLOCK_INHERITPRIORITY_SUPPORT \
	-DMQ_RWLOCK_ADAPTIVE_SUPPORT \
	-DMQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT \
	-DMQ_RWLOCK_ABORTWHENCORRUPTED_SUPPORT \
	-DMQ_RWLOCK_ABORTONMISUSE_SUPPORT \
	-DBENCHOPTIONS)
	
	root@mega:/home/douzzer/megaqueue-src/git-repos/megaqueue Ubu# primitives/tests/test_rwlock 0
	compiled-in rwlock flags: 0x2409a
	warning, leaving local build flags (0x2409a) in place for benchmarks
	1.00413ns per round for minimal unlocked spin iteration
	369.512ns per round for sched_yield()
	28.098ns per round benchmarking overhead
	25.094ns per pthread_mutex_{lock,unlock}
	25.590ns per MQ_Mutex_{Lock,Unlock}
	18.566ns per MQ_Mutex_{Lock,Unlock}_Fast
	25.586ns per pthread_mutex_{lock,unlock}, second run
	41.987ns per pthread_mutex_{lock,unlock}, PTHREAD_PRIO_INHERIT
	107.689ns per pthread_mutex_{lock,unlock}, PTHREAD_PRIO_PROTECT
	30.610ns per pthread_mutex_{lock,unlock}, PTHREAD_MUTEX_ERRORCHECK
	28.095ns per MQ_Mutex_Lock, MQ_Unlock
	25.084ns per MQ_W_{Lock,Unlock}
	42.652ns per MQ_W_Lock, MQ_W2R_Lock, MQ_R_Unlock
	27.093ns per MQ_W_Lock, MQ_Unlock
	28.097ns per MQ_R_{Lock,Unlock}
	45.157ns per MQ_R_Lock, MQ_R2W_Lock, MQ_W_Unlock
	31.105ns per MQ_R_Lock, MQ_Unlock
	42.893ns per pthread_rwlock_{rdlock,unlock}
	43.154ns per pthread_rwlock_{wrlock,unlock}
