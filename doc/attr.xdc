<?xml version="1.0" ?>
<!-- $Id$ -->

<xdc xmlns:oof="http://www.psc.edu/~yanovich/xsl/oof-1.0">
	<title>Attribute (metadata) handling in the MDS</title>

	<oof:header size="1">Overview</oof:header>
	<oof:p>Currently SLASH2 has two ways of updating attributes:</oof:p>
	<list>
		<oof:list-item>the client can issue a setattr RPC to mds.</oof:list-item>
		<oof:list-item>the i/o server can issue a crc-update RPC,
			piggybacking size and mtime</oof:list-item>
	</list>
	<oof:p>
		To avoid these two RPCs cross path with each other, we have a
		utimegen mechanism so that an attribute update is only allowed by
		the crc-update path if it has the right generation number
		(utimegen).
		This generation number is stored in the ZFS layer (zp_s2utimgen) and
		is bumped each time we update mtime.
		It only applies to mtime.
	</oof:p>
	<oof:p>
		Using generation number means that the crc-update path has to
		compete to get the right to update the attributes.
		Instead, we should really update attributes based on when the
		corresponding operation (chmod, write, etc) happened on the client.
		Because utimegen only applies to mtime, crc-update path always
		increases file size, which could be a problem as well.
		Imagine that a client writes some data to increase the file size and
		then decides to truncate the file size.
		The final file size could be incorrect.
	</oof:p>
	<oof:p>
		Another issue with the current mechanism is that the most recent
		attributes of a file can be at either mds or ios.
		When the fchm (file cache entry) of a client needs to be
		re-established, where does it get attributes from?
		The most recent attributes might still be en route to mds from IOS.
	</oof:p>
	<oof:p>
		We could make the current scheme work by adding tricks here and
		there.
		But to make things robust, we need a simpler way.
		We will let client be the only one that can update the attributes.
		It should work regardless of network delays and service outage.
	</oof:p>

Solution 1
----------

So the idea is that we will treat attribute we do with data.

We need a lease on the attributes just as we have leases on individual bmaps.

So we will make some changes to existing RPCs, and perhaps add some new ones.

	* SRMT_LOOKUP: implicitly grant a shared or exclusive lease on the attributes
		       depending on whether if there is another client working
		       on the same file.

	* SRMT_SETATTR: We should also send this RPC if we find dirty attributes when
			we close a file.

	* SRMT_RELASEBMAP: can piggyback dirty attributes.  No, this RPC goes to the
			   IOS, not mds.

	* SRMT_BMAPDIO: We may be able to extend its meaning to revoke the right of
			a client to cache attributes exclusively.

We might need a daemon to flush dirty attributes just like we flush bmaps.  Perhaps
we can use the same flush thread that does the latter.

The bottom line is that we need a lease to protect attributes. They are cacheable if
the lease is exclusive.

We maybe able to combine bmap and attribute leases in a RPC:

	* SRMT_RELEASE_LEASE: use type to distinguish whether it is a lease for
			      attributes or bmap.
	* SRMT_REVOKE_LEASE

Solution 2
----------

Instead of going for perfect coherency, we still uses the NFS way of caching attributes
for a short period of time.  The would save a lot of code complexity and should work well
in practice.  In addition, perfect coherency is costly to achieve in a wide area network.

Potential Issues:

	* Right now, there is only sliod updating the file size.  In this new scheme, two
	  clients can do so.

	* The peformance of close() could take a hit, because we want to make sure that
	  dirty attributes are flushed to mds.

Extra Notes:

	* If the mds knows that more than one client is writing the same file, it should
	  disallow any size decreases.

	* If the mds knows that more than one client is writing the same file, it should
	  advise clients to reduce attributes caching time.

We will go for the second solution.

12/19/2011
----------

We cache all the dirty attribute updates at the client side, including new uid/gid.  The MDS
just accepts these changes without checking for credentials.

12/21/2011
----------

