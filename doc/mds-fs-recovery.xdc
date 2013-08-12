6/23/2011 - pauln

Last week we recovered an MDS from a ZFS snapshot stream.  The result was 
quite encouraging because we were able to demonstrate that the SLASH2 mds
was able to be recovered from a backup.  Given that PSC seeks to use SLASH2 for
its achival system, I was very encouraged by this.  

As one can imagine the procedure did shed light on some shortcomings of the 
system.  For instance, SLASH2 relies on monotonic numbering for its FID and 
lease sequencing.  When recovering from a snapshot, it is assumed that the 
recovered FS will resume from an old state.  Therefore, it's possible and 
likely that clients and sliod's will contain state (caches, backing objects, 
etc.) which resulted from activity after the MDS snapshot was taken. Neededless
to say, the recovered MDS knows nothing about these items since they were 
not contained in the backup dump.

Besides restoring from backups, the SLASH2 MDS may encounter situations where
its journal has been corrupted or no longer exists.  On restart the MDS will 
complain about this condition because the resulting scenario is similar to
that described above.  Without the journal, the MDS may have lost committed 
transactions.  As a result it would be possible for the MDS to reallocate a
FID or have lost a bmap lease assignment.  Either of these cases can lead
to data corruption.  Losing some file objects in such situations may
be tolerable but corruption of data or metadata structures can and should be 
avoided.

(Other items lost will be crc and size attribute updates from SLIOD).  
       . We may want to keep some kind of recently used list of sliod
       backing objects which gets flushed when the MDS makes (or offloads)
       a snapshot somewhere..  This way the sliod's could re-report info
       that was lost due to a crash without having to do a full scan.
       
       Full scans / scrubs of the SLIOD backfs may be needed though.

The current FID design uses 10 bits for server ID while the remaining 54 bits 
are for enumerating files and directories.  Each MDS in the SLASH2 fs is 
assigned a unique site ID allowing the system to support 1024 metadata servers.

The FID is monotonic which allows for features such as asynchronous garbage 
collection.  Since backing objects
are created on demand and deleted asynchronously, the MDS must ensure that 
FIDs are not improperly reused.  When recovering from a corrupt ZFS pool or 
SLASH2 journal, the MDS has no (viable) way to recover the last allocated
FID.  It is possible that the MDS could query the SLIODs for this information
but that seems clumsy.  Plus, given that we're striving to support loosely 
coupled systems, we can't even guarantee that all SLIODs are running.  To 
make matters worse, a client could be holding onto an open file for some time
before issuing a write(), which means that SLIOD may not know about this file
object for some time. 

The proposal here is to change the partitioning scheme of the FIDs upper bits
such that each MDS is assigned a contiguous numerical range.  For the purposes
of this document each value in the range is referred to as a 'cycle'.   To 
illustrate - if the SLASH2 fs had 2 MDS's and used 32 high order bits, each 
MDS would have 2^32/(num mds's) cycles. Additionally, using 16 bits and 64 
MDS's we'd get a cycle count of 1024 per MDS (2^16/64 == 1024).
 
In the last case, each MDS would proceed to the next cycle on recovery or 
after allocating 2^(64-16) FIDs.  The caveat of this design is that a given 
MDS can't use more cycles than it's assigned without some legwork.

With cycling, we're trying to create a scenario where MDS recoveries due to 
restoration from backup or caused by corrupt or missing journals does not force
the system to undergo horrendous amounts of backtracking in order to guarantee
that progress may be made safely.  Should the system encounter this sort of 
error, or if it exhausts the cycle's range through normal FID allocation, 
the MDS bumps his cycle. Upon seeing the cycle change, clients and sliods are 
implicitly made aware that such an event has occurred.

Technical details:
	  . On connect (reconnect) or even in each RPC header, the MDS
	  communicates its current cycle to clients and sliods.
	  . The bmap lease sequence number (low waterwark) must be encoded
	  with the cycle so that timeouts are handled appropriately.
	  . Clients must be able to attribute a cycle range to a specific 
	  MDS.  This should be done via the configuration file.
	  	- NUM_MDS = XX 
		- CYCLE_BITS = XX  # num of high order bits used for cycles
		- Each MDS defined in the config is given an ID which 
		defines which cycle range it will use.
		

Another solution:

Each SLIOD maintains a file that remembers the highest FID it has seen.
The MDS, upon restart, should contact all sliods, and perhaps clients 
as well before it can proceed.

During the reconnection, information such as fid and leases can be 
exchanged.

07/08/2011  
----------

The above is a non-starter because we create backing files on demand.  

	<-- On the other hand, if an IOS hasn't seen a FID and the MDS
	    hasn't seen the FID either, we should be able to conclude
	    that FID is not needed anymore, right?

	<-- I am more concerned about the a stuck IOS will prevent a
	    crashed MDS from restarting.

The cycle number will increment when (1) lower space bits wrap around
and (2) missing journal.   In the latter case, the lower bits can be
reset to all zeros.

We also need a way to remove the backing files of those fids a MDS
has skipped.  But that can be done in the background.

07/25/2011
----------

Date: Fri, 22 Jul 2011 17:06:07 -0400
From: Paul Nowoczynski <pauln@psc.edu>
To: Zhihui Zhang <zhihui@psc.edu>
Cc: Jared Yanovich <yanovich@psc.edu>
Subject: Re: cycle id issue

Zhihui,
I see the issue - what if we increment +2 on recovery?
paul

On 07/22/2011 04:59 PM, Zhihui Zhang wrote:
> Hi Paul,
> 
> Something to think about ...
> 
> We can't allow normal fid allocation to eat into the cycle id space.
> 
> Suppose the last snapshot contains:
> 
>     cycle x, fid y
> 
> and y is very very close to overflow.  After that we advance to
> 
>     cycle x+1, fid z
> 
> If we lose journal, we only see (x,y) in recovery and we are going to reuse (x+1, z).
> 
> If this is right, then reserving space bits will cut down the
> number of available fid significantly, which is not what I have in mind.
> 
> 
> thanks,
> 
> -Zhihui


08/01/2011
----------

In light of the above issue, we never increment the cycle number behind the
administrator's back.  Instead, we print out warning and return ENOSPC.

It is up to the administrator to count how many crashes have happen since
the last available snapshot and bump the cycle number with our cursor_mgr
utility accordingly.  The goal is to skip any fids that might have been
issued by the MDS out there.