#!/usr/bin/perl
# Smoke test for D67: pack/unpack were completely non-functional — the
# parser built the AST nodes and runtime.c implemented the underlying C
# functions, but codegen never emitted calls to them at all, so every
# pack()/unpack() silently compiled to a no-op.
#
# NOTE: deliberately avoids any format/value combination that would produce
# an embedded NUL byte in the packed binary data (e.g. pack("N", small_int),
# or "a"'s NUL-padding) — that's a separate, deeper, NOT-fixed-here
# limitation (TESTS.md D85: PerlValue's string representation has no
# explicit length and relies on NUL-termination throughout the runtime).
# See d67_pack_unpack.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $packed = pack("C4", 65, 66, 67, 68);
check('smoke_pack_basic', $packed eq "ABCD");

my @vals = unpack("C*", "AB");
check('smoke_unpack_star', "@vals" eq "65 66");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d67_pack_unpack_smoke_done\n";
