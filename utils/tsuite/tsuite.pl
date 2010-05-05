#!/usr/bin/perl -W
# $Id$

use Getopt::Std;
use POSIX qw(:sys_wait_h :errno_h);
use IPC::Open3;
use Net::SMTP;
use strict;
use warnings;

my %opts;

sub fatalx {
	die "$0: @_\n";
}

sub fatal {
	die "$0: @_: $!\n";
}

sub usage {
	fatalx "usage: $0 [-mNqr] test-file\n";
}

sub init_env {
	my %r = @_;
	return join '', map { "export $_='$r{$_}'\n" } keys %r;
}

sub debug_msg {
	print WR @_, "\n" unless $opts{q};
}

sub execute {
	debug_msg "executing: ", join ' ', @_;
	system join ' ', @_;
}

sub slurp {
	my ($fn) = @_;
	local $/;

	open G, "<", "$fn" or fatal $fn;
	my $dat = <G>;
	close G;
	return ($dat);
}

sub dump_res {
	my ($res) = @_;
	print "resource:\n";
	while (my ($k, $v) = each %$res) {
		print "  $k: $v\n";
	}
	print "\n";
}

getopts("mNqr", \%opts) or usage();
usage() unless @ARGV == 1;

if ($opts{m}) {
	pipe RD, WR;
} else {
	*WR = *STDERR;
}

