#!/usr/bin/perl
# Deep test for D125: use/no pragma statements parse and behave like the
# file-top forms from any nested scope — bare blocks at several depths,
# subs, if/while bodies, and a module-`use` inside a sub (with the
# explicit qw() import real Perl's Exporter semantics require).
use strict;
use warnings;
use lib 'tests/lib';

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# pragma in a sub at depth 1: "abc" is coerced to 0 without the warning,
# so the sum is 1 — the pragma suppresses the warning, not the coercion
# (verified byte-for-byte vs real perl).
sub no_numeric {
    no warnings 'numeric';
    return "abc" + 1;
}
check('sub_no_numeric', no_numeric() == 1);

# pragma in a bare block, inside a bare block, inside a sub
sub nested_blocks {
    my $acc = 0;
    {
        use strict;
        use warnings;
        {
            no warnings 'numeric';
            $acc = "x" + 0;
        }
        $acc += 5;
    }
    return $acc;
}
check('nested_blocks', nested_blocks() == 5);

# pragma in an if body and a while body
my $ifval = -1;
if (1) {
    no warnings 'numeric';
    $ifval = "z" + 1;
}
check('if_body_pragma', $ifval == 1);

my $iters = 0;
my $wsum = 0;
while ($iters < 3) {
    use strict;
    use warnings;
    $wsum += $iters;
    $iters++;
}
check('while_body_pragma', $wsum == 3);

# module-use inside a sub: D125Pragma is inlined from this position; an
# explicit qw(triple) import makes triple() callable unqualified
# immediately after (real Perl's compile-time Exporter semantics).
sub import_inside_sub {
    use D125Pragma qw(triple);
    return triple(5);
}
check('module_use_in_sub', import_inside_sub() == 15);

# pragma immediately before a use inside the same sub
sub pragma_then_use {
    no warnings 'numeric';
    use D125Pragma qw(triple);
    return triple(2);
}
check('pragma_then_use', pragma_then_use() == 6);

# file-top uses unchanged: the module's other @EXPORT_OK name stays
# reachable qualified (real Exporter semantics — an explicit import list
# replaces, not adds to, the default; qualified access always works)
check('export_ok_qualified', D125Pragma::triple(3) == 9);

# no-op pragma forms in nested scopes
sub no_op_forms {
    no strict;
    no warnings;
    use integer;
    return "no-op-ok";
}
check('no_op_forms', no_op_forms() eq "no-op-ok");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d125_pragma_nested_done\n";