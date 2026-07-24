#!/usr/bin/perl
# Smoke: D90 — length() on pack binary / chr
my $b = pack("N", 1234567);
die "pack len" unless length($b) == 4;
die "chr200" unless length(chr(200)) == 1 && ord(chr(200)) == 200;
die "snow" unless length(chr(0x2603)) == 1;
print "ok\n";