eval {

require $ARGV[0];
my $testname = $ARGV[0];
$testname =~ s!.*/!!;

our ($rootdir, $svnroot, @cli, $src, $intvtimeout, $runtimeout,
    $logbase, $global_env, $tmp_base);

# Sanity check variables
fatalx "rootdir not defined"	unless defined $rootdir;
fatalx "svnroot not defined"	unless defined $svnroot;
fatalx "cli not defined"	unless defined @cli;
fatalx "intvtimeout not defined" unless defined $intvtimeout;
fatalx "runtimeout not defined"	unless defined $runtimeout;
fatalx "svnroot not defined"	unless defined $svnroot;
fatalx "logbase not defined"	unless defined $logbase;
fatalx "global_env not defined"	unless defined $global_env;
fatalx "tmp_base not defined"	unless defined $tmp_base;

local $SIG{ALRM} = sub { fatal "interval timeout exceeded" };

sub waitjobs {
	my ($to) = @_;
	local $_;

	alarm $to;
	until (wait == -1) {
		fatalx "child process exited nonzero: " . ($? >> 8) if $?;
	}
	alarm 0;
}

sub runcmd {
	my ($cmd, $in) = @_;

	my $infh;
	open3($infh, ">&WR", ">&WR", $cmd);
	print $infh $in;
	close $infh;
}

sub mkdirs {
	my ($dir) = @_;
	my @cpn = split m!/!, $dir;
	my $fn = "";
	while (@cpn) {
		my $cpn = shift @cpn;
		next if $cpn eq "";
		$fn .= "/$cpn";
		unless (mkdir $fn) {
			return 0 unless $! == EEXIST;
		}
	}
	return 1;
}

fatal "$rootdir" unless -d $rootdir;

my $base;
my $tsid;

# Grab a unique base directory
do {
	$tsid = sprintf "%06d", int rand 1_000_000;
	$base = "$rootdir/slsuite.$tsid";
} while -d $base;

debug_msg "base dir = $base";

my $mp = "$base/mp";
my $tmpdir = "$tmp_base/slsuite.$tsid";
my $datadir = "$tmpdir/data";

mkdirs $base		or fatal "mkdirs $base";
mkdirs $tmpdir		or fatal "mkdirs $tmpdir";
mkdir "$base/ctl"	or fatal "mkdir $base/ctl";
mkdir "$base/fs"	or fatal "mkdir $base/fs";
mkdir $mp		or fatal "mkdir $mp";

# Checkout the source and build it
chdir $base		or fatal "chdir $base";
if (defined($src)) {
	symlink $src, "$base/src" or fatal "symlink $src, $base/src";
} else {
	$src = "$base/src";
	debug_msg "svn checkout -q $svnroot $src";
	execute "svn checkout -q $svnroot $src";
	fatalx "svn failed" if $?;

	debug_msg "make build";
	execute "cd $src/fuse/fuse-2.8.1 && ./configure >/dev/null && make >/dev/null";
	fatalx "make failed" if $?;
	execute "cd $src/slash_nara && make zbuild >/dev/null";
	fatalx "make failed" if $?;
	execute "cd $src/slash_nara && make build >/dev/null";
	fatalx "make failed" if $?;
}

my $slbase = "$src/slash_nara";
my $tsbase = "$slbase/utils/tsuite";

my $zpool = "$slbase/utils/zpool.sh";
my $zfs_fuse = "$slbase/utils/zfs-fuse.sh";
my $slmkjrnl = "$slbase/slmkjrnl/slmkjrnl";
my $slkeymgt = "$slbase/slkeymgt/slkeymgt";
my $odtable = "$src/psc_fsutil_libs/utils/odtable/odtable";
my $slimmns_format = "$slbase/slimmns/slimmns_format";

my $ssh_init = <<EOF;
	set -ex

	runscreen()
	{
		scrname=\$1
		shift
		screen -S \$scrname -X quit || true
		screen -d -m -S \$scrname.$tsid "\$\@"
	}

	waitforscreen()
	{
		scrname=\$1
		while screen -ls | grep -q \$scrname.$tsid; do
			[ \$SECONDS -lt $runtimeout ]
			sleep 1
		done
	}

	cd $base;
EOF

# Setup configuration
my $conf = slash_conf(base => $base);
open SLCONF, ">", "$base/slash.conf" or fatal "open $base/slash.conf";
print SLCONF $conf;
close SLCONF;

my @mds;
my @ion;

sub new_res {
	my ($rname, $site) = @_;

	my %r = (
		rname	=> $rname,
		site	=> $site,
	);
	return \%r;
}

sub res_done {
	my ($r) = @_;

	if ($r->{type} eq "mds") {
		fatalx "MDS $r->{rname} has no \@zfspool configuration"
		    unless $r->{zpool_args};
		push @mds, $r;
	} else {
		fatalx "MDS $r->{rname} has no \@prefmds configuration"
		    unless $r->{prefmds};
		push @ion, $r;
	}
}

# Parse configuration for MDS and IONs
sub parse_conf {
	my $in_site = 0;
	my $site_name;
	my $r = undef;
	my @lines = split /\n/, $conf;

	for (my $ln = 0; $ln < @lines; $ln++) {
		my $line = $lines[$ln];
		if ($in_site) {
			if ($r) {
				if ($line =~ /^\s*type\s*=\s*(\S+)\s*;\s*$/) {
					$r->{type} = $1;
				} elsif ($line =~ /^\s*id\s*=\s*(\d+)\s*;\s*$/) {
					$r->{id} = $1;
				} elsif ($line =~ /^\s*#\s*\@zfspool\s*=\s*(\w+)\s+(.*)\s*$/) {
					$r->{zpoolname} = $1;
					$r->{zpool_args} = $2;
				} elsif ($line =~ /^\s*#\s*\@prefmds\s*=\s*(\w+\@\w+)\s*$/) {
					$r->{prefmds} = $1;
				} elsif ($line =~ /^\s*fsroot\s*=\s*(\S+)\s*;\s*$/) {
					($r->{fsroot} = $1) =~ s/^"|"$//g;
				} elsif ($line =~ /^\s*ifs\s*=\s*(.*)$/) {
					my $tmp = $1;

					for (; ($line = $lines[$ln]) !~ /;/; $ln++) {
						$tmp .= $line;
					}
					$tmp =~ s/;\s*$//;
					$r->{host} = (split /\s*,\s*/, $tmp, 1)[0];
				} elsif ($line =~ /^\s*}\s*$/) {
					res_done($r);
					$r = undef;
				}
			} else {
				if ($line =~ /^\s*resource\s+(\w+)\s*{\s*$/) {
					$r = new_res($1, $site_name);
				}
			}
		} else {
			if ($line =~ /^\s*site\s+@(\w+)\s*{\s*$/) {
				$site_name = $1;
				$in_site = 1;
			}
		}
	}
}

# Setup client commands
open CLICMD, ">", "$base/cli_cmd" or fatal "open $base/cli_cmd";
print CLICMD "set -e;\n";
print CLICMD cli_cmd(
	fspath	=> $mp,
	base	=> $base,
	src	=> $src,
	logbase	=> $logbase,
	gdbtry	=> "$src/tools/gdbtry.pl",
	doresults => "perl $src/tools/tsuite_results.pl " .
		($opts{N} ? "-N " : "") . ($opts{m} ? "-m " : "") .
		" $testname $logbase");
close CLICMD;

parse_conf();

my ($i);
my $need_authbuf_key = 1;

# Create the MDS file systems
foreach $i (@mds) {
	debug_msg "initializing slashd environment: $i->{rname} : $i->{host}";
	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		@{[init_env(%$global_env)]}

		screen -S SLMDS -X quit || true

		$zfs_fuse &
		sleep 2
		$zpool destroy $i->{zpoolname} || true
		$zpool create -f $i->{zpoolname} $i->{zpool_args}
		$slimmns_format /$i->{zpoolname}
		sync; sync
		umount /$i->{zpoolname}
		pkill zfs-fuse

		mkdir -p $datadir

		$slmkjrnl -D $datadir -f
		$odtable -C -N $datadir/ion_bmaps.odt

		@{[$need_authbuf_key ? <<EOS : "" ]}
		$slkeymgt -c -D $datadir
		cp -p $datadir/authbuf.key $base
EOS
EOF
	$need_authbuf_key = 0;
}

waitjobs $intvtimeout;

# Launch MDS servers
my $slmgdb = slurp "$tsbase/slashd.gdbcmd";
foreach $i (@mds) {
	debug_msg "launching slashd: $i->{rname} : $i->{host}";

	my $dat = $slmgdb;
	$dat =~ s/%zpool_name%/$i->{zpoolname}/g;
	$dat =~ s/%datadir%/$datadir/g;

	open G, ">", "$base/slashd.$i->{id}.gdbcmd" or fatal "write slashd.gdbcmd";
	print G $dat;
	close G;

	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		@{[init_env(%$global_env)]}
		cp -p $base/authbuf.key $datadir
		runscreen SLMDS \\
		    gdb -f -x $base/slashd.$i->{id}.gdbcmd $slbase/slashd/slashd
EOF
}

waitjobs $intvtimeout;

# Wait for the server control sockets to appear
alarm $intvtimeout;
sleep 1 until scalar @{[ glob "$base/ctl/slashd.*.sock" ]} == @mds;
alarm 0;

# Create the ION file systems
foreach $i (@ion) {
	debug_msg "initializing sliod environment: $i->{rname} : $i->{host}";
	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		@{[init_env(%$global_env)]}
		mkdir -p $datadir
		mkdir -p $i->{fsroot}
		$slimmns_format -i $i->{fsroot}
		cp -p $base/authbuf.key $datadir
EOF
}

waitjobs $intvtimeout;

# Launch the ION servers
my $sligdb = slurp "$tsbase/sliod.gdbcmd";
foreach $i (@ion) {
	debug_msg "launching sliod: $i->{rname} : $i->{host}";

	my $dat = $sligdb;
	$dat =~ s/%datadir%/$datadir/g;

	open G, ">", "$base/sliod.$i->{id}.gdbcmd"
	    or fatal "write sliod.gdbcmd";
	print G $dat;
	close G;

	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		@{[init_env(%$global_env, SLASH_MDS_NID => $i->{prefmds})]}
		runscreen SLIOD \\
		    gdb -f -x $base/sliod.$i->{id}.gdbcmd $slbase/sliod/sliod
EOF
}

waitjobs $intvtimeout;

# Wait for the server control sockets to appear
alarm $intvtimeout;
sleep 1 until scalar @{[ glob "$base/ctl/sliod.*.sock" ]} == @ion;
alarm 0;

# Launch the client mountpoints
foreach $i (@cli) {
	debug_msg "launching mount_slash: $i->{host}";
	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		@{[init_env(%$global_env, %{$i->{env}})]}
		runscreen MSL sh -c "gdb -f -x $tsbase/msl.gdbcmd \\
		    $slbase/mount_slash/mount_slash; umount $mp"
EOF
}

waitjobs $intvtimeout;

# Wait for the client control sockets to appear
alarm $intvtimeout;
sleep 1 until scalar @{[ glob "$base/ctl/msl.*.sock" ]} == @cli;
alarm 0;

# Spawn monitors/gatherers of control stats
foreach $i (@mds) {
	debug_msg "spawning slashd stats tracker: $i->{rname} : $i->{host}";
	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		runscreen SLMCTL sh -c "sh $tsbase/ctlmon.sh $i->{host} \\
		    $slbase/slmctl/slmctl -S ctl/slashd.$i->{host}.sock -Pall -Lall -iall || \$SHELL"
EOF
}

waitjobs $intvtimeout;

foreach $i (@ion) {
	debug_msg "spawning sliod stats tracker: $i->{rname} : $i->{host}";
	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		runscreen SLICTL sh -c "sh $tsbase/ctlmon.sh $i->{host} \\
		    $slbase/slictl/slictl -S ctl/sliod.$i->{host}.sock -Pall -Lall -iall || \$SHELL"
EOF
}

waitjobs $intvtimeout;

foreach $i (@cli) {
	debug_msg "spawning mount_slash stats tracker: $i->{host}";
	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		runscreen MSCTL sh -c "sh $tsbase/ctlmon.sh $i->{host} \\
		    $slbase/msctl/msctl -S ctl/msl.$i->{host}.sock -Pall -Lall -iall || \$SHELL"
EOF
}

waitjobs $intvtimeout;

# Run the client applications
foreach $i (@cli) {
	debug_msg "client: $i->{host}";
	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		runscreen SLCLI sh -c "sh $base/cli_cmd $i->{host} || \$SHELL"
		waitforscreen SLCLI
EOF
}

waitjobs $runtimeout;

# Unmount mountpoints
foreach $i (@cli) {
	debug_msg "unmounting mount_slash: $i->{host}";
	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		umount $mp
EOF
}

waitjobs $intvtimeout;

# Kill IONs
foreach $i (@ion) {
	debug_msg "stopping sliod: $i->{rname} : $i->{host}";
	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		$slbase/slictl/slictl -S $base/ctl/sliod.%h.sock -c exit
EOF
}

waitjobs $intvtimeout;

# Kill MDS's
foreach $i (@mds) {
	debug_msg "stopping slashd: $i->{rname} : $i->{host}";
	runcmd "ssh $i->{host} sh", <<EOF;
		$ssh_init
		$slbase/slmctl/slmctl -S $base/ctl/slashd.%h.sock -c exit
EOF
}

waitjobs $intvtimeout;

foreach $i (@cli) {
	debug_msg "force quitting mount_slash screens: $i->{host}";
	execute "ssh $i->{host} screen -S MSL.$tsid -X quit";
	execute "ssh $i->{host} screen -S MSCTL.$tsid -X quit";
}

foreach $i (@ion) {
	debug_msg "force quitting sliod screens: $i->{rname} : $i->{host}";
	execute "ssh $i->{host} screen -S SLIOD.$tsid -X quit";
	execute "ssh $i->{host} screen -S SLICTL.$tsid -X quit";
}

foreach $i (@mds) {
	debug_msg "force quitting slashd screens: $i->{rname} : $i->{host}";
	execute "ssh $i->{host} screen -S SLMDS.$tsid -X quit";
	execute "ssh $i->{host} screen -S SLMCTL.$tsid -X quit";
}

# Clean up files
if ($opts{r}) {
	debug_msg "deleting base dir";
	execute "rm -rf $base";
}

}; # end of eval

my $emsg = $@;

if ($opts{m}) {
	close WR;

	my @lines = <RD>;

	if (@lines || $emsg) {
		my $smtp = Net::SMTP->new('mailer.psc.edu');
		$smtp->mail('slash2-devel@psc.edu');
		$smtp->to('slash2-devel@psc.edu');
		$smtp->data();
		$smtp->datasend("To: slash2-devel\@psc.edu\n");
		$smtp->datasend("Subject: tsuite errors\n\n");
		$smtp->datasend("Output from run by ",
		    ($ENV{SUDO_USER} || $ENV{USER}), ":\n\n");
		$smtp->datasend(@lines) if @lines;
		$smtp->datasend("error: $emsg") if $emsg;
		$smtp->dataend();
		$smtp->quit;
	}
}

print "error: $emsg\n" if $emsg;
exit 1 if $emsg;
