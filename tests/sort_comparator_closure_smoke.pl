#!/usr/bin/perl
# Smoke test for D61: sort's { BLOCK } comparator — compiled as a genuinely
# separate LLVM function (needed since it's passed as a real C function
# pointer to the underlying qsort()-based sort routine) — had *zero*
# closure-capture support for any outer block-scoped (non-file-scope)
# variable: reading/writing one from inside the comparator silently had
# no effect at all. `map`/`grep`/`reduce` blocks (all compiled inline,
# sharing the enclosing scope stack directly) never had this issue — only
# sort's comparator, because of how it alone must be compiled as a
# separate function.
# Fast, narrow coverage — see sort_comparator_closure.pl for the in-depth
# suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: push()-ing into a block-scoped (not file-scope) outer
# array from inside a sort{} comparator silently had no effect at all.
{
    my @observed;
    my @data = (5, 3, 8, 1, 4);
    my @sorted = sort { push @observed, $a; $a <=> $b } @data;
    check('smoke_comparator_captures_block_array', scalar(@observed) > 0);
    check('smoke_sort_order_unaffected', join(",", @sorted) eq "1,3,4,5,8");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sort_comparator_closure_smoke_done\n";
