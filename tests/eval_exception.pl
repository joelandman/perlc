#!/usr/bin/perl
use strict;
use warnings;

# Simple eval/die test
my $result = eval {
    die "test error";
    return "should not reach";
};
print "eval_result=" . ($result ? "defined" : "undef") . "\n";
print "eval_err=" . ($@ ? "has_error" : "no_error") . "\n";

# Nested eval
my $outer = eval {
    eval {
        die "inner error";
    };
    return "inner caught";
};
print "nested_eval=" . $outer . "\n";

print "eval_tests_done\n";
