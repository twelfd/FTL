/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Thread routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"

// Logic of the locks:
// Any of the various threads (logparser, GC, client threads) is accessing FTL's data structure. Hence, they should
// never run at the same time since the data can change half-way through, leading to unspecified behavior.
// threadlock:  The threadlock ensures that only one thread can be active at any given time
pthread_mutex_t threadlock;

void enable_thread_lock(const char *message)
{
	int ret = pthread_mutex_lock(&threadlock);

	if(ret != 0)
		logg("Thread lock error: %i",ret);

	if(debugthreads)
		logg("Thread locked: %s", message);
}

void disable_thread_lock(const char *message)
{
	int ret = pthread_mutex_unlock(&threadlock);

	if(ret != 0)
		logg("Thread unlock error: %i",ret);

	if(debugthreads)
		logg("Thread unlocked: %s", message);
}

void init_thread_lock(void)
{
	if (pthread_mutex_init(&threadlock, NULL) != 0)
	{
		logg("FATAL: Thread mutex init failed\n");
		// Return failure
		exit(EXIT_FAILURE);
	}
}
