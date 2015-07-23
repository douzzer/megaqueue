/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_cext.h  2015mar23  daniel.pouzzner@megaqueue.com
 * 
 * macro layer atop gcc extensions
 */

#ifndef MQ_CEXT_H
#define MQ_CEXT_H

#pragma GCC diagnostic ignored "-Wvariadic-macros"

#define __unused __attribute__((unused))
#define __noreturn __attribute__((noreturn))
#define __deprecated __attribute__((deprecated))
#define __inline_all_calls __attribute__((flatten))
#define __printf_prototype(fmt_arg_index,first_vararg_index) __attribute__((format(printf,fmt_arg_index,first_vararg_index)))
#define __mallocish __attribute__((malloc))
#define __noreturn __attribute__((noreturn))
#define __unreachable __builtin_unreachable()

#define __fastpath(x) __builtin_expect((int64_t)(x),1L)
#define __slowpath(x) __builtin_expect((int64_t)(x),0L)

/* borrow D's name for __builtin_choose_expr, albeit with functional syntax */
#define __static_if(x,y,z) __builtin_choose_expr(x,y,z)

#define __thread_scoped __thread

#define __is_constant(x) __builtin_constant_p(x)

/* gcc 4.9 adds this:
#define auto __auto_type
*/

#define offsetof(type, member) __builtin_offsetof(type, member)

#define MQ_SyncInt_Get(i) __atomic_load_n(&(i),__ATOMIC_SEQ_CST)
#define MQ_SyncInt_Put(i,val) __atomic_store_n(&(i),val,__ATOMIC_SEQ_CST)
#define MQ_SyncInt_Increment(i,by) __atomic_add_fetch(&(i),by,__ATOMIC_SEQ_CST)
#define MQ_SyncInt_Increment_Acquire(i,by) __atomic_add_fetch(&(i),by,__ATOMIC_ACQUIRE)
#define MQ_SyncInt_PostIncrement(i,by) __atomic_fetch_add(&(i),by,__ATOMIC_SEQ_CST)
#define MQ_SyncInt_PostIncrement_Acquire(i,by) __atomic_fetch_add(&(i),by,__ATOMIC_ACQUIRE)
#define MQ_SyncInt_Decrement(i,by) __atomic_sub_fetch(&(i),by,__ATOMIC_SEQ_CST)
#define MQ_SyncInt_Decrement_Release(i,by) __atomic_sub_fetch(&(i),by,__ATOMIC_RELEASE)
#define MQ_SyncInt_PostDecrement(i,by) __atomic_fetch_sub(&(i),by,__ATOMIC_SEQ_CST)
#define MQ_SyncInt_PostDecrement_Release(i,by) __atomic_fetch_sub(&(i),by,__ATOMIC_RELEASE)

#define MQ_SyncInt_SetFlags(i,flags) __atomic_fetch_or(&(i),flags,__ATOMIC_SEQ_CST)
#define MQ_SyncInt_ClearFlags(i,flags) __atomic_fetch_and(&(i),~(flags),__ATOMIC_SEQ_CST)

#define MQ_SyncInt_TestAndSet(i) __atomic_test_and_set(&(i),__ATOMIC_SEQ_CST)
#define MQ_SyncInt_TestAndSet_Acquire(i) __atomic_test_and_set(&(i),__ATOMIC_ACQUIRE)
#define MQ_SyncInt_Clear(i) __atomic_clear(&(i),__ATOMIC_SEQ_CST)
#define MQ_SyncInt_Clear_Release(i) __atomic_clear(&(i),__ATOMIC_RELEASE)

#define MQ_SyncInt_Exchange(i,to) __atomic_exchange_n(&(i), to, __ATOMIC_SEQ_CST)
#define MQ_SyncInt_Exchange_Acquire(i,to) __atomic_exchange_n(&(i), to, __ATOMIC_ACQUIRE)
#define MQ_SyncInt_Exchange_Release(i,to) __atomic_exchange_n(&(i), to, __ATOMIC_SEQ_RELEASE)
#define MQ_SyncInt_ExchangeIfEq(i,from,to) __sync_bool_compare_and_swap(&(i), from, to) /* must use legacy gcc __sync because __atomics lacks this semantic */
#define MQ_SyncInt_ExchangeIfEq_ReturnOldVal(i,from,to) __sync_val_compare_and_swap(&(i), from, to) /* must use legacy gcc __sync because __atomics lacks this semantic */

