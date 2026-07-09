#!/usr/bin/perl
# In-depth test suite for the C-style comma operator in `for (init; cond; step)`.
#
# Covers: multi-item init/step, mixing `my`/`our`/pre-declared vars, non-increment
# step operators, side-effecting step expressions, array/hash element targets,
# nested independently-scoped comma clauses, last/next interaction, and
# regression checks for the pre-existing single-item and empty-clause forms.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: basic dual counter, ascending + descending ──────────────────
{
    my @pairs;
    for (my $i = 0, my $j = 10; $i < 3; $i++, $j--) {
        push @pairs, "$i:$j";
    }
    check('dual_counter', join(",", @pairs) eq "0:10,1:9,2:8");
}

# ── Section 2: triple counter with different step magnitudes ───────────────
{
    my $total = 0;
    for (my $i = 0, my $j = 0, my $k = 0; $i < 4; $i++, $j += 2, $k += 3) {
        $total += $i + $j + $k;
    }
    check('triple_counter', $total == 36);
}

# ── Section 3: four items in both init and step ─────────────────────────────
{
    my $total = 0;
    for (my $a = 0, my $b = 0, my $c = 0, my $d = 0;
         $a < 3;
         $a++, $b += 1, $c += 2, $d += 3) {
        $total += $a + $b + $c + $d;
    }
    check('quad_init_step', $total == 21);
}

# ── Section 4: regression — single-item init/step (pre-existing form) ──────
{
    my @vals;
    for (my $i = 1; $i <= 8; $i *= 2) {
        push @vals, $i;
    }
    check('single_item_regression', join(",", @vals) eq "1,2,4,8");
}

# ── Section 5: regression — empty init/step clauses ─────────────────────────
{
    my $n = 0;
    for (; $n < 3; ) { $n++; }
    check('empty_clauses_regression', $n == 3);
}

# ── Section 6: comma in init only, single-item step ─────────────────────────
{
    my @a;
    for (my $x = 0, my $y = 1; $x < 3; $x++) {
        push @a, "$x:$y";
    }
    check('comma_init_only', join(",", @a) eq "0:1,1:1,2:1");
}

# ── Section 7: comma in step only, single-item init ─────────────────────────
# (raw `push` is a statement-level builtin in this grammar, not usable in
# expression position — route through a helper sub instead)
{
    my @log;
    my $record = sub { push @log, $_[0] * 10; };
    for (my $i = 0; $i < 3; $i++, $record->($i)) {
    }
    check('comma_step_only', join(",", @log) eq "10,20,30");
}

# ── Section 8: mixing `my` and a pre-declared variable in the init list ─────
{
    my $k;
    my @out;
    for (my $i = 0, $k = 5; $i < 3; $i++, $k++) {
        push @out, "$i:$k";
    }
    check('mixed_my_predeclared', join(",", @out) eq "0:5,1:6,2:7");
}

# ── Section 9: `our` variables chained with comma in init ──────────────────
{
    my @pairs;
    for (our $gi = 0, our $gj = 100; $gi < 3; $gi++, $gj -= 10) {
        push @pairs, "$gi:$gj";
    }
    check('our_comma_init', join(",", @pairs) eq "0:100,1:90,2:80");
}

# ── Section 10: mixed compound-assignment operators in the step list ───────
{
    my @snapshots;
    for (my $add = 1, my $mul = 1; $add < 5; $add += 1, $mul *= 2) {
        push @snapshots, "$add:$mul";
    }
    check('mixed_step_operators', join(",", @snapshots) eq "1:1,2:2,3:4,4:8");
}

# ── Section 11: side-effecting function-call step item ──────────────────────
{
    my @log;
    my $record = sub { push @log, $_[0]; };
    for (my $i = 0; $i < 3; $i++, $record->($i)) {
        # body intentionally empty — all work happens in step
    }
    check('side_effect_step_call', join(",", @log) eq "1,2,3");
}

# ── Section 12: array-element target as a step item ─────────────────────────
{
    my @arr = (0, 0, 0);
    for (my $i = 0; $i < 3; $arr[$i]++, $i++) {
    }
    check('array_elem_step_target', join(",", @arr) eq "1,1,1");
}

# ── Section 13: hash-element target as a step item ───────────────────────────
{
    my %h;
    for (my $i = 0; $i < 3; $h{"k$i"} = $i * $i, $i++) {
    }
    check('hash_elem_step_target',
          $h{k0} == 0 && $h{k1} == 1 && $h{k2} == 4);
}

# ── Section 14: nested for loops each with independently-scoped comma vars ──
{
    my @nested;
    for (my $i = 0, my $j = 100; $i < 2; $i++, $j += 100) {
        for (my $i2 = 0, my $j2 = 1; $i2 < 2; $i2++, $j2 *= 2) {
            push @nested, "$i/$j/$i2/$j2";
        }
    }
    check('nested_independent_scopes',
          join(" ", @nested) eq "0/100/0/1 0/100/1/2 1/200/0/1 1/200/1/2");
}

# ── Section 15: `next` — step must still run for the skipped iteration ──────
{
    my @log;
    for (my $i = 0, my $j = 0; $i < 5; $i++, $j += 10) {
        next if $i == 2;
        push @log, "$i:$j";
    }
    check('next_runs_step', join(",", @log) eq "0:0,1:10,3:30,4:40");
}

# ── Section 16: `last` — step must NOT run for the terminating iteration ────
{
    my @log;
    for (my $i = 0, my $j = 100; $i < 10; $i++, $j -= 5) {
        last if $i == 3;
        push @log, "$i:$j";
    }
    check('last_stops_before_step', join(",", @log) eq "0:100,1:95,2:90");
}

# ── Section 17: comma operator inside a labeled loop with labeled next/last ─
{
    my @log;
    OUTER: for (my $i = 0, my $j = 0; $i < 3; $i++, $j += 1) {
        for (my $k = 0; $k < 3; $k++) {
            next OUTER if $k == 1;
            push @log, "$i.$j.$k";
        }
    }
    check('labeled_next_with_comma', join(",", @log) eq "0.0.0,1.1.0,2.2.0");
}

# ── Section 18: moderate iteration count — no off-by-one, no infinite loop ──
{
    my $sum = 0;
    my $count = 0;
    for (my $i = 0, my $j = 1000; $i < 500; $i++, $j--) {
        $sum += $i + $j;
        $count++;
    }
    check('moderate_iteration_count', $count == 500 && $sum == 500 * 1000);
}

# ── Section 19: while loop untouched by the for-comma change (sanity) ───────
{
    my $i = 0;
    my @vals;
    while ($i < 3) {
        push @vals, $i;
        $i++;
    }
    check('while_loop_unaffected', join(",", @vals) eq "0,1,2");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "comma_operator_tests_done\n";
