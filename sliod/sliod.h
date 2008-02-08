/* $Id$ */

#ifndef _SLIOD_H_
#define _SLIOD_H_

#define SLIOTHRT_CTL	0	/* control */
#define SLIOTHRT_LND	1	/* lustre networking helper */
#define SLIOTHRT_RPC	2	/* RPC comm */

struct slio_ctlthr {
	int sc_st_nclients;
	int sc_st_nsent;
	int sc_st_nrecv;
};

#define slioctlthr(thr) ((struct slio_ctlthr *)(thr)->pscthr_private)

void slio_init(void);

extern struct psc_thread slioControlThread;

#endif /* _SLIOD_H_ */
