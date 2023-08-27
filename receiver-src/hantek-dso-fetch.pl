#!/usr/bin/perl

use Getopt::Long;
use Expect;
use Socket;

my $debug = 0;
my $file = "hantek-dso-output-%d.csv";
my $repeated = 0;
my $sep = "\t";

my $grid = 25;
my $block_size = 4000;


my $dev = $ENV{"HANTEK_DEVICE"};
my $env = "RECEIVED_HANTEK_FILE";
my $run = undef;

GetOptions(
	"cont|continuous|repeated|r|c" => \$repeated,
	"now"      => \$now,
	"file=s"   => \$file,
	"run=s"    => \$run,
	"device=s" => \$dev,
	"sep=s"    => \$sep,
	"debug+"   => \$debug) or die(<DATA>);

# one remaining arguments may be the file pattern.
$file = shift() if @ARGV == 1;

my $dev_tty = "/dev/ttyACM0";
my $dev_tcp = "hantek.local";
my $dev_tcp = "172.31.254.254:8001";

$dev ||= $dev_tcp;
my $fetcher = open_function($dev);
our $fh;

if ($now) {
	print "Immediate capture...\n";
	&$fetcher(1);
} elsif ($repeated) {
	$| = 1;
	# $SIG{"CHLD"} = "IGNORE";
	print "Multi-capture enabled. Waiting for data.\n";

	warn("No %d found in filename - adding that.\n"),
		$file .= ".%d"
		unless $file =~ /\%[dsx]/;

	while (1) {
		# In case there's a transmission problem, don't stop this loop!
		# TODO: re-open device
		eval &$fetcher();
	}
} else {
	&$fetcher();
}


exit;

sub open_serial
{
	my($dev) = @_;

	unless ($fh) {
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
	}

	return $fh;
}

sub open_tcp
{
	my($tgt) = @_;

	my ($ip6, $ip4, $hostname, $port) =
	$tgt =~ m{^ (?: tcp: /?/? )?
		(?: \[ ( [a-f0-9:]+ ) \] |
		( [0-9.]+ ) |
		( [0-9a-zA-Z.-]+ ) )
		(?: : (\d+) )?$}xi;
	my $dest = $hostname || $ip4 || $ip6;
	return undef unless $port && $dest;

	return sub {
		my($now) = @_;
		socket(my $socket, PF_INET, SOCK_STREAM, 0)
			or die "socket: $!";

		connect($socket, pack_sockaddr_in($port, inet_aton($dest)))
			or die "connect: $!";

		binmode($socket);
		if ($now) {
			syswrite $socket, "now\n";
			$socket->flush();
			alarm(2);
		} else {
			# No timeout here, we need to wait for the "Save to USB" keypress!
			alarm(0);
		}
		really_fetch($socket);
		close $socket;
	};
}

sub open_function
{
	my($dev) = @_;
	
	my $f = open_tcp($dev);
	if ($f) {
		return $f;

	} elsif ($dev =~ m,^/dev/tty,) {
		my $fh = open_serial($dev);
		return sub {
			fetch_serial($fh);
		};
	} else {
		die "Bad device '$dev'\n";
	}
}


sub fetch_serial
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

	alarm(2);
	really_fetch($fh);
}

sub really_fetch
{
	my($fh) = @_;

	local $SIG{ALRM} = sub { unlink $tmpfile; die "alarm\n" };

	# wait for header before determining timestamp
	read $fh, my $header, 128;

	my $output = sprintf($file, time());
	my $tmpfile = "$output.tmp";
	my $file_type = ($output =~ /\.wave?$/i ? 'W' : 'C');

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
	# pack

	die "Wrong magic: $header" unless $head eq '#9';

	my @channels;
	my @calc;
	push @channels, [1,$v1+0, $off1] if $c1e;
	push @channels, [2,$v2+0, $off2] if $c2e;
	push @channels, [3,$v3+0, $off3] if $c3e;
	push @channels, [4,$v4+0, $off4] if $c4e;


	# Because of padding to 4KB we need to take the real sample count.
	my ($max) = $total_len/scalar(@channels);

	# A space before the dots so the filename can be copy/pasted as a <cword>.
	printf "Fetching %d channels with %d samples into $output ... ",
		scalar(@channels), $max;

	open(O, ">", $tmpfile) or die "Can't open '$tmpfile' for writing: $!\n";
	#print O "# header: @a \n"; #exit; # pack

	if ($file_type eq 'C') {
		printf O "# CH%d: scale %f, offset %d\n", @$_ for @channels;

		my @cols = qw(index time);
		push @cols, map { "raw.CH"  . $_->[0] } @channels;
		push @cols, map { "volt.CH" . $_->[0] } @channels;
		print O join($sep, @cols),"\n";
	} elsif ($file_type eq 'W') {

		binmode(O);
		# https://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html
		
		# TODO: use 16bit signed instead?
		$bytes_per_sample = 1;
		$data_len = $bytes_per_sample * @channels * $max;
		my $fmt = pack('S S L L S S', 
			0x0001, # format PCM
			scalar(@channels),
			$sampling_rate, # sample rate
			$bytes_per_sample*$sampling_rate*@channels, # data rate
			$bytes_per_sample*@channels, # align
			$bytes_per_sample*8);
		print O pack('a4 L a4 a4 L a* a4 L',
			'RIFF', 4 + 4*2 + length($fmt) + 4*2 + $data_len,
			'WAVE',
			'fmt ', length($fmt),
			$fmt,
			'data', $data_len);
	} else {
		die "Unknown filetype??";
	}

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

		if ($file_type eq 'C') {
			my @volt;
			my @data;
			for my $ch (@channels) {
				my ($idx, $scale, $off, $data) = @$ch;

				my $byte = unpack("c", substr($data, $i-$chunk_start, 1)); 
				push @data, $byte;
				push @volt, AbsVolt($byte, $scale, $off);
			}
			print O join($sep, $i, $i/$sampling_rate, @data, @volt),"\n";
		} elsif ($file_type eq 'W') {
			for my $ch (@channels) {
				my $byte = unpack("c", substr($ch->[3], $i-$chunk_start, 1)); 
				print O pack("c", $byte + 0x80);
			}
		}
	}
	alarm(0);

	close O;
	close F;
	rename $tmpfile, $output;

	my $chld;

	if ($run) {
		my $pid = fork();
		die "Can't fork: $!" unless defined($pid);
		if (!$pid) {
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
  --device=hantek.local
  --device=172.31.254.254:8001
     Hantek DSO2000 USB device to use.
     If not given as argument, default is taken from the
     environment variable $HANTEK_DEVICE;
     if that doesn't exist or is empty,
     /dev/ttyACM0 and TCP via hantek.local are tried.
     A serial device must be passed in including the /dev/ prefix!

  --sep=,
     Separator to use in output, normally tab

  --continuous
  --cont
     Don't stop but loop and wait for new data,
     dump it to a CSV immediately

  --now
     Tell the DSO to return data immediately, without waiting
     for a "Save to USB" keypress.

  --run=
     Specifies a shell command that is run in the background
     after receiving a new data file in continuous mode;
     the current file name is available as environment
     variable RECEIVED_HANTEK_FILE.
