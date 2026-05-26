#!/usr/bin/perl
use strict;
use warnings;

# Test: $$ — process ID
my $pid = $$;
print "pid_ok=" . (($pid > 0) ? "yes" : "no") . "\n";

# Test: $^O — OS name
my $os = $^O;
print "os=$os\n";

# Test: fileno
open(my $fh, "<", "/dev/null") or die "open failed: $!";
my $fn = fileno($fh);
print "fileno_ok=" . (($fn >= 0) ? "yes" : "no") . "\n";
close($fh);

# Test: read()
open(my $rfh, "<", "/dev/zero") or die "open /dev/zero: $!";
my $buf = "";
my $n = read($rfh, $buf, 4);
close($rfh);
print "read_ok=" . (($n == 4) ? "yes" : "no") . "\n";

# Test: truncate
my $tmpfile = "/tmp/perlc_tier3_test_$$";
open(my $wfh, ">", $tmpfile) or die "open: $!";
print $wfh "hello world";
close($wfh);
truncate($tmpfile, 5);
open(my $vfh, "<", $tmpfile) or die "open: $!";
my $contents = <$vfh>;
close($vfh);
unlink($tmpfile);
print "truncate_ok=" . (($contents eq "hello") ? "yes" : "no") . "\n";

# Test: each %hash — count iterations
my %h = (a => 1, b => 2, c => 3);
my $each_count = 0;
my @kv1 = each %h;
my @kv2 = each %h;
my @kv3 = each %h;
my @kv4 = each %h;  # should be empty (exhausted)
$each_count = scalar(@kv1) + scalar(@kv2) + scalar(@kv3);
my $each_ok = ($each_count == 6 && scalar(@kv4) == 0);
print "each_ok=" . ($each_ok ? "yes" : "no") . "\n";

# Test: pos()
my $str = "hello world hello";
$str =~ /hello/g;
my $p = pos($str);
print "pos_ok=" . (($p == 5) ? "yes" : "no") . "\n";

# Test: getpid() via $$
my $pid2 = $$;
print "pid_consistent=" . ($pid == $pid2 ? "yes" : "no") . "\n";

print "tier3_done\n";
