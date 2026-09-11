#!/usr/bin/perl
# Deep test for D119: scalar-context keys/values on a deref'd hashref
# (%$href or %{$href}) — src/codegen.cpp's case NK::KeysFunc/ValuesFunc
# in emitExpr only ever resolved a *named* hash variable (n.name); the
# deref form (n.left set, n.name empty) fell through to lookupHash("")
# and silently returned 0. emitArrayPtr's identical n.left dispatch
# (used for list context) already had this right, so list-context
# `keys %$href` was never affected — only scalar context.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my %h = (a => 1, b => 2, c => 3);
my $href = \%h;

check('keys_deref_dollar', scalar(keys %$href) == 3);
check('keys_deref_braces', scalar(keys %{$href}) == 3);
check('values_deref_dollar', scalar(values %$href) == 3);
check('values_deref_braces', scalar(values %{$href}) == 3);

# boolean context (implicit scalar) — common idiom: `if (keys %$href)`
if (keys %$href) {
    check('keys_deref_boolean_true', 1);
} else {
    check('keys_deref_boolean_true', 0);
}
my %empty;
my $eref = \%empty;
check('keys_deref_boolean_false', !(keys %$eref));

# regression: named hash (always worked) and list-context deref (always
# worked) must be unaffected by this fix
check('keys_named_hash_unaffected', scalar(keys %h) == 3);
my @klist = keys %$href;
check('keys_deref_list_context_unaffected', scalar(@klist) == 3);

# nested deref through a sub return value
sub get_href { return \%h; }
check('keys_deref_from_sub_call', scalar(keys %{ get_href() }) == 3);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d119_keys_deref_scalar_done\n";
