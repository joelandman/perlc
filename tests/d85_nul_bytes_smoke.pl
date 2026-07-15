#!/usr/bin/perl
# Smoke test for D85: PerlValue's string representation had no explicit
# length field, so pack()'d binary data containing an embedded NUL byte
# (e.g. pack("N", 1234567), a hugely common 4-byte network-order pack)
# was silently truncated the moment it was wrapped into a scalar.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $packed = pack("N", 1234567);
my ($n) = unpack("N", $packed);
check('smoke_pack_unpack_roundtrip_with_embedded_nul', $n == 1234567);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d85_nul_bytes_smoke_done\n";
