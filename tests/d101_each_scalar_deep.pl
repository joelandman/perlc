#!/usr/bin/perl
# Deep test for D101: `each %hash` in scalar context returned the
# pair-array's element count (0, 1, or 2) instead of the key itself —
# `while (my $k = each %h)` iterated the right *number* of times (a
# truthy 2 happens to look like a working loop) but every $k was wrong.
# Root cause: src/codegen.cpp's case NK::EachFunc (scalar context)
# called perl_array_len(av) instead of perl_array_get(av, 0). Fix
# reuses perl_array_get, which already returns undef for an
# out-of-range index — exactly the post-exhaustion case.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# collect every key seen via scalar-context each, order-independent
my %h = (a => 1, b => 2, c => 3);
my @seen;
while (my $k = each %h) {
    push @seen, $k;
}
check('scalar_each_key_count', scalar(@seen) == 3);
check('scalar_each_keys_match', join(",", sort @seen) eq "a,b,c");

# single-key hash: exactly one iteration, key is that key, then undef
my %single = (only => 1);
my $k1 = each %single;
check('single_key_first', $k1 eq "only");
my $k2 = each %single;
check('single_key_exhausted', !defined($k2));

# empty hash: immediately undef
my %empty;
my $k3 = each %empty;
check('empty_hash_undef', !defined($k3));

# re-iterating after exhaustion (each auto-resets once the iterator
# runs off the end) works the same way a second time
my %again = (x => 1, y => 2);
my @first_pass;
while (my $k = each %again) { push @first_pass, $k; }
my @second_pass;
while (my $k = each %again) { push @second_pass, $k; }
check('reiterate_after_exhaustion',
    join(",", sort @first_pass) eq join(",", sort @second_pass));

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d101_each_scalar_done\n";
