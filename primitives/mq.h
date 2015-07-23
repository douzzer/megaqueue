/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq.h  daniel.pouzzner@megaqueue.com
 * 
 * master include file for commonly needed MQ headers
 */

#ifndef MQ_H
#define MQ_H

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "mq_cext.h"
#include "mq_types.h"
#include "mq_time.h"
/* #include "mq_object.h" */
#include "mq_error.h"
#include "mq_procutil.h"
#include "mq_threadutil.h"
#include "mq_rwlock.h"

#endif
