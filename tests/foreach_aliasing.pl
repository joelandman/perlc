#!/usr/bin/perl
# In-depth test suite for foreach loop-variable aliasing.
#
# Root cause of the original bug: the general (non-integer-range) foreach
# codegen allocated ONE stable PerlValue* cell for the loop variable before
# the loop started, then on each iteration cloned the current array element
# (perl_array_get, which calls perl_clone) and copied that clone's value
# INTO the stable cell via perl_assign. The loop variable was therefore
# always a private copy — mutating it inside the body only ever touched the
# throwaway stable cell, never the array's own element.
#
# Fixed by borrowing the array's own element pointer each iteration
# (perl_array_get_ref, which returns the live a->elems[idx] with no clone)
# and storing THAT pointer into the loop variable's alloca every iteration,
# instead of copying a value into a fixed cell. Any perl_assign / compound-
# assign / ++ / etc. on the loop var during the body now mutates the array's
# own cell directly — real aliasing, matching Perl semantics.
#
# NOTE: real Perl actually raises "Modification of a read-only value
# attempted" when the loop var aliases an element of a literal list
# (`foreach my $x (1,2,3) { $x *= 2 }`), since literal-list elements are
# constants. perlc does not enforce that read-only protection (silently
# allows the mutation, which is discarded along with the temporary list) —
# a separate, low-priority, intentionally-not-exercised-here gap.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: implicit $_ aliasing (numeric mutation) ──────────────────────
{
    my @arr = (1, 2, 3, 4);
    foreach (@arr) { $_ *= 10; }
    check('underscore_numeric_alias', join(",", @arr) eq "10,20,30,40");
}

# ── Section 2: named loop-variable aliasing ─────────────────────────────────
{
    my @arr = (1, 2, 3);
    foreach my $x (@arr) { $x += 100; }
    check('named_var_alias', join(",", @arr) eq "101,102,103");
}

# ── Section 3: string mutation via alias (.=  compound assign) ─────────────
{
    my @arr = ("a", "b", "c");
    foreach (@arr) { $_ .= "!"; }
    check('string_compound_assign_alias', join(",", @arr) eq "a!,b!,c!");
}

# ── Section 4: nested foreach — inner loop aliases a dereferenced sub-array ─
{
    my @outer = ([1, 2], [3, 4]);
    foreach my $row (@outer) {
        foreach my $v (@$row) { $v += 1; }
    }
    check('nested_foreach_deref_alias',
          join(" ", map { join(",", @$_) } @outer) eq "2,3 4,5");
}

# ── Section 5: next/last still work correctly alongside aliasing ───────────
{
    my @arr = (1, 2, 3, 4, 5);
    foreach my $x (@arr) {
        next if $x == 3;
        last if $x == 5;
        $x *= 100;
    }
    check('next_last_with_alias', join(",", @arr) eq "100,200,3,400,5");
}

# ── Section 6: mutating a hashref element reached via the loop var ─────────
{
    my @arr = ({ v => 1 }, { v => 2 });
    foreach my $h (@arr) { $h->{v} *= 10; }
    check('hashref_element_mutation', join(",", map { $_->{v} } @arr) eq "10,20");
}

# ── Section 7: reading only — regression check, array must be untouched ────
{
    my @arr = (1, 2, 3);
    my $sum = 0;
    foreach my $x (@arr) { $sum += $x; }
    check('read_only_regression', $sum == 6 && join(",", @arr) eq "1,2,3");
}

# ── Section 8: empty array — zero iterations, no crash ──────────────────────
{
    my @arr = ();
    my $count = 0;
    foreach my $x (@arr) { $count++; }
    check('empty_array_no_iterations', $count == 0);
}

# ── Section 9: single-element array ─────────────────────────────────────────
{
    my @arr = (42);
    foreach my $x (@arr) { $x++; }
    check('single_element_alias', $arr[0] == 43);
}

# ── Section 10: closures created per-iteration still capture independent ───
# ── values (aliasing the loop var must not break existing per-iteration ────
# ── closure-capture semantics, which rely on capture-time cloning) ─────────
{
    my @arr = (1, 2, 3);
    my @subs;
    foreach my $x (@arr) {
        push @subs, sub { $x * 1000 };
    }
    check('closure_per_iteration_isolation',
          join(",", map { $_->() } @subs) eq "1000,2000,3000");
}

# ── Section 11: aliasing survives array growth via push from outside scope ─
# ── (iterate a fixed snapshot length; growing after the loop must not      ─
# ── retroactively affect already-completed iterations)                     ─
{
    my @arr = (1, 2, 3);
    foreach my $x (@arr) { $x += 1; }
    push @arr, 99;
    check('post_loop_growth_unaffected', join(",", @arr) eq "2,3,4,99");
}

# ── Section 12: two separate foreach loops over the same array in sequence ─
{
    my @arr = (1, 2, 3);
    foreach my $x (@arr) { $x *= 2; }
    foreach my $x (@arr) { $x += 1; }
    check('sequential_loops_compound', join(",", @arr) eq "3,5,7");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "foreach_aliasing_tests_done\n";
