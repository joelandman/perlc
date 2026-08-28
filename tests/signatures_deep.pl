#!/usr/bin/perl
# Deep: slurpy @rest, nameless ignored via prototypes still work, anon sig.
use v5.36;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub head_tail($h, @rest) { $h . ":" . join(",", @rest) }
check('slurp', head_tail(1, 2, 3, 4) eq "1:2,3,4");

my $f = sub ($a, $b = 1) { $a * $b };
check('anon', $f->(4) == 4);
check('anon2', $f->(4, 3) == 12);

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "signatures_deep_done\n";
