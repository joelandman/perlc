# D215 deep: block-value if/unless beyond the smoke shapes.
# Side effects run once, nested blocks keep the taken arm, grep/map/sort
# and eval see the same value, and a false condition keeps its own
# string ("" vs "0" vs undef).

sub show {
    my ($n, $v) = @_;
    if (!defined $v) { print "$n UNDEF\n"; return; }
    print "$n [$v]\n";
}

my $n = 0;
show("once", do { if (++$n) { "y" } else { "n" } });
print "oncencount $n\n";
my $m = 0;
show("oncefalse", do { if (++$m == 0) { "y" } });
print "oncefalsecount $m\n";

my $f = "";
show("falsevar", do { if ($f) { "a" } });
my $z = "0";
show("strzero", do { if ($z) { "a" } });
my $u;
show("undefvar", do { if ($u) { "a" } });

show("elsif2", do { if (0) { "a" } elsif (0) { "b" } });
show("elsiffall", do { if (0) { "a" } elsif ("qq") { "b" } else { "c" } });
show("unlessvar", do { my $x = 1; unless ($x) { "a" } });
show("modvar", do { my $x = 0; "a" if $x });
show("modun0", do { "a" unless 0 });
show("ifnot", do { if (!1) { "a" } });
show("ifnot0", do { if (!0) { "a" } });

show("nest2", do {
    if (0) { "a" }
    elsif (1) {
        if ("") { "inner" } else { "mid" }
    }
    else { "outer" }
});

show("armmy", do {
    if (1) { my $t = "q"; $t }
    else { "no" }
});

show("emptythen", do { if (1) { } });
show("emptythen0", do { if (0) { } });

sub anon {
    my $f = sub { if ($_[0]) { "yes" } else { "no" } };
    return $f->(1) . "/" . $f->(0);
}
show("anon", anon());

my @g = grep { if ($_ % 2) { $_ } else { 0 } } (1, 2, 3, 4);
print "grep [", join(",", @g), "]\n";

my @mp = map { if ($_ > 2) { $_ * 10 } else { $_ } } (1, 2, 3);
print "map [", join(",", @mp), "]\n";

my @s = sort {
    if ($a < $b) { return -1 } else { return 1 }
} (3, 1, 2);
print "sort [", join(",", @s), "]\n";

my @s2 = sort { return $a <=> $b } (3, 1, 2);
print "sortret [", join(",", @s2), "]\n";

sub ev1 {
    my $r = eval { if (1) { return 5 } else { 9 } };
    return "$r/$@";
}
show("evalret", ev1());
sub ev2 {
    my $r = eval { if (0) { return 5 } else { 9 } };
    return "$r";
}
show("evalelse", ev2());

sub early {
    my $x = do { if (0) { return 5 } else { 2 } };
    return "fall $x";
}
show("elsefall", early());

my $side = 0;
show("condside", do {
    if ($side++) { "a" }
    elsif ($side++) { "b" }
    else { "c" }
});
print "sidecount $side\n";

show("listelse", do { if (0) { (1, 2, 3) } else { 4 } });
show("paren", do { if (1) { ("p") } else { "q" } });

sub bothret { if (1) { return 5 } else { return 6 } }
show("bothret", bothret());

sub nestret {
    if (1) {
        if (0) { return 1 } else { return 2 }
    } else { return 3 }
}
show("nestret", nestret());
