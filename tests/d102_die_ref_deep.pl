#!/usr/bin/perl
# Deep test for D102: die REF / die $blessed_obj must propagate the
# reference itself into $@ unmodified — no stringification, no
# " at FILE line N." location suffix (confirmed directly against real
# Perl: that suffix is never appended to a reference, even at top
# level, not just inside eval). Real code routinely branches on
# ref($@) to distinguish structured/typed exceptions from plain string
# errors.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    eval { die { code => 42, msg => "bad" }; };
    check('hashref_type', ref($@) eq "HASH");
    check('hashref_field', $@->{code} == 42 && $@->{msg} eq "bad");
}
{
    eval { die ["a", "b", "c"]; };
    check('arrayref_type', ref($@) eq "ARRAY");
    check('arrayref_field', $@->[1] eq "b");
}
{
    eval { die \"scalar error"; };
    check('scalarref_type', ref($@) eq "SCALAR");
    my $err = $@;
    check('scalarref_value', $$err eq "scalar error");
}
{
    package MyErr;
    sub new { my ($class, $msg) = @_; return bless { msg => $msg }, $class; }
    package main;
    eval { die MyErr->new("oops"); };
    check('blessed_type', ref($@) eq "MyErr");
    check('blessed_field', $@->{msg} eq "oops");
}
{
    # a plain string die still gets the normal location suffix
    eval { die "plain error\n"; };
    check('plain_string_unaffected', $@ eq "plain error\n");
    eval { die "no newline"; };
    check('plain_string_location_suffix', $@ =~ /^no newline at .+ line \d+\.\n?$/);
}
{
    # die with no args still works (defaults to "Died")
    eval { die; };
    check('die_no_args', $@ =~ /^Died/);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d102_die_ref_done\n";
