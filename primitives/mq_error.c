/* MegaQueue
 *
 * Copyright 2015 Daniel Pouzzner
 *
 * mq_error.c  2015mar4  daniel.pouzzner@megaqueue.com
 * 
 * error handling and debugging aids
 */

__thread int MQ_errno = 0;
__thread const char *MQ_errno_file = 0;
__thread int MQ_errno_line = 0;
__thread const char *MQ_errno_function = 0;
