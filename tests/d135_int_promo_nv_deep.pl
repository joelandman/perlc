#!/usr/bin/perl
# Deep test for D135: exercises the int-promotion/truncation bug across
# every found trigger shape — literal float RHS, string-coercion RHS
# ("7.25"+0), sub-call RHS returning an NV, plain string RHS ("7.25" —
# coerced on read), chained self-assignment, / division, float twin
# (float-promoted var receiving an int must hold the int), nested bare
# blocks, and pure-int hot loops that must stay exact (and fast).
# All output byte-for-byte vs real perl.
use strict;
use warnings;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
}

# literal NV RHS after int initializer
sub literal_nv {
    my $x = 0;
    $x = 5.5;
    return $x;
}
check('literal_nv', literal_nv() == 5.5);

# string-coercion RHS ("7.25" + 0)
sub coerce_nv {
    my $x = 0;
    $x = "7.25" + 0;
    return $x;
}
check('coerce_nv', coerce_nv() == 7.25);

# sub-call RHS returning an NV
sub gives_nv { return 2.75; }
sub call_nv {
    my $x = 0;
    $x = gives_nv();
    return $x;
}
check('call_nv', call_nv() == 2.75);

# plain string RHS (coerced through perl_to_int before the fix → 7)
sub string_assign {
    my $x = 0;
    $x = "7.25";
    return $x;
}
check('string_assign', string_assign() == 7.25);

# chained self-assignment carrying a fraction
sub chained {
    my $t = 0;
    $t = $t + 0.5;
    $t = $t + $t;
    return $t;
}
check('chained', chained() == 1.0);

# division RHS (always NV-shaped in real Perl: 1/2 == 0.5)
sub div_shape {
    my $q = 0;
    $q = 1 / 2;
    return $q;
}
check('div_shape', div_shape() == 0.5);

# /= compound
sub div_compound {
    my $q = 3;
    $q /= 2;
    return $q;
}
check('div_compound', div_compound() == 1.5);

# float-promoted twin: my $v = 0.5; $v = 3; must hold 3 (prints "3")
sub float_then_int {
    my $v = 0.5;
    $v = 3;
    return $v;
}
check('float_then_int', float_then_int() == 3);

# nested bare blocks
sub nested_blocks {
    my $a = 0;
    {
        {
            $a = 1.25;
        }
    }
    return $a;
}
check('nested_blocks', nested_blocks() == 1.25);

# assignment inside an if body
sub cond_assign {
    my $x = 0;
    if (1) { $x = 0.75; }
    return $x;
}
check('cond_assign', cond_assign() == 0.75);

# formatting parity: perl prints 5.5 as "5.5", 3.0 as "3"
{
    my $x = 0;
    $x = 5.5;
    print "fmt_nv=$x\n";
    my $v = 0.5;
    $v = 3;
    print "fmt_int_back=$v\n";
}

# pure-int hot loop must stay exact (i64 fast path intact)
sub int_counter {
    my $s = 0;
    for my $i (1 .. 1000000) { $s += $i; }
    return $s;
}
check('int_counter_1e6', int_counter() == 500000500000);

# int loop in a sub that also has an unrelated float var
sub mixed_int_float {
    my $n = 0;
    my $f = 0.0;
    for my $i (1 .. 100) {
        $n += $i;
        $f = $i * 0.5;
    }
    return ($n == 5050 && $f == 50.0) ? 1 : 0;
}
check('mixed_int_float', mixed_int_float() == 1);

# int var copied between variables (conservative: may fall to boxed path,
# but values must be exact either way)
sub int_copies {
    my $a = 3;
    my $b = 0;
    $b = $a;
    $b += 4;
    my $c = 0;
    $c = $b;
    return $c;
}
check('int_copies', int_copies() == 6);

# the compiler must not break ordinary string/numeric behavior
my $half = 10 / 4;
check('plain_div', $half == 2.5);
print "d135_int_promo_nv_done\n";