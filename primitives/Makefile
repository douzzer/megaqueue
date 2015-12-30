#MegaQueue
#
#Makefile  2015jul16  daniel.pouzzner@megaqueue.com

SHELL=/bin/bash
#CC=gcc-4.8
CC=gcc
CXX=g++
#LD=gcc-4.8
LD=gcc
LDCXX=g++
COPTIONS=-std=gnu11
CXXOPTIONS=-std=gnu++11
OPT=-O9
DBG=-g -Wall -Wextra -Wformat=2 -Winit-self -Wmissing-include-dirs -Wsuggest-attribute=pure -Wsuggest-attribute=const -Wsuggest-attribute=noreturn -Wunknown-pragmas -Wtraditional-conversion -Wshadow -Wpointer-arith -Wbad-function-cast -Wcast-align -Wwrite-strings -Wconversion -Wjump-misses-init -Wlogical-op -Wstrict-prototypes -Wold-style-definition -Wmissing-declarations -Wmissing-format-attribute -Wpointer-arith -Woverlength-strings -Wredundant-decls -Winline -Winvalid-pch
DBGCXX=-g -Wall -Wextra -Wformat=2 -Winit-self -Wmissing-include-dirs -Wsuggest-attribute=pure -Wsuggest-attribute=const -Wsuggest-attribute=noreturn -Wunknown-pragmas -Wshadow -Wpointer-arith -Wcast-align -Wwrite-strings -Wconversion -Wlogical-op -Wmissing-declarations -Wmissing-format-attribute -Wpointer-arith -Woverlength-strings -Wredundant-decls -Winline -Winvalid-pch

#-DMQ_DEBUG -DMQ_RWLOCK_PRIORITYORDER_SUPPORT 
DEFINES=-DMQ_RWLOCK_ADAPTIVE_SUPPORT -DMQ_RWLOCK_INHERITPRIORITY_SUPPORT -DMQ_RWLOCK_NOCOSTLYERRORCHECKING_SUPPORT -DMQ_RWLOCK_ABORTWHENCORRUPTED_SUPPORT -DMQ_RWLOCK_ABORTONMISUSE_SUPPORT -DBENCHOPTIONS
INCLUDES=
CFLAGS=$(COPTIONS) $(DEFINES) $(INCLUDES) $(OPT) $(DBG)
CXXFLAGS=$(CXXOPTIONS) $(DEFINES) $(INCLUDES) $(OPT) $(DBGCXX)

MQLIB_SRCS=mq_time.c mq_error.c mq_procutil.c mq_threadutil.c mq_rwlock_internal.c mq_rwlock_global.c
MQLIB_OBJS=mq_time.o mq_error.o mq_procutil.o mq_threadutil.o mq_rwlock_internal.o mq_rwlock_global.o
MQLIB_TARGET=libmq.a

LIBS=$(MQLIB_TARGET) -lrt -pthread
LDFLAGS=$(LIBS)

TEST_SRCS=tests/test_rwlock.c
TEST_OBJS=tests/test_rwlock.o tests/test_rwlock_cxx.o
TEST_PROGS=tests/test_rwlock tests/test_rwlock_cxx

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

tests/test_rwlock_cxx.o:
	$(CXX) -c $(CXXFLAGS) -o $@ tests/test_rwlock.c

tests/test_rwlock_cxx: tests/test_rwlock_cxx.o $(MQLIB_TARGET)
	$(LDCXX) -o $@ $< $(LDFLAGS)

.PHONY: clean
clean:
	rm -f $(OBJS) $(TEST_PROGS)

.PHONY: depend
depend: $(SRCS)

	$(CC) -MM $(CFLAGS) $(MQLIB_SRCS) > .depend
	$(CC) -MM $(CFLAGS) $(TEST_SRCS) | sed 's/^\([^:]*\.o: \)/tests\/\1/' >> .depend

-include .depend
