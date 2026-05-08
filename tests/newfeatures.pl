#!/usr/bin/perl
use strict;
use warnings;

# state variables
sub counter {
    state $count = 0;
    $count++;
    return $count;
}
print counter() . "\n";  # 1
print counter() . "\n";  # 2
print counter() . "\n";  # 3

# state in different sub is independent
sub counter2 {
    state $n = 10;
    $n += 2;
    return $n;
}
print counter2() . "\n";  # 12
print counter2() . "\n";  # 14

# wantarray (stub returns false — scalar context)
sub ctx {
    if (wantarray) { return (1, 2, 3); }
    else           { return "scalar";  }
}
print ctx() . "\n";  # scalar

# $/ and local $/
print defined($/) ? "sep defined\n" : "sep undef\n";  # sep defined
{
    local $/ = undef;
    print defined($/) ? "inner defined\n" : "inner undef\n";  # inner undef
}
print defined($/) ? "sep restored\n" : "sep gone\n";  # sep restored

# $! (errno string after a failed open)
open(my $fh, "<", "/no_such_file_perlc_test");
my $err = "$!";
print length($err) > 0 ? "errno ok\n" : "no errno\n";  # errno ok

# BEGIN block (runs inline at start)
my $init = 0;
BEGIN {
    # nothing observable here for compiler; just confirm it doesn't crash
}
$init = 1;
print "init=$init\n";  # init=1

# END block
END {
    print "end ok\n";  # end ok (last line)
}

# caller (stub returns "main")
sub get_pkg {
    my ($pkg) = caller();
    return $pkg;
}
my $p = get_pkg();
print defined($p) ? "caller ok\n" : "caller fail\n";  # caller ok

print "done\n";
