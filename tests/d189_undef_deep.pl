#!/usr/bin/perl
# Deep regression guard (perlc D189) for the `undef EXPR` lvalue-clearing
# behavior, exercising the shapes a real program actually hits — the
# Text::ParseWords::parse_line accumulator reset (`undef $acc` between
# fields), undef-then-reassign, undef inside subs and bare blocks, and an
# allocator-stress repeat. All output must match real perl byte-for-byte.
use strict;
use warnings;

sub check { my ($name, $ok) = @_; print $name, "=", ($ok ? "ok" : "FAIL"), "\n"; }

# The exact Text::ParseWords repro shape: an accumulator reset with `undef`
# between builds. Without this behavior `undef $acc;` is a no-op and the next
# build appends to the running prefix, so accumulate(5) returns "xxxxx"
# instead of "x".
sub accumulate {
    my $n = shift;
    my $acc;
    for (1 .. $n) {
        undef $acc;
        $acc .= "x";
    }
    return $acc;
}
check('accumulate5', accumulate(5) eq "x");
check('accumulate1', accumulate(1) eq "x");
check('accumulate0', !defined(accumulate(0)));

# undef then reassign via .=
sub reassign_via_concat {
    my $acc = "x";
    undef $acc;
    $acc .= "y";
    return $acc;
}
check('undef_then_concat', reassign_via_concat() eq "y");

# undef empties the array (0 elements) — not the "@a = (undef)" 1-element case
sub empty_array {
    my @a = (1, 2, 3);
    undef @a;
    return scalar(@a);
}
check('empty_array_len', empty_array() == 0);

# undef empties the hash (0 keys)
sub empty_hash {
    my %h = (a => 1, b => 2);
    undef %h;
    return scalar(keys %h);
}
check('empty_hash_keys', empty_hash() == 0);

# undef inside a bare block at top level
my $t = "old";
{
    undef $t;
}
check('top_level_undef', defined($t) ? 1 : 0);

# undef a string-holding lexical — old payload freed, scalar is undef
sub string_reset {
    my $s = "a very long string " x 100;
    undef $s;
    return defined($s) ? 1 : 0;
}
check('string_reset', string_reset() == 0);

# repeated iterations — allocator corruption surfaces after repetition
my $sum = 0;
for my $i (1 .. 300) {
    my $acc;
    undef $acc;
    $acc = $i;
    $sum += $acc;
}
check('three_hundred_iters', $sum == 45150);

print "d189_undef_done\n";
