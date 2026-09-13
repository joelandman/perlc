#!/usr/bin/perl
# Deep test for D133: exercises the double-free trigger through multiple
# shapes — inside subs and bare blocks, via both the int-var and float-var
# Assign fallbacks, through call-argument and condition positions, and with
# an allocator-stress repeat. All output must match real perl byte-for-byte;
# the pre-fix binary segfaulted (or aborted on a heap check) on several of
# these. `no warnings 'numeric'` suppresses real perl's coercion warnings
# (perlc has no warning system — and the nested pragma also exercises D125).
use strict;
use warnings;
no warnings 'numeric';

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
}

# int-var Assign fallback: RHS is an owned boxed temp (string coercion)
sub coerce_int {
    my $acc = 0;
    {
        $acc = "x" + 0;
    }
    return $acc;
}
check('bare_block_coerce', coerce_int() == 0);

# float-var Assign fallback: RHS coerces to a fractional NV
sub coerce_float {
    my $acc = 0.5;
    {
        $acc = "3.75" + 0;
    }
    return $acc;
}
check('bare_block_float', coerce_float() == 3.75);

# nested bare blocks, two coercion assigns in sequence
sub nested_coerce {
    my $a = 0;
    my $b = 0;
    {
        {
            $a = "2" + 0;
        }
        $b = "z" + 1;
    }
    return $a + $b;
}
check('nested_two_coerce', nested_coerce() == 3);

# direct statement form (no sub): the crash also fired at top level
my $top = 0;
{
    $top = "q" + 0;
}
check('top_level_coerce', $top == 0);

# RHS through a sub call result assigned inside a bare block.
# NOTE: initialized to 0.5 (not 0) deliberately — an int-initialized `my`
# in a sub gets int-promoted and would silently truncate a later NV
# assignment (that is a separate, pre-existing bug — D135 — logged in
# TESTS.md, unrelated to D133's memory bug).
sub give_str { return "5.5"; }
sub call_assign {
    my $acc = 0.5;
    {
        $acc = give_str() + 0;
    }
    return $acc;
}
check('call_result_coerce', call_assign() == 5.5);

# many iterations: allocator corruption tends to surface after repetition
my $sum = 0;
for my $i (1 .. 200) {
    my $t = 0;
    {
        $t = "1" + 0;
    }
    $sum += $t;
}
check('two_hundred_iters', $sum == 200);

# the exact original repro shape: == on the call result as a call argument
sub checker { my ($name, $ok) = @_; return $ok ? "$name ok" : "$name FAIL"; }
sub f { my $acc = 0; { $acc = "x" + 0; } return $acc; }
check('repro_shape', checker("f", f() == 0) eq "f ok");

print "d133_assign_double_free_done\n";