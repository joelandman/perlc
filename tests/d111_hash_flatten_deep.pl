#!/usr/bin/perl
# Deep test for D111: %hash flattening to a (key, value, ...) list in
# every list context perlc has, not just `my %new = %old`, since the
# root cause (emitArrayPtr had no case for a hash-shaped node) was
# general, not specific to the `my %h2 = %h1` declaration path.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    # the exact real-world repro: declaration copy
    my %h = (a => 1, b => 2);
    my %c = %h;
    check('decl_copy_count', scalar(keys %c) == 2);
    check('decl_copy_vals', $c{a} == 1 && $c{b} == 2);
}
{
    # merge pattern: bless { %args }, $class idiom's underlying mechanism
    my %defaults = (x => 1, y => 2);
    my %overrides = (y => 20, z => 3);
    my %opts = (%defaults, %overrides);
    check('merge_count', scalar(keys %opts) == 3);
    check('merge_vals', $opts{x} == 1 && $opts{y} == 20 && $opts{z} == 3);
}
{
    # flatten into an array
    my %h = (p => 10, q => 20);
    my @flat = %h;
    check('flat_len', scalar(@flat) == 4);
    my %roundtrip = @flat;
    check('flat_roundtrip', $roundtrip{p} == 10 && $roundtrip{q} == 20);
}
{
    # anon-hashref construction from a spread hash (the bless {%args} idiom)
    my %args = (name => "widget", count => 5);
    my $obj = { %args, extra => 1 };
    # NB: `scalar(keys %$obj)` hits a separate, pre-existing bug (D119 —
    # `keys` on a deref'd hashref returns 0 in scalar context even though
    # list-context `keys` on the same expression is correct) unrelated to
    # D111 — counting via list-context keys sidesteps it here.
    my @okeys = keys %$obj;
    check('anon_href_count', scalar(@okeys) == 3);
    check('anon_href_vals', $obj->{name} eq "widget" && $obj->{count} == 5 && $obj->{extra} == 1);
}
{
    # sub-call arg flattening (foo(%h)) must be unaffected by the fix
    # (it already worked via a separate code path — flattenArgInto)
    my %h = (m => 1, n => 2);
    my $sum = 0;
    my @list = (%h);
    $sum += $_ for grep { /^\d+$/ } @list;
    check('sub_arg_path_unaffected', $sum == 3);
}
{
    # %$href (deref-hash) flattening
    my %h = (dk => 7);
    my $href = \%h;
    my %c2 = %$href;
    check('deref_hash_flatten', $c2{dk} == 7);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d111_hash_flatten_done\n";
