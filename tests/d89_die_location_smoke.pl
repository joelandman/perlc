# D89: die message should append " at FILE line N." when no trailing newline
use strict;
use warnings;

# Test 1: eval-caught die with no newline
eval { die "oops" };
if ($@ =~ /oops at .+ line \d+\./) {
    print "1: ok\n";
} else {
    die "test1: got [$@]\n";
}

# Test 2: eval-caught die with trailing newline — no suffix
eval { die "ok\n" };
if ($@ eq "ok\n") {
    print "2: ok\n";
} else {
    die "test2: got [$@]\n";
}

# Test 3: var-based die with no newline
eval {
    my $msg = "variable error";
    die $msg;
};
if ($@ =~ /variable error at .+ line \d+\./) {
    print "3: ok\n";
} else {
    die "test3: got [$@]\n";
}

# Test 4: die with empty string (should still get location)
eval { die "" };
if ($@ =~ /at .+ line \d+\./) {
    print "4: ok\n";
} else {
    die "test4: got [$@]\n";
}

print "all ok\n";
