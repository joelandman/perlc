#!/usr/bin/perl
use strict;
use warnings;
use threads;
use threads::shared;

# ── Regression Test 1: compound -= on shared scalar ─────────────────────────
# Previously: $shared -= 5 was emitted as perl_atomic_add($shared, +5)
# Fix: negates delta before perl_atomic_add

my $counter : shared = 100;
my $worker = sub {
    $counter -= 10;
    return $counter;
};
my $thr = threads->create($worker);
my $ret = $thr->join();
print "regression_subtract=" . ($ret == 90 ? "pass" : "fail") . "\n";

# Test with multiple threads
my $counter2 : shared = 1000;
my @threads;
for my $i (1..10) {
    push @threads, threads->create(sub {
        $counter2 -= 10;
    });
}
for my $t (@threads) { $t->join(); }
print "regression_multi_subtract=" . ($counter2 == 0 ? "pass" : "fail") . "\n";

# ── Regression Test 2: closure + range-with-captured-variable ───────────────
# Previously: for (1..$per) where $per is captured emitted undef bound
# Fix: closure capture now checks intScopes_ and floatScopes_

my $limit = 5;
my $sum : shared = 0;
my $thr2 = threads->create(sub {
    my $local_limit = $limit;  # captured from outer scope
    for my $i (1..$local_limit) {
        $sum += $i;
    }
});
$thr2->join();
print "regression_closure_range=" . ($sum == 15 ? "pass" : "fail") . "\n";  # 1+2+3+4+5=15

# Test with anonymous sub (no threads)
my $per = 10;
my @result = (sub {
    my @nums;
    for my $i (1..$per) {
        push @nums, $i;
    }
    return @nums;
})->();
print "regression_anon_range=" . (join(",", @result) eq "1,2,3,4,5,6,7,8,9,10" ? "pass" : "fail") . "\n";

# ── Regression Test 3: shared scalar *= / /= / %= (non-add ops) ────────────
# Previously: *, /, % fell through to non-atomic perl_assign

my $shared_val : shared = 20;
my $thr3 = threads->create(sub {
    $shared_val *= 2;
});
$thr3->join();
print "regression_multiply=" . ($shared_val == 40 ? "pass" : "fail") . "\n";

my $shared_val2 : shared = 100;
my $thr4 = threads->create(sub {
    $shared_val2 /= 4;
});
$thr4->join();
print "regression_divide=" . ($shared_val2 == 25 ? "pass" : "fail") . "\n";

my $shared_val3 : shared = 17;
my $thr5 = threads->create(sub {
    $shared_val3 %= 5;
});
$thr5->join();
print "regression_modulo=" . ($shared_val3 == 2 ? "pass" : "fail") . "\n";

print "regression_tests_done\n";
