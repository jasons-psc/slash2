/* $Id$ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pfl.h"
#include "psc_util/alloc.h"
#include "psc_util/thread.h"
#include "psc_util/ctlsvr.h"

#include "sliod.h"
#include "slconfig.h"
#include "rpc.h"
#include "pathnames.h"

extern void *nal_thread(void *);

const char *progname;

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s [-f cfgfile] [-S socket]\n", progname);
	exit(1);
}

void *
sliolndthr_start(void *arg)
{
	struct psc_thread *thr;

	thr = arg;
	return (nal_thread(thr->pscthr_private));
}

void
spawn_lnet_thr(pthread_t *t, void *(*startf)(void *), void *arg)
{
	extern int tcpnal_instances;
	struct psc_thread *pt;

	if (startf != nal_thread)
		psc_fatalx("unexpected LND start routine");

	pt = PSCALLOC(sizeof(*pt));
	pscthr_init(pt, SLIOTHRT_LND, sliolndthr_start, arg, "sliolndthr%d",
	    tcpnal_instances - 1);
	*t = pt->pscthr_pthread;
}

int
main(int argc, char *argv[])
{
	struct slio_ctlthr *thr;
	const char *cfn, *sfn;
	int c;

	progname = argv[0];
	cfn = _PATH_SLASHCONF;
	sfn = _PATH_SLIOCTLSOCK;
	while ((c = getopt(argc, argv, "f:S:")) != -1)
		switch (c) {
		case 'f':
			cfn = optarg;
			break;
		case 'S':
			sfn = optarg;
			break;
		default:
			usage();
		}

	if (getenv("LNET_NETWORKS") == NULL)
		psc_fatalx("please export LNET_NETWORKS");
	if (getenv("TCPLND_SERVER") == NULL)
		psc_fatalx("please export TCPLND_SERVER");

	pfl_init();
	thr = PSCALLOC(sizeof(*thr));
	pscthr_init(&pscControlThread, SLIOTHRT_CTL, NULL, thr,
	    "slioctlthr");

	lnet_thrspawnf = spawn_lnet_thr;

	slashGetConfig(cfn);
	libsl_init(PSC_SERVER);
	rpc_initsvc();
	sliotimerthr_spawn();
	slioctlthr_main(sfn);
	exit(0);
}
