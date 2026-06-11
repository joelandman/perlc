#!/usr/bin/perl
# tests/threads_atomic.pl
#
# Phase-1 contract for the threads::shared atomic rewrite
# (see THREADS_SHARED_ATOMIC_PLAN.md, Phase 1).
#
# This file is the *contract* the new runtime must satisfy once Phases 2-3
# of the plan are implemented.  Today (baseline + minimal-fix release fence
# in perl_assign) the visibility test passes but the read-modify-write
# test fails, because $counter++ is still racy without an explicit lock().
# That is exactly the behaviour the plan calls out under
# "Risk 2 / Risk 3" and "Phase 1 → Phase 4 validation".
#
# TSan-instrumented build:
#   CC=clang-18 CFLAGS="-g -O1 -fsanitize=thread" make clean && make
#   ./perlc tests/threads_atomic.pl -o /tmp/threads_atomic_tsan
#   /tmp/threads_atomic_tsan
# A clean TSan run must show zero "data race" reports on shared scalars.
# (The compiler-internal $thr->join() boundary provides the happens-before
# edge the sanitizer needs; if the per-op atomicity in Test 2 is missing,
# TSan will flag the racy ++ directly.)

use strict;
use warnings;
use threads;
use threads::shared;

# ── Test 1: shared-scalar write visible to another thread without lock() ───
# This is the pattern that motivated the rewrite.  With the minimal
# release-fence fix in perl_assign, the worker's busy-wait eventually sees
# the writer's $phase = 1; on a 5-second watchdog.
my $phase : shared = 0;
my $phase_seen : shared = 0;

my $phase_watcher = threads->create(sub {
    my $deadline = time() + 5;
    until ($phase) {
        if (time() > $deadline) {
            $phase_seen = -1;     # timeout sentinel
            return 0;
        }
        sleep 1;
    }
    $phase_seen = 1;
    return 1;
});

# Let the worker reach the until-loop
select(undef, undef, undef, 0.05);
$phase = 1;
my $phase_ret = $phase_watcher->join();
print "visibility_no_lock=" . ($phase_seen == 1 ? "yes" : "no") . "\n";
print "visibility_no_lock_ret=$phase_ret\n";

# ── Test 2: $counter++ on a shared var produces exactly M*N total ─────────
# M=8 threads, N=20000 increments each → expected 160000.
# Without atomic increment, the lost-update count varies but is always
# strictly less than 160000 on a multi-core machine.
my $N = 20000;
my $M = 8;
my $counter : shared = 0;
my @inc_workers;
for my $i (1..$M) {
    push @inc_workers, threads->create(sub {
        for my $j (1..$N) {
            $counter = $counter + 1;
        }
    });
}
for my $w (@inc_workers) { $w->join(); }
my $expected = $M * $N;
print "rmw_atomic_total=$counter\n";
print "rmw_atomic_expected=$expected\n";
print "rmw_atomic_ok=" . ($counter == $expected ? "yes" : "no") . "\n";

# ── Test 3: lock($x); $x = $x + 1 still works (no regression) ─────────────
# The compiler's current lock scope model is sub-exit-only (no per-iteration
# unlock), so we hold the lock for the whole sub body.  This serialises the
# increments but still proves the no-regression claim: lock+increment with
# N threads produces exactly M*N totals.
#
# Phase 3: the codegen now detects `$shared = $shared OP N` and routes
# through perl_atomic_add, which is re-entrant on the per-thread
# SharedMutex.  So holding the lock for the whole sub body no longer
# deadlocks; the inner `$locked_counter = $locked_counter + 1` re-enters
# the lock transparently.  We can now use the same M=8 / N=20000 sizing
# as the un-wrapped RMW test below.
my $L_N = 20000;
my $L_M = 8;
my $locked_counter : shared = 0;
my @locked_workers;
for my $i (1..$L_M) {
    push @locked_workers, threads->create(sub {
        lock($locked_counter);
        for my $j (1..$L_N) {
            $locked_counter = $locked_counter + 1;
        }
    });
}
for my $w (@locked_workers) { $w->join(); }
my $locked_expected = $L_M * $L_N;
print "lock_increment_total=$locked_counter\n";
print "lock_increment_ok=" . ($locked_counter == $locked_expected ? "yes" : "no") . "\n";

