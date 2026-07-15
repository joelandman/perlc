#!/usr/bin/perl
# Smoke test for D75: multiple-inheritance method resolution picked the
# wrong parent — a class with 2+ direct @ISA parents could only ever see
# the LAST-registered one, since perl_set_isa (runtime.c) overwrote rather
# than accumulated each @ISA element's registration.
# Fast, narrow coverage — see d75_multi_inherit.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

package D75SmokeA;
sub hello { return "A::hello"; }

package D75SmokeB;
our @ISA = ('D75SmokeA');
sub hello { my $self = shift; return "B::hello->" . $self->SUPER::hello(); }

package D75SmokeC;
our @ISA = ('D75SmokeA');
sub hello { return "C::hello"; }

package D75SmokeD;
our @ISA = ('D75SmokeB', 'D75SmokeC');
sub new { return bless {}, shift; }

package main;

my $d = D75SmokeD->new;
check('smoke_dfs_dispatch', $d->hello eq "B::hello->A::hello");
check('smoke_isa_second_parent', $d->isa('D75SmokeC'));

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d75_multi_inherit_smoke_done\n";