1. We still allow nblks to be directly reported from IOS to MDS. This is a bit odd because
   size and disk usage won't be in sync for a while.  But at least this attribute won't be
   updated by two guys at the same time.

2. Use setattr to update dirty attributes seem to have problem with ENOTSUP.  The partial
   truncate code is disabled for the moment.  Need to figure out why.

01/12/2011
----------

At change 18602, the following tests have been done and largely passed:

(1) rsync -vrlthHD /local/linux-3.0/ lemon://zzh-slash2/zhihui

Repeat the same command should produce no new work.

(2) s2build inside slash2

Work like a charm.

(3) kernel build, except fail on mmap like the following:

  IHEX    firmware/3com/typhoon.bin
  IHEX2FW firmware/emi26/loader.fw
mmap: No such device
make[1]: *** [firmware/emi26/loader.fw] Error 1
make[1]: *** Waiting for unfinished jobs....
make: *** [modules] Error 2

(4) posix test, except the following failures (some are known, some are caused by partial truncate)

Test Summary Report
-------------------
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/chmod/05.t   (Wstat: 0 Tests: 14 Failed: 1)
  Failed test:  8
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/chown/05.t   (Wstat: 0 Tests: 15 Failed: 2)
  Failed tests:  8, 10
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/link/06.t    (Wstat: 0 Tests: 18 Failed: 4)
  Failed tests:  11, 14, 17-18
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/mkdir/06.t   (Wstat: 0 Tests: 12 Failed: 1)
  Failed test:  7
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/open/05.t    (Wstat: 0 Tests: 12 Failed: 1)
  Failed test:  7
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/open/06.t    (Wstat: 0 Tests: 72 Failed: 1)
  Failed test:  69
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/rmdir/07.t   (Wstat: 0 Tests: 10 Failed: 2)
  Failed tests:  6, 8
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/symlink/03.t (Wstat: 0 Tests: 14 Failed: 2)
  Failed tests:  1-2
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/truncate/00.t (Wstat: 0 Tests: 21 Failed: 10)
  Failed tests:  3-6, 8-11, 14-15
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/truncate/02.t (Wstat: 0 Tests: 5 Failed: 2)
  Failed tests:  2-3
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/truncate/03.t (Wstat: 0 Tests: 12 Failed: 1)
  Failed test:  6
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/truncate/05.t (Wstat: 0 Tests: 15 Failed: 6)
  Failed tests:  5-6, 8, 10-12
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/truncate/12.t (Wstat: 0 Tests: 3 Failed: 1)
  Failed test:  2
/zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/tests/unlink/05.t  (Wstat: 0 Tests: 10 Failed: 2)
  Failed tests:  6, 8
Files=184, Tests=1957, 1124 wallclock secs ( 0.87 usr  0.34 sys + 477.52 cusr  8.13 csys = 486.86 CPU)
Result: FAIL
[root@lemon dodo]# prove -r /zzh-slash2/zhihui/projects/distrib/pjd-fstest-20080816/

01/14/2011
----------

There is a bug in the current code, which fails to propagate nblocks used by a file to its peer MDS. This
needs to be fixed by tweaking the following code in mds_distill_handler(), perhaps add a new op type:

 494         /*
 495          * Fabricate a setattr update entry to change the size.
 496          */
 497         if (type == MDS_LOG_BMAP_CRC) {
 498                 update_entry.op = NS_OP_SETSIZE;
 499                 update_entry.mask = mdsio_slflags_2_setattrmask(
 500                     PSCFS_SETATTRF_DATASIZE);
 501                 update_entry.size = sjbc->sjbc_fsize;
 502                 update_entry.target_fid = sjbc->sjbc_fid;
 503                 goto write_update;
 504         }

The problem is that do we want to propagate nblocks on each IOS to our peer, or a sum of them, or even
all crcs to our peer?

By the way, the above code was introduced by  http://frodo/svnweb/main/revision/?rev=15343.