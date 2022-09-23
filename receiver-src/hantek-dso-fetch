#!/usr/bin/perl

use Getopt::Long;

my $debug = 0;
my $file = "hantek-dso-output-%d.csv";
my $repeated = 0;
my $sep = "\t";
my $grid = 25; ## ?

my $dev = glob("/dev/disk/by-id/usb-Waveform_Dump*-0:0");

GetOptions(
	"cont|continuous|repeated|r" => \$repeated,
	"file=s"   => \$file,
	"device=s"   => \$dev,
	"sep=s"   => \$sep,
	"debug+"  => \$debug);


my $disk = readlink($dev) or die "No Hantek Waveform Device found.\n";


if ($repeated) {
	open(MON, "udevadm monitor --kernel --subsystem-match=block/disk |") or die $!;

	my ($blockdev) = $disk =~ m,/([^/]+?)$,;

	$| = 1;
	print "Multi-capture enabled. Waiting for data.\n";
	while (<MON>) {
		next unless m,^KERNEL.*/block/$blockdev \(block\)$,;
		# In case there's a transmission problem, don't stop this loop!
		eval fetch_one();
	}
	close MON;
} else {
	fetch_one();
}


sub fetch_one
{

	open(F, "<", $dev) or die "can't open $dev: $!\n";
	binmode(F);
	sysread F, my $header, 128;

	my $output = sprintf($file, time());

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

	open(O, ">", $output) or die "Can't open '$output' for writing: $!\n";
	#print O "# header: @a \n"; #exit; # pack

	my @channels;
	my @calc;
	push @channels, [1,$v1+0, $off1] if $c1e;
	push @channels, [2,$v2+0, $off2] if $c2e;
	push @channels, [3,$v3+0, $off3] if $c3e;
	push @channels, [4,$v4+0, $off4] if $c4e;

	printf O "# CH%d: scale %f, offset %d\n", @$_ for @channels;

	for (@channels) {
		my ($idx, $scale, $off) = @$_;
		push @$_, eval "sub { (\$_[0] - $off) / " . ($scale/$grid) . " }";
	} 
	

	my @cols = qw(index time);
	push @cols, map { "raw.CH"  . $_->[0] } @channels;
	push @cols, map { "volt.CH" . $_->[0] } @channels;
	print O join($sep, @cols),"\n";

	while (1) {
		my $read = sysread(F, my $chunk, 2000);
		last if $read == 0;
		die "error reading: $!" if !defined($read);

		my $ch = shift(@channels);
		push @channels, $ch;
		$data_per_ch[$ch->[0]] .= $chunk;
	}
	# find smallest length
	my @lengths =  map { defined($_) ? length($_) : () } @data_per_ch;

	# Because of padding to 4KB we need to take the real sample count.
	push @lengths, $total_len/scalar(@channels);

	my ($max) = sort { $a <=> $b; } @lengths;
	for($i = 0; $i < $max; $i++) {
		my @volt;
		my @data;
		for $ch (@channels) {
			my ($idx, $scale, $off, $cb) = @$ch;

			my $byte = unpack("c", substr($data_per_ch[$idx], $i, 1));
			push @data, $byte;
			push @volt, &$cb($byte);
		}

		print O join($sep, $i, $i/$sampling_rate, @data, @volt),"\n";
	}

	close O;
	close F;

	printf "Fetched %d channels with %d samples into $file.\a\n",
		scalar(@channels), $max;
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

  --continuous
     Don't stop but loop and wait for new data,
     dump it to a CSV immediately

  --file=my-file-name-%d.csv
     Pattern for a file name;
     for --continuous it should include "%d" to insert a timestamp

  --device=/dev/sdb
     Hantek DSO2000 USB block device, normally autodetected

  --sep=,
     Separator to use in output, normally tab
