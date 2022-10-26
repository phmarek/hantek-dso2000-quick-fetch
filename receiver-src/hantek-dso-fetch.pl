#!/usr/bin/perl

use Getopt::Long;
use Expect;

my $debug = 0;
my $file = "hantek-dso-output-%d.csv";
my $repeated = 0;
my $sep = "\t";

my $grid = 25;
my $block_size = 4000;

my $dev = "/dev/ttyACM0";
my $env = "RECEIVED_HANTEK_FILE";
my $run = undef;

GetOptions(
	"cont|continuous|repeated|r" => \$repeated,
	"file=s"   => \$file,
	"run=s"   => \$run,
	"device=s"   => \$dev,
	"sep=s"   => \$sep,
	"debug+"  => \$debug) or die(<DATA>);

# one remaining arguments may be the file pattern.
$file = shift() if @ARGV == 1;


our $fh = login();

if ($repeated) {
	$| = 1;
	# $SIG{"CHLD"} = "IGNORE";
	print "Multi-capture enabled. Waiting for data.\n";
	while (1) {
		# In case there's a transmission problem, don't stop this loop!
		# TODO: re-open device
		eval fetch_one($fh);
	}
} else {
	fetch_one($fh);
}


exit;

sub login
{
	open($fh, "+<", $dev) or die "Can't open $dev\n";
	binmode($fh);
	my $pid = fork();
	if ($pid == 0) {
		open(STDIN, "<&", $fh) or die $!;
		exec("stty","-echo", "raw", "pass8");
		die;
		exit 1;
	}
	die "Can't fork" if !defined $pid;
	waitpid($pid, 0) or die $!;

	# Empty serial buffer - the login process etc. might have been running

	my $mask = "";
	vec($mask, fileno($fh), 1) = 1;
	sysread($fh, my $void, 4096) while select(my $r=$mask, undef, undef, 0);

	return $fh;

	#	syswrite($fh, "\n");
	#	sysread($fh, $buf, 100) or die $!;
	#	print $buf;
	#	exit;
	$expect = Expect->exp_init($fh);
	$expect->raw_pty(1);
	$expect->expect(1.0, "login:") or die;
	$expect->send("root\r");
	$expect->expect(1.0, '~$') or die;
	$expect->send("export PS1='----'\r");
	$expect->expect(1.0, '----') or die;
	exit;
}


sub fetch_one
{
	my($fh) = @_;

	# Ping pong - a 3-way handshake.
	my $my_tag = time();

	print $fh "\n\n\nqf.ping $my_tag\n\n";
	# This waits for the DSO - so we must not timeout here.
	my $buf, $rcvd_tag, $remote_tag;
	while ($my_tag != $rcvd_tag) {
		sysread($fh, $buf, 4096) or die $!;

		$rcvd_tag=$1, $remote_tag=$2 while ($buf =~ /\s*qf\.pong (\d+) (\d+)\s+/g);
	}
	# Send final ACK, and allow time sync.
	my $pung = "qf.pung $remote_tag " . time() . "\n";
	syswrite $fh, $pung;


	local $SIG{ALRM} = sub { unlink $tmpfile; die "alarm\n" };
	alarm(1);

	read $fh, my $header, 128;

	my $output = sprintf($file, time());
	my $tmpfile = "$output.tmp";

	#print length($header), " ", unpack("H*", $header), "\n";

	my ($head, $first_len, $total_len, $upl_len,
		$running, $trigger,
		$unknown1,
		$off1, $off2, $off3, $off4,
		$v1, $v2, $v3, $v4,
		$c1e, $c2e, $c3e, $c4e,
		$sampling_rate, $sampling_multiple,
		$trigger_time, $acq_start,
		$unknown2,
	) = my @a =
	unpack('a2 a9a9a9 a1a1 a8 s1s1s1s1 a7a7a7a7 a1a1a1a1 a9 a6 x9 a9 a6 a10', $header);
	# TODO: signed LE short not available? For the offsets.

	die "Wrong magic: $header" unless $head eq '#9';

	open(O, ">", $tmpfile) or die "Can't open '$tmpfile' for writing: $!\n";
	#print O "# header: @a \n"; #exit; # pack

	my @channels;
	my @calc;
	push @channels, [1,$v1+0, $off1] if $c1e;
	push @channels, [2,$v2+0, $off2] if $c2e;
	push @channels, [3,$v3+0, $off3] if $c3e;
	push @channels, [4,$v4+0, $off4] if $c4e;

	printf O "# CH%d: scale %f, offset %d\n", @$_ for @channels;

	# Because of padding to 4KB we need to take the real sample count.
	my ($max) = $total_len/scalar(@channels);

	# A space before the dots so the filename can be copy/pasted as a <cword>.
	printf "Fetching %d channels with %d samples into $output ... ",
	scalar(@channels), $max;

	my @cols = qw(index time);
	push @cols, map { "raw.CH"  . $_->[0] } @channels;
	push @cols, map { "volt.CH" . $_->[0] } @channels;
	print O join($sep, @cols),"\n";
	# find smallest length

	my $chunk_size = $block_size / scalar(@channels);
	my $bytes_processed = 0;
	my $chunk_start = 0;
	for(my $i = 0; $i < $max; $i++) {

		if ($i == $chunk_start) {
			# Fetch a set of data chunks
			for my $ch (@channels) {
				alarm(1);
				my $read = read($fh, my $chunk, $chunk_size);
				last if $read == 0;
				die "error reading: $!" if !defined($read);
				# printf "=== got %d bytes for %d\n", $read, $ch->[0];

				$bytes_processed += $read;
				$ch->[3] = $chunk;
			}
			$chunk_start += $chunk_size;
		}

		my @volt;
		my @data;
		for my $ch (@channels) {
			my ($idx, $scale, $off, $data) = @$ch;

			my $byte = unpack("c", substr($data, $i-$chunk_start, 1)); 
			push @data, $byte;
			push @volt, AbsVolt($byte, $scale, $off);
		}

		print O join($sep, $i, $i/$sampling_rate, @data, @volt),"\n";
	}
	alarm(0);

	close O;
	close F;
	rename $tmpfile, $output;

	my $chld;

	if ($run) {
		my $pid = fork();
		die "Can't fork: $!" unless defined($pid);
		if ($pid) {
			open(STDIN, "< /dev/null") or die $!;
			# TODO: reset STDOUT and STDERR as well?
			# Would need to remember STDERR for the error message below, though.

			$ENV{$env} = $output;
			
			system($run);
			die "Hantek quick fetch: error running shell command: $?" if $?;
			exit;
		}

		$chld = ", spawned child $pid";
	}

	printf "Done! $bytes_processed bytes$chld.\a\n";
}

sub AbsVolt
{
	my($byte, $scale, $off) = @_;

	#	print "$byte - $off / $grid * $scale\n";# exit;
	return ($byte-$off)/$grid*$scale;
}

__DATA__

Quickly fetching the waveform from a Hantek DSO2000 series oscilloscope.

REQUIRES THAT THE "phoenix" BINARY IS PATCHED VIA "LD_PRELOAD"!


Options:

  --file=my-file-name-%d.csv
     Pattern for a file name;
     for --continuous it should include "%d" to insert a timestamp

  --device=/dev/ttyACM0
     Hantek DSO2000 USB serial device

  --sep=,
     Separator to use in output, normally tab

  --continuous
  --cont
     Don't stop but loop and wait for new data,
     dump it to a CSV immediately

  --run=
     Specifies a shell command that is run in the background
     after receiving a new data file in continuous mode;
     the current file name is available as environment
     variable RECEIVED_HANTEK_FILE.
