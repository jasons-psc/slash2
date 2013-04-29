<?xml version="1.0" ?>
<!-- $Id$ -->

<xdc>
	<title>Configuring a SLASH2 deployment</title>

	<h1>Overview</h1>
	<p>
		This document describes the network stack initialization and
		slash.slcfg automatic profile self discovery.
	</p>
	<p>
		The following actions are taken depending on the value of the 'nets'
		slash.slcfg option:
	</p>
	<list>
		<list-item>
			if 'nets' is a single Lustre network name, such as "tcp10",
			network interface/Lustre network name pairs are constructed from
			each interface address registered in the system and this specified
			Lustre network name.
			The resulting list of pairs is copied into the daemon process
			environment as LNET_NETWORKS.
		</list-item>
		<list-item>
			if 'nets' is a fully fledged LNET_IP2NETS value, this value is
			used to determine which addresses held by SLASH2 peers correspond
			to which Lustre networks and the system's network interfaces are
			queried to determine routes to such peers.
		</list-item>
	</list>
	<p>
		These behaviors will be overridden if LNET_NETWORKS is already
		specified in the process environment, which always takes precedence.
	</p>
	<p>
		Otherwise, LNET_NETWORKS is fabricated based on the values specified
		in the SLASH2 configuration.
	</p>
	<p>
		LNET_NETWORKS is then parsed to determine routable addresses and
		their corresponding Lustre network names.
		These pairs are used to assign Lustre networks to the resource node
		addresses listed in the slash.slcfg site resources configuration.
	</p>
</xdc>
