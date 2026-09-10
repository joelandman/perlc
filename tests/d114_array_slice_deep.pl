#!/usr/bin/perl
# Deep test for D114: array-slice subscript dispatch — literal indices,
# a range, an array-variable index list, negative indices, a mix of
# scalar and list-shaped entries in one slice, and the scalar-context /
# arrayref-postfix-deref form (`$r->@[...]`), which has its own separate
# codegen site (case NK::ArraySlice in emitExpr) from the list-context
# one (emitArrayPtr) — both needed the same dispatch fix.
use strict;
use warnings;
no warnings 'syntax'; # real perl warns on the deliberate single-index-slice test below

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my @x = (10, 20, 30, 40, 50);

{
    my @s = @x[1..2];
    check('range_slice', "@s" eq "20 30");
}
{
    my @idx = (0, 2, 4);
    my @s = @x[@idx];
    check('array_var_slice', "@s" eq "10 30 50");
}
{
    # literal comma-separated indices must still work (pre-existing path)
    my @s = @x[0, 2];
    check('literal_list_slice', "@s" eq "10 30");
}
{
    # single literal index (must not be treated as list-shaped)
    my @s = @x[2];
    check('single_literal_index', "@s" eq "30");
}
{
    # negative indices in a range
    my @s = @x[-2..-1];
    check('negative_range_slice', "@s" eq "40 50");
}
{
    # mixed: a range entry alongside a literal entry in one slice
    my @s = @x[0, 2..3];
    check('mixed_slice', "@s" eq "10 30 40");
}
{
    # @{$ref}[LIST] — array-deref slice with a literal list (goes through
    # the same emitArrayPtr ArraySlice dispatch site as the plain-array
    # cases above, just with n.left set instead of n.name)
    my $r = [100, 200, 300, 400];
    my @s = @{ $r }[1, 3];
    check('derefarray_slice_literal', "@s" eq "200 400");
}
{
    # @{$ref}[RANGE] — the actual D114 bug, through the deref-array path
    my $r = [100, 200, 300, 400];
    my @s = @{ $r }[1..2];
    check('derefarray_slice_range', "@s" eq "200 300");
}
{
    # empty index list
    my @s = @x[()];
    check('empty_slice', scalar(@s) == 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d114_array_slice_done\n";
