---
layout: post
title: avoiding syslog flooding
author: yanovich
type: progress
---

With a large deployment featuring many nodes including clients and I/O servers, it is useful to centralize all error logs generated by the various daemons to one place to scan for problems and react as necessary.  However, because of abuses in either (1) buggy SLASH2 causing flooding of syslog activity or (2) other applications that are sharing the same syslog receivers, it is possible that <tt>rsyslogd</tt> can backup and become unresponsive.

In these situations, any applications using <tt>syslog(3)</tt> with a remote configuration with essentially stall until the remote <tt>rsyslogd</tt> stops thrashing.  Once cleared, service can return to normal, but any behavior in the interim that generates log messages, such as RPC timeouts, can bring the deployment into a chicken-and-egg dependency loop of threads stuck in our debug logging routines while awaiting syslog transmission.

Of course, fixing the spam is the major solution to this problem, but that still does not fix case #2 outlined above, which is exactly what happened in on of our SLASH2 deployments.  The proper solution is remove the network dependency from an essential code path in the SLASH2 code base.

This alleviation is the introduction of the <tt>PFL_SYSLOG_PIPE</tt> environment variable.  Instead of issuing <tt>syslog(3)</tt> directly, this variable arranges that stderr be written to as a normal file somewhere on the system (requiring some local storage for netboots if high volumes of debug logging traffic are to be generated) and for a <tt>logger(1)</tt> process to be spawned doing the <tt>syslog(3)</tt> on behalf so the application does not grind to a halt.

Not having debug logs, or even genuine system activity logs for that matter, is unfortunate but a class altogether different from a completely unresponsive system.