#!/usr/bin/perl
# Deep: D90 — length/ord/substr on binary vs Unicode
print "=== pack ===\n";
my $b = pack("N", 1234567);
print "len=", length($b), "\n";
print "unpack=", join(",", unpack("C*", $b)), "\n";
print "ord0=", ord(substr($b, 0, 1)), "\n";

print "=== chr bytes ===\n";
my $d = chr(200);
print "len=", length($d), " ord=", ord($d), "\n";
my $s = chr(233) . "bc";
print "d68len=", length($s), " ord0=", ord(substr($s, 0, 1)), " ord1=", ord(substr($s, 1, 1)), "\n";

print "=== unicode chr ===\n";
my $c = chr(0x2603);
print "snow=", length($c), "\n";
my $e = $c . "x";
print "concat=", length($e), "\n";

print "d90_length_binary_done\n";