/* note __atomic_compare_exchange_n() is consistently 2 cycles slower per call then __sync_bool_compare_and_swap.
 * inspecting gcc's x86-64 assembler output shows it is generating two instructions for the fail-store even though
 * it should be clear to the optimizer that it is discarded (passes out of scope immediately).
 * also of note, on x86-64 the __ATOMIC_ACQUIRE and __ATOMIC_RELEASE generate assembler identical to __ATOMIC_SEQ_CST,
 * laying to rest any hopes of a performance boost from loosening the memory model.
 * consequently these macros built around __atomic_compare_exchange_n() are not actually used at present.
*/

/* ignore -Wtraditional-conversion to work around spurious, otherwise inescapable warning on arg 4 of __atomic_compare_exchange_n ("weak_p") */
#define MQ_SyncInt_ExchangeIfEq_Acquire(i,from,to) ({_Pragma("GCC diagnostic push"); _Pragma("GCC diagnostic ignored \"-Wtraditional-conversion\""); __typeof__(from) dummyfrom = from; __atomic_compare_exchange_n(&(i), &dummyfrom, to, 0 /* weak_p */, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); _Pragma("GCC diagnostic pop");})
#define MQ_SyncInt_ExchangeIfEq_Release(i,from,to) ({_Pragma("GCC diagnostic push"); _Pragma("GCC diagnostic ignored \"-Wtraditional-conversion\""); __typeof__(from) dummyfrom = from; __atomic_compare_exchange_n(&(i), &dummyfrom, to, 0 /* weak_p */, __ATOMIC_RELEASE, __ATOMIC_RELAXED); _Pragma("GCC diagnostic pop");})
#define MQ_SyncInt_ExchangeIfEq_StoreOnFail(i,from,to) ({_Pragma("GCC diagnostic push"); _Pragma("GCC diagnostic ignored \"-Wtraditional-conversion\""); __atomic_compare_exchange_n(&(i), &(from), to, 0 /* weak */, __ATOMIC_SEQ_CST /* success_memmodel */, __ATOMIC_RELAXED /* failure_memmodel */); _Pragma("GCC diagnostic pop");})
#define MQ_SyncInt_ExchangeIfEq_StoreOnFail_Strict(i,from,to) ({_Pragma("GCC diagnostic push"); _Pragma("GCC diagnostic ignored \"-Wtraditional-conversion\""); __atomic_compare_exchange_n(&(i), (void **)&(from), to, 0 /* weak */, __ATOMIC_SEQ_CST /* success_memmodel */, __ATOMIC_SEQ_CST /* failure_memmodel */); _Pragma("GCC diagnostic pop");})

#define MQ_SyncInt_Fence_Consume() __atomic_thread_fence(__ATOMIC_CONSUME)
#define MQ_SyncInt_Fence_Acquire() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define MQ_SyncInt_Fence_Release() __atomic_thread_fence(__ATOMIC_RELEASE)
#define MQ_SyncInt_Fence_AcqRel() __atomic_thread_fence(__ATOMIC_ACQ_REL)
#define MQ_SyncInt_Fence_Full() __atomic_thread_fence(__ATOMIC_SEQ_CST)

// returns a uint of same size as the argument, which can be any int or float
#define MQ_ByteSwap(x) __builtin_choose_expr(sizeof(x)==2, __builtin_bswap16((uint16_t)(x)), __builtin_choose_expr(sizeof(x)==4, __builtin_bswap32((uint32_t)(x)), __builtin_bswap64((uint64_t)(x))))
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define MQ_NativeToBigEndian(x) MQ_ByteSwap(x)
#define MQ_BigToNativeEndian(x) MQ_ByteSwap(x)
#else
#define MQ_NativeToBigEndian(x) (x)
#define MQ_BigToNativeEndian(x) (x)
#endif

#define __popcount(x) __builtin_choose_expr(sizeof(x)==sizeof(unsigned long long int), __builtin_popcountll((unsigned long long int)(x)), __builtin_popcount(x))
#define __clz(x) __builtin_choose_expr(sizeof(x)==sizeof(unsigned long long int), __builtin_clzll(x), __builtin_clz(x))
#define __ctz(x) __builtin_choose_expr(sizeof(x)==sizeof(unsigned long long int), __builtin_ctzll(x), __builtin_ctz(x))

#define __prefetch_for_read(addr,locality...) __builtin_prefetch(addr, 0, ## locality)
#define __prefetch_for_write(addr,locality...) __builtin_prefetch(addr, 1, ## locality)

typedef __int128 int128_t;
typedef unsigned __int128 uint128_t;

/* these have to go somewhere */
#define max(x,y) ({__typeof__(x) _x = (x), _y = (y); (_y <= _x) ? _x : _y;})
#define min(x,y) ({__typeof__(x) _x = (x), _y = (y); (_y <= _x) ? _y : _x;})

#endif
