#!/usr/bin/perl
# Deep: Data::Dumper — found completely unimplemented during a real-world
# code survey (2026-09): Dumper() silently produced no output at all,
# despite being one of the most common debugging tools in real Perl code.
#
# Implementation: src/runtime.c perl_dumper() (with helpers dumper_value/
# dumper_array/dumper_hash), dispatched from src/codegen.cpp's "Dumper"/
# "Data::Dumper::Dumper" case (which flattens array/list args the same
# way List::Util::sum does).
#
# Reproduces the default Indent=2 style byte-for-byte: nested structures
# are indented to (column of their opening bracket) + 2, and the closing
# bracket goes back at that same column — which depends on the actual
# rendered width of everything before it on the line (key length,
# "bless( " prefix, etc.), not a fixed per-depth indent. Also reproduces
# the IV-unquoted / NV-and-string-quoted distinction real Dumper makes.
#
# NOT verified/supported here (documented simplifications): circular
# references, code refs/globs, $Data::Dumper::Indent/Terse/Deepcopy/
# Purity config knobs. $Data::Dumper::Sortkeys is honored only within the
# same lexical scope it was set in (see perl_dumper's comment — a
# separate, pre-existing limitation of how perlc handles arbitrary
# $Package::var globals in general, not specific to Dumper). Small
# all-numeric-literal arrays (e.g. [1,2,3]) that hit the Stage 22/23
# FLAT_ARRAY fast path lose the IV/NV distinction and print quoted
# ('1','2','3') instead of bare — a pre-existing representational
# limitation of that optimization (see TESTS.md D105/D106), not tested
# for exact match here; this test uses mixed-type arrays instead, which
# don't take that fast path.
use strict;
use warnings;
use Data::Dumper;
$Data::Dumper::Sortkeys = 1;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

check('scalar_int', Dumper(42) eq "\$VAR1 = 42;\n");
check('scalar_string', Dumper("hi") eq "\$VAR1 = 'hi';\n");
check('scalar_float_quoted', Dumper(3.5) eq "\$VAR1 = '3.5';\n");
check('scalar_undef', Dumper(undef) eq "\$VAR1 = undef;\n");
check('negative_int', Dumper(-3) eq "\$VAR1 = -3;\n");
check('string_escaping', Dumper("it's") eq "\$VAR1 = 'it\\'s';\n");

check('multi_arg_numbering',
    Dumper(1, 2) eq "\$VAR1 = 1;\n\$VAR2 = 2;\n");

check('empty_array', Dumper([]) eq "\$VAR1 = [];\n");
check('empty_hash', Dumper({}) eq "\$VAR1 = {};\n");

{
    my $expected = "\$VAR1 = [\n" .
                   "          1,\n" .
                   "          undef,\n" .
                   "          -3,\n" .
                   "          'x',\n" .
                   "          '3.5'\n" .
                   "        ];\n";
    check('mixed_array', Dumper([1, undef, -3, "x", 3.5]) eq $expected);
}

{
    # column-dependent indent: a longer key pushes its nested value's
    # indent further right than a shorter key's does.
    my %h = (short => ["x"], a_much_longer_key => ["y"]);
    my $out = Dumper(\%h);
    my $expected =
        "\$VAR1 = {\n" .
        "          'a_much_longer_key' => [\n" .
        "                                   'y'\n" .
        "                                 ],\n" .
        "          'short' => [\n" .
        "                       'x'\n" .
        "                     ]\n" .
        "        };\n";
    check('column_dependent_indent', $out eq $expected);
}

{
    my $o = bless { a => 1 }, "Foo::Bar";
    my $expected = "\$VAR1 = bless( {\n" .
                   "                 'a' => 1\n" .
                   "               }, 'Foo::Bar' );\n";
    check('blessed_object', Dumper($o) eq $expected);
}

{
    my $x = 42;
    check('scalar_ref', Dumper(\$x) eq "\$VAR1 = \\42;\n");
}

{
    # $Data::Dumper::Sortkeys, set above in this same top-level scope,
    # must produce alphabetically-sorted keys.
    my %h = (zebra => 1, apple => 2, mango => 3);
    my $out = Dumper(\%h);
    my $expected = "\$VAR1 = {\n" .
                   "          'apple' => 2,\n" .
                   "          'mango' => 3,\n" .
                   "          'zebra' => 1\n" .
                   "        };\n";
    check('sortkeys_honored', $out eq $expected);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "data_dumper_deep_done\n";
