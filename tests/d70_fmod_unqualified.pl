# D70: POSIX::fmod unqualified import deep test
# Comprehensive tests for POSIX::fmod with both qualified and unqualified forms
# (no use warnings to avoid diagnostic differences, D56)
use POSIX qw(fmod);

# Section 1: Basic qualified form
print "=== Section 1: Qualified form ===\n";
{
    die "1.1" unless POSIX::fmod(10, 3) == 1;
    die "1.2" unless POSIX::fmod(10, 4) == 2;
    die "1.3" unless POSIX::fmod(7, 2) == 1;
    die "1.4" unless POSIX::fmod(10, 5) == 0;
    print "section1=ok\n";
}

# Section 2: Basic unqualified form
print "=== Section 2: Unqualified form ===\n";
{
    die "2.1" unless fmod(10, 3) == 1;
    die "2.2" unless fmod(10, 4) == 2;
    die "2.3" unless fmod(7, 2) == 1;
    die "2.4" unless fmod(10, 5) == 0;
    print "section2=ok\n";
}

# Section 3: Negative operands
print "=== Section 3: Negative operands ===\n";
{
    die "3.1" unless POSIX::fmod(-10, 3) == -1;
    die "3.2" unless POSIX::fmod(10, -3) == 1;
    die "3.3" unless POSIX::fmod(-10, -3) == -1;
    print "section3=ok\n";
}

# Section 4: Float operands
print "=== Section 4: Float operands ===\n";
{
    my $r = POSIX::fmod(5.5, 2);
    die "4.1" unless abs($r - 1.5) < 0.001;
    $r = fmod(5.5, 2);
    die "4.2" unless abs($r - 1.5) < 0.001;
    print "section4=ok\n";
}

# Section 5: Variables as operands
print "=== Section 5: Variables as operands ===\n";
{
    my $a = 17;
    my $b = 5;
    die "5.1" unless POSIX::fmod($a, $b) == 2;
    die "5.2" unless fmod($a, $b) == 2;
    print "section5=ok\n";
}

# Section 6: Expression as operands
print "=== Section 6: Expression as operands ===\n";
{
    die "6.1" unless POSIX::fmod(3 + 4, 2) == 1;
    die "6.2" unless fmod(3 + 4, 2) == 1;
    print "section6=ok\n";
}

# Section 7: Multiple POSIX functions together
print "=== Section 7: Multiple POSIX functions ===\n";
{
    die "7.1" unless fmod(10, 3) == 1;
    die "7.2" unless POSIX::floor(3.7) == 3;
    die "7.3" unless POSIX::ceil(3.2) == 4;
    print "section7=ok\n";
}

print "all=ok\n";
