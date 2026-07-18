# D93: wantarray ternary dispatch deep test
# Comprehensive tests for wantarray() ? (LIST) : SCALAR pattern
# (no use warnings to avoid diagnostic differences, D56)

# Section 1: Basic list return in list context
print "=== Section 1: Basic list return ===\n";
{
    sub f1 { return wantarray() ? (1,2,3) : "scalar"; }
    my @a = (f1());
    die "1.1" unless scalar(@a) == 3;
    die "1.2" unless $a[0] == 1 && $a[1] == 2 && $a[2] == 3;
    print "section1=ok\n";
}

# Section 2: Scalar return in scalar context
print "=== Section 2: Scalar return ===\n";
{
    sub f2 { return wantarray() ? (1,2,3) : "scalar"; }
    my $s = f2();
    die "2.1" unless $s eq "scalar";
    print "section2=ok\n";
}

# Section 3: List in true branch, scalar in false branch
print "=== Section 3: List true, scalar false ===\n";
{
    sub f3 { return wantarray() ? (10,20) : "none"; }
    my @a = (f3());
    die "3.1" unless scalar(@a) == 2;
    die "3.2" unless $a[0] == 10 && $a[1] == 20;
    my $s = f3();
    die "3.3" unless $s eq "none";
    print "section3=ok\n";
}

# Section 4: Scalar in true branch, list in false branch
print "=== Section 4: Scalar true, list false ===\n";
{
    sub f4 { return wantarray() ? "single" : (1,2); }
    my @a = (f4());
    die "4.1" unless scalar(@a) == 1;  # wantarray() is true, so returns "single"
    die "4.2" unless $a[0] eq "single";
    my $s = f4();
    die "4.3" unless $s == 2;  # scalar context: last element of false branch
    print "section4=ok\n";
}

# Section 5: List in both branches
print "=== Section 5: List both branches ===\n";
{
    sub f5 { return wantarray() ? (1,2) : (3,4); }
    my @a = (f5());
    die "5.1" unless scalar(@a) == 2;
    die "5.2" unless $a[0] == 1 && $a[1] == 2;
    my $s = f5();
    die "5.3" unless $s == 4;  # scalar context: last element of false branch
    print "section5=ok\n";
}

# Section 6: Empty list in true branch
print "=== Section 6: Empty list ===\n";
{
    sub f6 { return wantarray() ? () : "none"; }
    my @a = (f6());
    die "6.1" unless scalar(@a) == 0;
    my $s = f6();
    die "6.2" unless $s eq "none";
    print "section6=ok\n";
}

# Section 7: Nested ternary
print "=== Section 7: Nested ternary ===\n";
{
    sub f7 { return wantarray() ? (wantarray() ? (1,2) : "inner", 3) : "outer"; }
    my @a = (f7());
    die "7.1" unless scalar(@a) == 3;
    die "7.2" unless $a[0] == 1 && $a[1] == 2 && $a[2] == 3;
    my $s = f7();
    die "7.3" unless $s eq "outer";
    print "section7=ok\n";
}

# Section 8: List with expressions
print "=== Section 8: List with expressions ===\n";
{
    sub f8 { return wantarray() ? (1+1, 2*3, 4-5) : "scalar"; }
    my @a = (f8());
    die "8.1" unless scalar(@a) == 3;
    die "8.2" unless $a[0] == 2 && $a[1] == 6 && $a[2] == -1;
    print "section8=ok\n";
}

# Section 9: List from sub call
print "=== Section 9: List from sub call ===\n";
{
    sub items { return (10,20,30); }
    sub f9 { return wantarray() ? items() : "scalar"; }
    my @a = (f9());
    die "9.1" unless scalar(@a) == 3;
    die "9.2" unless $a[0] == 10 && $a[1] == 20 && $a[2] == 30;
    print "section9=ok\n";
}

# Section 10: List assignment from ternary
print "=== Section 10: List assignment ===\n";
{
    sub f10 { return wantarray() ? (1,2,3) : "scalar"; }
    my ($x, $y, $z) = f10();
    die "10.1" unless $x == 1 && $y == 2 && $z == 3;
    print "section10=ok\n";
}

print "all=ok\n";
