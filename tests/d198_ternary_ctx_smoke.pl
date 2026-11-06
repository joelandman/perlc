#!/usr/bin/env perl
# D198: the conditional operator propagates ITS OWN context to whichever
# branch runs — independent of what wantarray() reports about the enclosing
# sub's call context. Smoke: the list-assign / print / call-arg / join shapes.
use strict; use warnings;

my @a = 1 ? (1,2) : (3);
print "A=@a\n";
my @b = 0 ? (1,2) : (3,4);
print "B=@b\n";
my ($c,$d) = 1 ? (1,2) : (3,4);
print "C=$c D=$d\n";
print "P:", 1 ? (1,2) : (3), "|\n";
my %h = 1 ? (x=>1, y=>2) : (z=>3);
print "H=", join(",", map {"$_:$h{$_}"} sort keys %h), "\n";

sub foo { return (7,8); }
my @e = 1 ? foo() : (3);
print "E=@e\n";

print "R12=", join(",", 1 ? (1,2) : (3)), "\n";

# scalar-position ternary inside a list-called sub: the branch must collapse
# to its scalar value (local scalar assign), NOT follow the caller's list ctx
sub gg { my $v = 1 ? (1,2) : (3); return $v; }
my @o = gg(); print "O=@o\n";
my $p = gg(); print "P2=$p\n";

# list-position ternary inside a scalar-called sub: the branch must flatten
sub cq { my ($x) = @_; my @r = $x ? (1,2) : (3); return wantarray ? "want-list" : join("/", @r); }
print "Q=", cq(1), "\n";
print "Q2=", join("|", cq(1), "end"), "\n";

# return COND ternary keeps caller-context semantics (wantarray ctx)
sub idt { my ($x) = @_; return $x ? (1,2) : (3); }
my @r1 = idt(1); print "R1=@r1\n";
my @r2 = idt(0); print "R2=@r2\n";
my $r3  = idt(1); print "R3=$r3\n";