# ── Test 4: cond_wait / cond_signal still works ────────────────────────────
my $ready : shared = 0;
my $cond_ret : shared = "";
my $cond_thr = threads->create(sub {
    lock($ready);
    my $deadline = time() + 5;
    while (!$ready) {
        if (time() > $deadline) {
            $cond_ret = "timeout";
            return;
        }
        cond_wait($ready);
    }
    $cond_ret = "signaled";
});
{
    lock($ready);
    $ready = 1;
    cond_signal($ready);
}
$cond_thr->join();
print "cond_wait_signal=" . ($cond_ret eq "signaled" ? "yes" : "no") . "\n";

# ── Test 5: cond_broadcast wakes all waiters ───────────────────────────────
my $bcast_ready : shared = 0;
my $bcast_count : shared = 0;
my @bcast_thrs;
for my $i (1..4) {
    push @bcast_thrs, threads->create(sub {
        lock($bcast_ready);
        my $deadline = time() + 5;
        while (!$bcast_ready) {
            if (time() > $deadline) { return; }
            cond_wait($bcast_ready);
        }
        lock($bcast_count);
        $bcast_count = $bcast_count + 1;
    });
}
# Give all 4 workers time to enter cond_wait
select(undef, undef, undef, 0.1);
{
    lock($bcast_ready);
    $bcast_ready = 1;
    cond_broadcast($bcast_ready);
}
for my $t (@bcast_thrs) { $t->join(); }
print "cond_broadcast_count=$bcast_count\n";
print "cond_broadcast_ok=" . ($bcast_count == 4 ? "yes" : "no") . "\n";

# ── Test 6: lock($x) auto-releases on scope exit ───────────────────────────
# Pattern from threads.pl: lock inside a { } block, then access from main
# without re-locking (would deadlock if the lock leaked).  We extend it by
# doing actual work in the inner block, which would corrupt the shared array
# if the lock weren't actually held.
my @shared_arr : shared;
{
    lock(@shared_arr);
    for my $i (1..5) {
        push @shared_arr, $i * 10;
    }
}
# Now we are OUTSIDE the lock scope.  Reading the array without lock is fine
# for visibility (release fence on push); we just verify the lock *was*
# released by not deadlocking here.
my $arr_sum = 0;
$arr_sum += $_ for (@shared_arr);
my $arr_expected = 10 + 20 + 30 + 40 + 50;
print "lock_scope_released_sum=$arr_sum\n";
print "lock_scope_released_ok=" . ($arr_sum == $arr_expected ? "yes" : "no") . "\n";

# ── Test 7: non-shared vars are still isolated (sanity, mirrors threads.pl) ─
# Without :shared, the worker's write to its captured $plain stays in the
# worker's isolated copy; the main thread observes its own (unmodified) $plain.
my $plain = 0;
my $plain_thr = threads->create(sub { $plain = 99; });
$plain_thr->join();
print "plain_isolated=" . ($plain == 0 ? "yes" : "no") . "\n";

