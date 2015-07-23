# MegaQueue
lightweight high performance streaming data kernel

The only currently complete component of MegaQueue is its high
performance sharable lock facility, available in the primitives/
subdirectory.  The primitives library is released under an MIT
license.

"* LWP locks and signals with parsimonious userspace-only logic in
 * uncontended scenarios, reliable order-of-arrival queueing of
 * waiters (optionally, priority-ordered), mutex-weight W and R locks,
 * error checking on all locks, priority inheritance on RW locks,
 * integrated usage statistics, optimized contended scenario handling
 * with parsimonious syscalling, and integrated cond signalling with
 * reliable argument passing and reliable control of number of woken
 * threads"

Currently, build is based on a hand-built Makefile, and requires an
AMD64 CPU, Linux, Gnu make, and gcc 4.8+.  There are provisions and
plans for portability to other 64 bit CPUs and to *BSD.  In general,
this software should be considered experimental/alpha, though
substantial testing has already been performed.

To build and test:

  make check

To benchmark, including comparison with the system (pthread) lock
routines:

  make benchmark

API documentation: coming soon
