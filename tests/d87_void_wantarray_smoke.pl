#!/usr/bin/perl
# Smoke: D87 — wantarray() is undef in void context
sub w {
    my $c = wantarray;
    if (!defined $c) { print "void\n"; }
    elsif ($c) { print "list\n"; }
    else { print "scalar\n"; }
}
w();
my $x = w();
my @a = w();
print "d87_void_wantarray_smoke_done\n";
