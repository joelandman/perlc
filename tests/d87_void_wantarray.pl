#!/usr/bin/perl
# Deep: D87 — three-state wantarray (void/scalar/list)
sub w {
    my $c = wantarray;
    if (!defined $c) { print "void\n"; }
    elsif ($c) { print "list\n"; }
    else { print "scalar\n"; }
    return (1, 2, 3);
}
print "=== top ===\n";
w();
my $x = w();
my @a = w();
print "a=", join(",", @a), "\n";

print "=== nested ===\n";
sub outer {
    w();
    my $y = w();
    my @b = w();
}
outer();

print "=== inherit last/return ===\n";
sub chain { w() }
my @d = chain();
my $e = chain();
sub retw { return w(); }
my @f = retw();
my $g = retw();

print "d87_void_wantarray_done\n";
