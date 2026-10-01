#!/usr/bin/perl
# Deep test: bare `;` null statements in every place real Perl allows
# them — after if/unless/while/for/foreach blocks, doubled after a
# statement that already ends in `;`, runs of `;;;`, and a bare
# top-level `;` by itself — and confirm the surrounding control flow
# still produces correct runtime output (not just that it parses).
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $got, $want) = @_;
    my $ok = (defined($got) && defined($want)) ? ($got eq $want)
           : (!defined($got) && !defined($want));
    print $name, "=", ($ok ? "ok" : "FAIL($got)"), "\n";
    push @failures, $name unless $ok;
}

;

sub tag {
    my $x = shift;
    if ($x =~ /a/) { return "A" };
    unless ($x =~ /b/) { return "notB"; };
    while (0) { };
    return "none";
}
check('if_block_semi', tag("a"), "A");
check('unless_block_semi', tag("x"), "notB");
check('miss_all', tag("b"), "none");

{
    my $n = 0;
    for (my $i = 0; $i < 3; $i++) { $n += $i; };
    check('for_block_semi', $n, 3);
}

{
    my $n = 0;
    foreach my $v (1, 2, 3) { $n += $v; };
    check('foreach_block_semi', $n, 6);
}

{
    my $n = 5;;
    check('double_semi', $n, 5);
}

{
    my $n = 7;;;;
    check('triple_semi', $n, 7);
}

{
    my $n = 0;
    if (1) { $n = 1; } else { $n = 2; };
    check('if_else_semi', $n, 1);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "empty_stmt_done\n";
