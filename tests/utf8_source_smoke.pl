#!/usr/bin/perl
# Smoke: use utf8 treats source string literals as characters, not bytes.
use strict;
use warnings;
use utf8;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

# é is U+00E9, UTF-8 C3 A9 (2 bytes). With use utf8, length is 1.
check('len_eacute', length("é") == 1);
check('ord_eacute', ord("é") == 0xE9);
check('interp', length("xéy") == 3);

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "utf8_source_smoke_done\n";
