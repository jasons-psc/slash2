/* $Id$ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "psc_util/cdefs.h"
#include "psc_util/alloc.h"
#include "psc_util/thread.h"

#include "slash.h"

const char *progname;

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s\n", progname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	int c;

	progname = argv[0];
	while ((c = getopt(argc, argv, "")) != -1)
		switch (c) {
		default:
			usage();
		}
	exit(0);
}

void
spawn_lnet_thr(pthread_t *t, void *(*startf)(void *), void *arg)
{
	extern int tcpnal_instances;
	struct psc_thread *pt;

	pt = PSCALLOC(sizeof(*pt));
	pscthr_init(pt, SLTHRT_LND, startf, tcpnal_instances - 1);
	*t = pt->pscthr_pthread;
	pt->pscthr_private = arg;
}

#if 0
void
spawn_lnet_thr(pthread_t *t, void *(*startf)(void *), void *arg)
{
	int rc;

	rc = pthread_create(t, NULL, startf, arg);
	if (rc)
		psc_fatalx("pthread_create: %s", strerror(rc));
}
#endif
