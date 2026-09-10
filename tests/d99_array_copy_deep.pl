#!/usr/bin/perl
# Deep: D99 — `my @new = @existing;` (and @$ref, sub-return, our, range
# variants) must copy element-by-element, not alias the source array's
# storage. Root cause was src/codegen.cpp's `case NK::My` (isArr branch):
# emitArrayPtr() returns a *borrowed* pointer for a plain @var or @$ref/
# ->@* deref (see the identical ownsTmpArr distinction in Foreach codegen)
# and that pointer was declared directly as the new variable's backing
# array instead of being copied into a fresh one.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    my @a = (1, 2, 3);
    my @b = @a;
    $b[0] = 99;
    check('plain_array_copy', $a[0] == 1 && $b[0] == 99);
}
{
    my @c = (1, 2, 3);
    push @c, 4;
    my @d = @c;
    $d[1] = 88;
    push @d, 5;
    check('copy_after_push', "@c" eq "1 2 3 4" && "@d" eq "1 88 3 4 5");
}
{
    my $ref = [1, 2, 3];
    my @e = @$ref;
    $e[0] = 77;
    check('deref_array_copy', $ref->[0] == 1 && $e[0] == 77);
}
{
    my $ref = [1, 2, 3];
    my @f = $ref->@*;
    $f[0] = 66;
    check('postfix_deref_copy', $ref->[0] == 1 && $f[0] == 66);
}
{
    sub mk { my @z = (1, 2, 3); return @z; }
    my @g = mk();
    $g[0] = 42;
    my @h = mk();
    check('sub_return_independent', $h[0] == 1 && $g[0] == 42);
}
{
    our @ga = (1, 2, 3);
    our @gb = @ga;
    $gb[0] = 55;
    check('our_array_copy', $ga[0] == 1 && $gb[0] == 55);
}
{
    my @r = (1 .. 5);
    my @r2 = @r;
    $r2[0] = -1;
    check('range_array_copy', $r[0] == 1 && $r2[0] == -1);
}
{
    # fallback path (no array-shaped RHS) must still work after the fix
    my $ref = [1, 2, 3];
    my @i = $ref;
    check('scalar_rhs_single_elem', scalar(@i) == 1 && $i[0] == $ref);
}
{
    my @j = (1, 2, 3);
    check('literal_list_unaffected', scalar(@j) == 3 && "@j" eq "1 2 3");
}
{
    # shallow-copy semantics preserved: copying an array of refs must copy
    # the ref *slots*, not deep-clone what they point to.
    my $inner = [1, 2];
    my @k = ($inner, [3, 4]);
    my @l = @k;
    check('shallow_copy_same_ref', $k[0] == $l[0]);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d99_array_copy_done\n";
