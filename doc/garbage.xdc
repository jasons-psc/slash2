<?xml version="1.0" ?>
<!-- $Id$ -->

<xdc xmlns:oof="http://www.psc.edu/~yanovich/xsl/oof-1.0">
	<title>Garbage collection and reclamation</title>



(1) Write each unlink operation into the system log before replying the unlink RPC.

(2) Distill the log entry from the tile and write into the one of the unlink log files.

    Each unlink entry consists of the following information:

	* The identity of the file: SLASH FID and generation number
	* The identities of the I/O servers that have a copy of the file at the moment.

(3) Read the unlink log files and send RPCs to I/O servers.  An unlink log file will be removed when
    all relevant I/O servers have responded.

06/16/2010
----------

Currently, we use tiling code to distill some log entries (those related to namespace
update and truncation) from the system journal for further processing.  Every log
entry is written to the journal and the tile at the same time.  We have to adjust
the tiles to map to different regions of the system journal as time goes by.
The main motivation of tiles is to cache the log entries and avoid reading the
system journal again.

Since we already have a list of pending transactions, we could leverage that
list for the purpose of distilling.  That way, there is no need to maintain
tiles.  Data associated with log entries can just hang off their respective
transaction handles.

Because we determine the current log tail by looking at the head of the list,
we can also prevent a log entry from being reused before it is distilled.
Not all entries need to be distilled.

We also need to log some counters (bmap sequence number and SLASH FID), we
can insert them periodically in the log, so we are never worried that they
will be overwritten or do not exist at all.

Bmap sequence numbers are used to time out old bmaps on the I/O servers.
We don't do time syncronization amoung clients and servers.  So it is
entirely up to the MDS to decide when to time out bmaps it has issued.

01/05/2011
----------

Today Paul found a problem in which the mds have 30MB/s I/O every 1 minute.
This slows down the readdir performance a lot.

It turns out that one of the IOS (likely tahini) is down, and the MDS still
tries to send reclaim log to it.  It does so by reading the log file first,
only to find out that the IOS can not be contacted.

In the short time, there are two ways to attack this:

(1) Make sure that an IOS is alive before reading a reclaim log file
    for it.

(2) Print out the down IOS in the log, so that sys admin can take action
    to correct it.

In the long term, we may need to sort out the residency of a deleted
file first before sending down the garbage reclamation RPCs to the IOS.

Right now, partial truncate is handled differently by creating special
files to represent the work to do.  Maybe that is the way to go.

</xdc>
