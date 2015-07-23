#MegaQueue
#
#Makefile  2015jul16  daniel.pouzzner@megaqueue.com

SHELL=/bin/bash
#CC=gcc-4.8
CC=gcc
#LD=gcc-4.8
LD=gcc
COPTIONS=-std=gnu99
OPT=-O9
DBG=-Wall -Wextra -Wformat=2 -Winit-self -Wmissing-include-dirs -Wsuggest-attribute=pure -Wsuggest-attribute=const -Wsuggest-attribute=noreturn -Wunknown-pragmas -Wtraditional-conversion -Wshadow -Wpointer-arith -Wbad-function-cast -Wcast-align -Wwrite-strings -Wconversion -Wjump-misses-init -Wlogical-op -Wstrict-prototypes -Wold-style-definition -Wmissing-declarations -Wmissing-format-attribute -Wpointer-arith -Woverlength-strings -Wredundant-decls -Winline -Winvalid-pch
DEFINES=-DMQ_RWLOCK_PRIORITYORDER_SUPPORT -DMQ_RWLOCK_INHERITPRIORITY_SUPPORT -DMQ_RWLOCK_ADAPTIVE_SUPPORT -DMQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT -DMQ_RWLOCK_ABORTWHENCORRUPTED_SUPPORT -DMQ_RWLOCK_ABORTONMISUSE_SUPPORT -DBENCHOPTIONS
INCLUDES=
CFLAGS=$(COPTIONS) $(DEFINES) $(INCLUDES) $(OPT) $(DBG)

MQLIB_SRCS=mq_time.c mq_error.c mq_procutil.c mq_threadutil.c mq_rwlock_internal.c
MQLIB_OBJS=mq_time.o mq_error.o mq_procutil.o mq_threadutil.o mq_rwlock_internal.o
MQLIB_TARGET=libmq.a

LIBS=$(MQLIB_TARGET) -lrt -pthread
LDFLAGS=$(LIBS)

TEST_SRCS=tests/test_rwlock.c
TEST_OBJS=tests/test_rwlock.o
TEST_PROGS=tests/test_rwlock

SRCS=$(MQLIB_SRCS) $(TEST_SRCS)
OBJS=$(MQLIB_OBJS) $(MQLIB_TARGET) $(TEST_OBJS)

.c.o:
	$(CC) -c $(CFLAGS) -o $@ $<

all: $(TEST_PROGS)

lib: $(MQLIB_TARGET)

tests: $(TEST_PROGS)

check: tests
	./tests/test_rwlock 10

benchmark: tests
	taskset 7f ./tests/test_rwlock 0

$(MQLIB_TARGET): $(MQLIB_OBJS)
	ar cr $(MQLIB_TARGET) $(MQLIB_OBJS)
	ranlib $(MQLIB_TARGET)

tests/%: tests/%.o $(MQLIB_TARGET)
	$(LD) -o $@ $< $(LDFLAGS)

.PHONY: clean
clean:
	rm -f $(OBJS) $(TEST_PROGS)

.PHONY: depend
depend: $(SRCS)

	$(CC) -MM $(CFLAGS) $(MQLIB_SRCS) > .depend
	$(CC) -MM $(CFLAGS) $(TEST_SRCS) | sed 's/^\([^:]*\.o: \)/tests\/\1/' >> .depend

-include .depend
