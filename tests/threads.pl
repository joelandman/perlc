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

print "threads_done\n";
