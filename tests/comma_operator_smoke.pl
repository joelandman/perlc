#!/usr/bin/perl
# Smoke test for the C-style comma operator in `for (init; cond; step)`.
# Fast, narrow coverage — see comma_operator.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# Two counters, one up one down, both declared in init and stepped together.
{
    my @pairs;
    for (my $i = 0, my $j = 10; $i < 3; $i++, $j--) {
        push @pairs, "$i:$j";
    }
    check('smoke_dual_counter', join(",", @pairs) eq "0:10,1:9,2:8");
}

# Comma-separated step with a non-increment operator.
{
    my @vals;
    for (my $i = 1; $i <= 8; $i *= 2) {
        push @vals, $i;
    }
    check('smoke_single_step_regression', join(",", @vals) eq "1,2,4,8");
}

# Three items in both init and step.
{
    my $total = 0;
    for (my $i = 0, my $j = 0, my $k = 0; $i < 4; $i++, $j += 2, $k += 3) {
        $total += $i + $j + $k;
    }
    check('smoke_triple_init_step', $total == 36);
}

# Side-effecting function call as a step item.
# (raw `push` is a statement-level builtin in this grammar, not usable in
# expression position — route through a helper sub instead)
{
    my @log;
    my $record = sub { push @log, $_[0]; };
    for (my $i = 0; $i < 3; $i++, $record->($i)) {
        # body intentionally empty
    }
    check('smoke_side_effect_step', join(",", @log) eq "1,2,3");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "comma_operator_smoke_done\n";
