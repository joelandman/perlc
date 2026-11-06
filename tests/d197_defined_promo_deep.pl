use strict;
use warnings;

# D197 deep: the definedness-observer refusal across promotion classes and
# the promotion-preserved counterpart (params that never observe defined()
# keep the fast unboxed bind), plus the Stage-37-proven path and closures.

sub only_arith { my ($a, $b) = @_; return ($a + $b) * 2; }
print "arith: ", only_arith(10, 5), "|", only_arith(1, 2), "\n";

sub mixed_arith { my ($a) = @_; return $a * $a - 1; }
print "mixed: ", mixed_arith(6), "\n";

# proven path: identical single-shape call sites, defined observer
sub prov_def { my ($p) = @_; return defined($p) ? "P" : "N"; }
print "prov: ", prov_def(7), "|", prov_def(8), "|", prov_def(), "\n";

# the observer may appear in a nested if/else chain, loops, and string forms
sub chain { my ($q) = @_;
    if (defined($q)) {
        return "c-D$q" if $q > 100;
        return "c-d$q";
    }
    return "c-U";
}
print "chain: ", chain(), "|", chain(5), "|", chain(500), "\n";

sub loopcheck { my ($s) = @_; my $n = 0;
    for my $i (0..2) { $n++ if defined($s); }
    return "n=$n";
}
print "loop: ", loopcheck(), "|", loopcheck(1), "\n";

# a closure observing defined() on the outer param must also refuse
sub closured { my ($c) = @_;
    my $f = sub { return defined($c) ? "C" : "c" ; };
    return $f->();
}
print "closure: ", closured(), "|", closured(2), "\n";

# ternary conds deeper inside expressions
sub deepcond { my ($d) = @_; return ((defined($d) ? 1 : 0) + 3) * 2; }
print "deep: ", deepcond(), "|", deepcond(9), "\n";

# arithmetic-only params in a defined()-free sub promote as before
sub shiftleft { my ($v) = @_; return $v * 4 + $v; }
print "shift: ", shiftleft(3), "\n";
