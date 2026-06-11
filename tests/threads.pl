#!/usr/bin/perl
use strict;
use warnings;
use threads;

# Test 1: basic thread creation and join, return value
my $thr = threads->create(sub { return 42; });
my $result = $thr->join();
print "retval=$result\n";

# Test 2: thread with arguments
my $thr2 = threads->create(sub { return $_[0] + $_[1]; }, 10, 32);
my $sum = $thr2->join();
print "sum=$sum\n";

# Test 3: thread tid is nonzero
my $thr3 = threads->create(sub { return threads->self()->tid(); });
my $tid = $thr3->join();
print "tid_ok=" . ($tid > 0 ? "yes" : "no") . "\n";

# Test 4: main thread tid is 0
print "main_tid=" . threads->self()->tid() . "\n";

# Test 5: multiple threads, all return values collected
my @thrs;
for my $i (1..5) {
    push @thrs, threads->create(sub { return $_[0] * $_[0]; }, $i);
}
my @results;
for my $t (@thrs) {
    push @results, $t->join();
}
my @sorted = sort { $a <=> $b } @results;
print "squares=" . join(",", @sorted) . "\n";

# Test 6: closure capture works in threads
my $base = 100;
my $thr6 = threads->create(sub { return $base + $_[0]; }, 7);
my $r6 = $thr6->join();
print "closure=$r6\n";

# Test 7: isolation — thread modifying a captured 'my' var doesn't affect parent
my $isolated = 10;
my $thr7 = threads->create(sub { $isolated = 99; return $isolated; });
my $r7 = $thr7->join();
print "thread_saw=$r7\n";              # 99
print "parent_saw=$isolated\n";        # 10

# Test 8: threads::shared — thread's write to a shared var is visible in parent after join
use threads::shared;
my $shared_val : shared = 0;
my $thr8 = threads->create(sub { $shared_val = 42; });
$thr8->join();
print "shared_ok=" . ($shared_val == 42 ? "yes" : "no") . "\n";  # yes

# Test 9: non-shared var still isolated
my $local_only = 0;
my $thr9 = threads->create(sub { $local_only = 99; });
$thr9->join();
print "isolated_ok=" . ($local_only == 0 ? "yes" : "no") . "\n";  # yes

# Test 10: lock() enables atomic read-modify-write on a shared scalar
my $counter : shared = 0;
my @inc_workers;
for my $i (1..10) {
    push @inc_workers, threads->create(sub {
        lock($counter);
        $counter = $counter + 1;
    });
}
for my $w (@inc_workers) { $w->join(); }
print "lock_ok=" . ($counter == 10 ? "yes" : "no") . "\n";  # yes

# Test 11: shared array with lock
my @shared_arr : shared;
my @arr_workers;
for my $i (1..5) {
    push @arr_workers, threads->create(sub {
        lock(@shared_arr);
        push @shared_arr, $_[0];
    }, $i);
}
for my $w (@arr_workers) { $w->join(); }
my @sorted_arr = sort { $a <=> $b } @shared_arr;
print "shared_arr_ok=" . (scalar(@sorted_arr) == 5 ? "yes" : "no") . "\n";  # yes

# Test 12: cond_wait / cond_signal producer-consumer
my $ready : shared = 0;
my $thr12 = threads->create(sub {
    lock($ready);
    while (!$ready) { cond_wait($ready); }
    return "signaled";
});
{
    lock($ready);
    $ready = 1;
    cond_signal($ready);
}
my $r12 = $thr12->join();
print "cond_ok=" . ($r12 eq "signaled" ? "yes" : "no") . "\n";  # yes

# Test 13: shared hash — multiple threads write distinct keys, all visible after join
my %shared_hash : shared;
my @hash_workers;
for my $i (1..5) {
    push @hash_workers, threads->create(sub {
        my $k = "k$_[0]";
        lock(%shared_hash);
        $shared_hash{$k} = $_[0] * 10;
    }, $i);
}
for my $w (@hash_workers) { $w->join(); }
my $hash_ok = 1;
for my $i (1..5) {
    $hash_ok = 0 unless exists $shared_hash{"k$i"} && $shared_hash{"k$i"} == $i * 10;
}
print "shared_hash_ok=" . ($hash_ok ? "yes" : "no") . "\n";  # yes

# Test 14: cond_broadcast — multiple waiters all wake up
my $bcast_ready : shared = 0;
my $bcast_count : shared = 0;
my @bcast_thrs;
for my $i (1..3) {
    push @bcast_thrs, threads->create(sub {
        lock($bcast_ready);
        while (!$bcast_ready) { cond_wait($bcast_ready); }
        lock($bcast_count);
        $bcast_count = $bcast_count + 1;
    });
}
{
    lock($bcast_ready);
    $bcast_ready = 1;
    cond_broadcast($bcast_ready);
}
for my $t (@bcast_thrs) { $t->join(); }
print "cond_broadcast_ok=" . ($bcast_count == 3 ? "yes" : "no") . "\n";  # yes

# Test: named-sub closure capture of a shared scalar.
# A named sub (sub name { ... } rather than my $x = sub { ... })
# that closes over a shared scalar and is dispatched via
# threads->create(\&name, ...) must see the shared cell, not a
# thread-local deep copy.  Pre-Sub-task-2 the RefSub path produced
# a plain code ref with ncaptures=0, so the worker's read of the
# captured shared var saw uninitialised memory and the assertion
# below failed.  Now that RefSub builds a closure with the captured
# shared cell, each worker increments the same cell the parent
# sees.  We verify by checking that the parent's final value of
# the shared counter is the sum of all increments performed by
# all threads (race-free, since perl_atomic_inc is a single CAS
# on the int/float payload per the lock-free CAS work).
sub worker_named {
    my $times = $_[0];
    for (1..$times) { $counter_named++; }
    return 1;  # tag the thread as completed
}
my $counter_named : shared = 0;
my $per_thread_named = 100;
my $n_threads_named = 5;
my @worker_thrs;
for my $i (1..$n_threads_named) {
    push @worker_thrs, threads->create(\&worker_named, $per_thread_named);
}
for my $t (@worker_thrs) {
    $t->join();
}
# Final counter must equal n_threads * per_thread (race-free increment).
print "named_sub_closure_total=$counter_named\n";
print "named_sub_closure_ok=" . ($counter_named == $n_threads_named * $per_thread_named ? "yes" : "no") . "\n";

print "threads_done\n";
