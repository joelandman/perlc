# Helper for tests/do_literal_smoke.pl and tests/do_literal_deep.pl.
# Pure self-contained sub definitions (no top-level statements, no outer
# closures) — the shape blocker #1's static `do "literal.pl"` expansion
# inlines at compile time instead of spawning perlc+clang at runtime.
sub add2 {
    my ($a, $b) = @_;
    return $a + $b;
}

sub greet {
    my ($who) = @_;
    return "hello, $who";
}