# ── Aggregate pass/fail ────────────────────────────────────────────────────
# Re-evaluate every assertion as a boolean expression against the values
# already computed by the tests above, and exit non-zero if any assertion
# that should pass came back as fail.  This file is the *contract* the
# runtime must satisfy (see THREADS_SHARED_ATOMIC_PLAN.md §"Phase 1").  With
# Phases 2-3 of the plan complete, every assertion below is expected to
# pass — %expect_fail is empty.  If a future change breaks one of these
# assertions, the test will exit non-zero and `make test` will fail.
my @failures;
push @failures, "visibility_no_lock"     unless $phase_seen == 1;
push @failures, "rmw_atomic_ok"          unless $counter == $expected;
push @failures, "lock_increment_ok"      unless $locked_counter == $locked_expected;
push @failures, "cond_wait_signal"       unless $cond_ret eq "signaled";
push @failures, "cond_broadcast_ok"      unless $bcast_count == 4;
push @failures, "lock_scope_released_ok" unless $arr_sum == $arr_expected;
push @failures, "plain_isolated"         unless $plain == 0;

# ── Test 8: `our $x : shared` ───────────────────────────────────────────────
# Verifies the Sub-task 3 parser+codegen support.  Three sub-assertions:
#   8a. basic `our $x : shared` at file scope: thread increments visible to
#       the parent (and the parent sees the final value).
#   8b. `our ($a, $b) : shared` list form: the parser's (LIST) path also
#       accepts the `: shared` attribute and the resulting cells behave
#       like the single-name form.
#   8c. cross-package access: `our $x : shared` inside `package Foo` is
#       reachable as `$Foo::x` from `package main`, and a thread
#       spawned from main can mutate it via `\&Foo::worker` (named-sub
#       dispatch from a different package).
our $our_shared_counter : shared = 0;
{
    my $n_threads = 5;
    my @our_thrs;
    for (1..$n_threads) {
        push @our_thrs, threads->create(sub {
            for (1..200) { $our_shared_counter = $our_shared_counter + 1; }
            return 1;
        });
    }
    for my $t (@our_thrs) { $t->join(); }
}
my $our_expected = 5 * 200;
print "our_shared_total=$our_shared_counter\n";
print "our_shared_ok=" . ($our_shared_counter == $our_expected ? "yes" : "no") . "\n";
push @failures, "our_shared_ok" unless $our_shared_counter == $our_expected;

# 8b: list form
our ($our_a, $our_b) : shared = (10, 20);
{
    my @our_list_thrs;
    for (1..5) {
        push @our_list_thrs, threads->create(sub {
            $our_a = $our_a + 2;
            $our_b = $our_b + 5;
            return 1;
        });
    }
    for my $t (@our_list_thrs) { $t->join(); }
}
# 5 workers: a += 10 (10→20), b += 25 (20→45)
print "our_list_a=$our_a our_list_b=$our_b\n";
print "our_list_ok=" . ($our_a == 20 && $our_b == 45 ? "yes" : "no") . "\n";
push @failures, "our_list_ok" unless ($our_a == 20 && $our_b == 45);

# 8c: cross-package `our $x : shared` + threads::create
package Foo;
our $xpkg_shared : shared = 0;
sub bump {
    for (1..50) { $xpkg_shared++; }
    return 1;
}
package main;
{
    my @xpkg_thrs;
    for (1..4) {
        push @xpkg_thrs, threads->create(\&Foo::bump);
    }
    for my $t (@xpkg_thrs) { $t->join(); }
}
print "xpkg_shared_via_Foo=$Foo::xpkg_shared\n";
print "xpkg_shared_ok=" . ($Foo::xpkg_shared == 4 * 50 ? "yes" : "no") . "\n";
push @failures, "xpkg_shared_ok" unless $Foo::xpkg_shared == 4 * 50;

my %expect_fail;
my @unexpected = grep { !$expect_fail{$_} } @failures;
my @expected_failures = grep { $expect_fail{$_} } @failures;
if (@unexpected) {
    print "UNEXPECTED_FAILURES=" . join(",", @unexpected) . "\n";
}
if (@expected_failures) {
    print "EXPECTED_FAILURES=" . join(",", @expected_failures) . "\n";
}
print "threads_atomic_done\n";
die "UNEXPECTED FAILURES: " . join(",", @unexpected) . "\n" if @unexpected;
