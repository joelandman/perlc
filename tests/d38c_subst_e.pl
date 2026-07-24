#!/usr/bin/perl
# Deep: D38c — s///e evaluate replacement
print "=== basic e ===\n";
{
    my $x = "hello";
    $x =~ s/l/uc($&)/e;
    print "x=$x\n";
}

print "=== ge with capture ===\n";
{
    my $y = "a1b2c";
    $y =~ s/(\d)/$1+1/ge;
    print "y=$y\n";
}

print "=== string literal repl ===\n";
{
    my $z = "foo";
    $z =~ s/o/"X"/e;
    print "z=$z\n";
}

print "=== uc capture ge ===\n";
{
    my $w = "ab";
    $w =~ s/(.)/uc($1)/ge;
    print "w=$w\n";
}

print "=== outer lexical ===\n";
{
    my $p = "pre";
    my $s = "a-b";
    $s =~ s/-/$p/e;
    print "s=$s\n";
}

print "=== count ===\n";
{
    my $t = "xx";
    my $n = ($t =~ s/x/uc($&)/ge);
    print "n=$n t=$t\n";
}

print "=== no e still literal ===\n";
{
    my $u = "ab";
    $u =~ s/a/uc($&)/;
    print "u=$u\n";
}

print "d38c_subst_e_done\n";
