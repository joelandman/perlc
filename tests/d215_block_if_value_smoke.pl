# D215: the value of a block whose last statement is if/unless.
#
# `do { if (1) { "a" } else { "b" } }` used to be empty: emitBlockLast ran
# the if as a statement and dropped both arms. A failed if with no else
# yields the condition itself (the last expression evaluated). A failed
# unless yields the original condition, not the negation.

sub show {
    my ($n, $v) = @_;
    if (!defined $v) { print "$n UNDEF\n"; return; }
    print "$n [$v]\n";
}

show("ifelse1", do { if (1) { "a" } else { "b" } });
show("ifelse0", do { if (0) { "a" } else { "b" } });
show("ifnoelse1", do { if (1) { "a" } });
show("ifnoelse0", do { if (0) { "a" } });
show("ifempty", do { if ("") { "a" } });
show("ifundef", do { if (undef) { "a" } });
show("ifstr0", do { if ("0") { "a" } });
show("elsif", do { if (0) { "a" } elsif (1) { "b" } else { "c" } });
show("elsifnone", do { if (0) { "a" } elsif ("") { "b" } });
show("unless1", do { unless (1) { "a" } });
show("unless0", do { unless (0) { "a" } });
show("unlessfoo", do { unless ("foo") { "a" } });
show("unlesselse", do { unless (1) { "a" } else { "b" } });
show("mod1", do { "a" if 1 });
show("mod0", do { "a" if 0 });
show("modun", do { "a" unless "z" });
show("nested", do { if (1) { if (0) { "a" } else { "b" } } else { "c" } });
show("mycond", do { if (my $y = 5) { $y } else { "no" } });
show("mycond0", do { if (my $y = 0) { "yes" } else { "no:$y" } });
show("emptyelse", do { if (0) { "a" } else { } });
show("scalarlist", scalar do { if (1) { (1, 2, 3) } else { 4 } });
show("plain", do { "z" });
show("before", do { my $t = 1; if ($t) { $t + 2 } else { 9 } });
show("cmp", do { if (1 > 2) { "y" } });
show("cmpt", do { if (1 < 2) { "y" } });

sub f1 { if (1) { "a" } else { "b" } }
show("sub", f1());
sub f2 { if (0) { "a" } }
show("subnoelse", f2());
sub f3 {
    my $x = do { if (1) { return 5 } else { 2 } };
    return "no $x";
}
show("retindo", f3());
sub f4 { if (1) { return 5 } else { 2 } }
show("retif", f4());
