#!/usr/bin/perl
# Smoke test for D12: `wantarray` context was not propagated into
# `print`/`printf` call arguments — a sub called as a `print`/`printf`
# argument saw scalar context instead of the list context real Perl always
# uses for these arguments.
#
# ctx() records what wantarray() actually saw as a side effect (pushed
# onto @seen) rather than trying to capture print's own stdout output.
# Fast, narrow coverage — see d12_print_wantarray.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my @seen;
sub ctx { push @seen, (wantarray ? "list" : "scalar"); return "x"; }

print ctx(), "\n";
check('smoke_print_list_ctx', $seen[-1] eq "list");

printf("%s\n", ctx());
check('smoke_printf_list_ctx', $seen[-1] eq "list");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d12_print_wantarray_smoke_done\n";
