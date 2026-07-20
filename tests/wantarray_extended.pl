#!/usr/bin/perl
use strict;

# ── Test 1: grep/map/sort inside nested subs called from list context ──────
# sub outer { inner() } where inner calls grep — should propagate list ctx

sub inner_grep {
    my @data = @_;
    return grep { $_ > 2 } @data;
}

sub inner_map {
    my @data = @_;
    return map { $_ * 2 } @data;
}

sub inner_sort {
    my @data = @_;
    return sort { $a <=> $b } @data;
}

sub outer_grep { return inner_grep(@_); }
sub outer_map  { return inner_map(@_); }
sub outer_sort { return inner_sort(@_); }

my @filtered = outer_grep(1, 2, 3, 4, 5);
print "grep_nested=" . join(",", @filtered) . "\n";  # 3,4,5

my @doubled = outer_map(1, 2, 3);
print "map_nested=" . join(",", @doubled) . "\n";  # 2,4,6

my @sorted = outer_sort(3, 1, 2);
print "sort_nested=" . join(",", @sorted) . "\n";  # 1,2,3

# ── Test 2: Implicit last expression in anonymous subs in list context ─────
# Anonymous subs should return the last expression's value in list context

my @results = (sub { return (1, 2, 3) })->();
print "anon_sub_list=" . join(",", @results) . "\n";  # 1,2,3

# Test with implicit return (no explicit return)
my @implicit = (sub {
    my @data = @_;
    grep { $_ % 2 == 0 } @data;
})->(1, 2, 3, 4, 5);
print "anon_sub_implicit=" . join(",", @implicit) . "\n";  # 2,4

# ── Test 3: wantarray inside deeply nested closures ────────────────────────

sub level1 {
    my $ctx = wantarray;
    return $ctx ? 'list' : 'scalar';
}

sub level2 {
    my $result = level1();
    return $result;
}

sub level3 {
    my $result = level2();
    return $result;
}

# Top-level: scalar context
print "nested_ctx_top=" . level3() . "\n";  # scalar

# List context: level3 called from list context
my @ctx_results = level3();
print "nested_ctx_list_count=" . scalar(@ctx_results) . "\n";  # 1 (scalar returns single value)

# ── Test 4: wantarray propagation through call chains ──────────────────────

sub caller_ctx {
    my $ctx = wantarray;
    return $ctx ? 'list' : 'scalar';
}

sub mid_fn {
    return caller_ctx();
}

sub top_fn {
    return mid_fn();
}

# Scalar context at top
print "ctx_prop_scalar=" . top_fn() . "\n";  # scalar

# List context at top
my @ctx_prop_list = top_fn();
print "ctx_prop_list_count=" . scalar(@ctx_prop_list) . "\n";  # 1

# ── Test 5: grep/map in anonymous sub passed to threads ────────────────────

use threads;

my $worker = sub {
    my @data = @_;
    return grep { $_ > 5 } @data;
};

my $thr = threads->create($worker, 1, 2, 6, 7, 8);
my @worker_result = $thr->join();
print "thread_grep=" . join(",", @worker_result) . "\n";  # 6,7,8

# ── Test 6: sort with block in nested context ─────────────────────────────

sub inner_sort_block {
    return sort { $b <=> $a } @_;
}

sub outer_sort_block {
    return inner_sort_block(@_);
}

my @desc = outer_sort_block(5, 3, 8, 1, 9);
print "sort_desc=" . join(",", @desc) . "\n";  # 9,8,5,3,1

# ── Test 7: map with block in nested context ──────────────────────────────

sub inner_map_block {
    return map { uc($_) } @_;
}

sub outer_map_block {
    return inner_map_block(@_);
}

my @upper = outer_map_block("hello", "world");
print "map_upper=" . join(",", @upper) . "\n";  # HELLO,WORLD

# ── Test 8: scalar context for grep/map/sort ──────────────────────────────

sub count_grep {
    return grep { $_ > 2 } 1, 2, 3, 4, 5;
}

sub count_map {
    return map { $_ * 2 } 1, 2, 3;
}

sub count_sort {
    my @data = (3, 1, 2);
    return sort { $a <=> $b } @data;
}

my $grep_count = count_grep();
print "grep_scalar_ctx=$grep_count\n";  # 3

my $map_count = count_map();
print "map_scalar_ctx=$map_count\n";  # 6 (3*2)

my $sort_count = count_sort();
print "sort_scalar_ctx=", (defined($sort_count) ? $sort_count : ""), "\n";  # undef in scalar

print "wantarray_tests_done\n";
