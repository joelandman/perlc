# Deep test for D174: q(...)/qq(...)/qx(...) with a BRACKETING
# delimiter pair (where the open and close characters differ --
# (), [], <>, as opposed to a symmetric delimiter like / or |) did
# not track nested depth at all, unlike the existing qq{...} brace
# scanner (a separate, hand-rolled loop) which already did this
# correctly. The shared src/lexer.cpp readString() helper used for
# the non-brace delimiter forms only ever received the single close
# character and stopped at its FIRST occurrence -- confirmed broken
# for (), [], and <> via a real /usr/bin/ucfq script's
# `qq($main::MYNAME $main::VERSION\n\tCopyright (C) 2002-2024 )` (an
# unescaped nested "(C)" inside a qq(...)-delimited string).
my $a = qq(hello (world) foo);
print "paren=$a\n";

my $b = q(a (b (c) d) e);
print "paren_double_nested=$b\n";

my $c = q[hello [world] foo];
print "bracket=$c\n";

my $d = qq[a [b [c] d] e];
print "bracket_double_nested=$d\n";

my $e = qq<hello <world> foo>;
print "angle=$e\n";

# interpolation still works correctly inside a nested-paren qq(...)
my $name = "Perl";
my $f = qq(hello ($name) world);
print "interp=$f\n";

# regression: the pre-existing brace delimiter (already correct) and
# the non-nested, single-pair case for each delimiter type
my $g = qq{a {b} c};
print "brace_regression=$g\n";
my $h = q(simple);
print "simple_paren=$h\n";
my $i = qq[simple];
print "simple_bracket=$i\n";

print "done\n";
