#!/usr/bin/perl
# Smoke test for D88: callCtx_ (list-context propagation) leaked through
# scalar-forcing operators like `eq`/`==` into a nested call, outside of
# print/printf (e.g. `my @a = (ctx() eq "x")` wrongly gave ctx() list
# context, even though `eq` always forces scalar context on its operands
# in real Perl, regardless of the outer expression's own context).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my @log;
sub ctx { push @log, wantarray() ? "list" : "scalar"; return "x"; }
my @a = (ctx() eq "x");
check('smoke_binop_forces_scalar_context', "@log" eq "scalar");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d88_callctx_leak_smoke_done\n";
