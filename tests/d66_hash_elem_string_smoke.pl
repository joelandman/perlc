#!/usr/bin/perl
# Smoke test for D66: a block-scoped `my $x = $hash{key};` silently
# coerced a string hash value to 0 (canEmitF64's NK::HashElem fast path
# assumed any in-scope hash element was numeric, then unconditionally
# called perl_to_float on whatever was actually stored there).
# Fast, narrow coverage — see d66_hash_elem_string.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original repro: block-scoped hash with a string value.
{
    my %h = (key => "strval");
    my $x = $h{key};
    check('smoke_block_scope_string_value', $x eq "strval");
}

# Numeric values must still work (the fast path's original intent).
{
    my %h = (key => 42);
    my $x = $h{key};
    check('smoke_block_scope_numeric_value', $x == 42);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d66_hash_elem_string_smoke_done\n";
