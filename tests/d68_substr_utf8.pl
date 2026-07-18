# D68: substr UTF-8 awareness deep test
# Comprehensive tests for substr with multi-byte UTF-8 characters
# (no use warnings — Perl's "substr outside of string" diagnostic, D56, would differ)

# Section 1: Basic multi-byte character extraction
print "=== Section 1: Basic multi-byte ===\n";
{
    my $s = chr(233) . "bc";  # ébc
    die "1.1" unless ord(substr($s, 0, 1)) == 233;
    die "1.2" unless ord(substr($s, 1, 1)) == 98;
    die "1.3" unless ord(substr($s, 2, 1)) == 99;
    die "1.4" unless length(substr($s, 0, 1)) == 1;
    print "section1=ok\n";
}

# Section 2: Multi-byte character at various positions
print "=== Section 2: Various positions ===\n";
{
    my $s = "a" . chr(246) . "c" . chr(247) . "e";  # a÷c÷e
    die "2.1" unless ord(substr($s, 0, 1)) == 97;
    die "2.2" unless ord(substr($s, 1, 1)) == 246;
    die "2.3" unless ord(substr($s, 2, 1)) == 99;
    die "2.4" unless ord(substr($s, 3, 1)) == 247;
    die "2.5" unless ord(substr($s, 4, 1)) == 101;
    print "section2=ok\n";
}

# Section 3: 3-byte UTF-8 characters
print "=== Section 3: 3-byte UTF-8 ===\n";
{
    my $s = chr(0x00E9) . chr(0x00F6) . "xy";  # éöxy
    die "3.1" unless ord(substr($s, 0, 1)) == 0x00E9;
    die "3.2" unless ord(substr($s, 1, 1)) == 0x00F6;
    die "3.3" unless ord(substr($s, 2, 1)) == 120;
    die "3.4" unless ord(substr($s, 3, 1)) == 121;
    print "section3=ok\n";
}

# Section 4: 4-byte UTF-8 characters
print "=== Section 4: 4-byte UTF-8 ===\n";
{
    my $s = chr(0x1F600) . "ab";  # 😀ab
    die "4.1" unless ord(substr($s, 0, 1)) == 0x1F600;
    die "4.2" unless ord(substr($s, 1, 1)) == 97;
    die "4.3" unless ord(substr($s, 2, 1)) == 98;
    print "section4=ok\n";
}

# Section 5: substr with length > remaining chars
print "=== Section 5: Length beyond end ===\n";
{
    my $s = chr(233) . "bc";
    my $r = substr($s, 1, 100);
    die "5.1" unless length($r) == 2;
    die "5.2" unless ord(substr($r, 0, 1)) == 98;
    die "5.3" unless ord(substr($r, 1, 1)) == 99;
    print "section5=ok\n";
}

# Section 6: Negative offset
print "=== Section 6: Negative offset ===\n";
{
    my $s = chr(233) . "bc";
    my $r = substr($s, -1, 1);
    die "6.1" unless ord($r) == 99;
    $r = substr($s, -2, 1);
    die "6.2" unless ord($r) == 98;
    $r = substr($s, -3, 1);
    die "6.3" unless ord($r) == 233;
    print "section6=ok\n";
}

# Section 7: Negative length
print "=== Section 7: Negative length ===\n";
{
    my $s = chr(233) . "bcd";  # ébcd = 4 chars
    my $r = substr($s, 0, -1);  # stop 1 char from end = ébc = 3 chars
    die "7.1" unless length($r) == 3;
    die "7.2" unless ord(substr($r, 0, 1)) == 233;
    die "7.3" unless ord(substr($r, 1, 1)) == 98;
    die "7.4" unless ord(substr($r, 2, 1)) == 99;
    print "section7=ok\n";
}

# Section 8: Mixed ASCII and multi-byte
print "=== Section 8: Mixed ASCII and multi-byte ===\n";
{
    my $s = "Hello" . chr(233) . " World";
    die "8.1" unless ord(substr($s, 5, 1)) == 233;
    die "8.2" unless ord(substr($s, 6, 1)) == 32;
    my $sub = substr($s, 0, 5);
    die "8.3" unless length($sub) == 5;
    die "8.4" unless ord(substr($sub, 0, 1)) == 72;  # 'H'
    print "section8=ok\n";
}

# Section 9: 2-arg substr (no length)
print "=== Section 9: 2-arg substr ===\n";
{
    my $s = chr(233) . "bcd";
    my $r = substr($s, 1);
    die "9.1" unless length($r) == 3;
    die "9.2" unless ord(substr($r, 0, 1)) == 98;
    die "9.3" unless ord(substr($r, 2, 1)) == 100;
    $r = substr($s, 0);
    die "9.4" unless length($r) == 4;
    die "9.5" unless ord(substr($r, 0, 1)) == 233;
    print "section9=ok\n";
}

# Section 10: Empty result
print "=== Section 10: Empty result ===\n";
{
    my $s = chr(233) . "bc";
    my $r = substr($s, 0, 0);
    die "10.1" unless length($r) == 0;
    $r = substr($s, 10, 1);
    die "10.2" unless !defined($r) || length($r) == 0;
    print "section10=ok\n";
}

print "all=ok\n";
