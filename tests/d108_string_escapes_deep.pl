#!/usr/bin/perl
# Deep test for D108: plain double-quoted string literals (unrelated to
# s///, which already handled these via D107) didn't recognize
# \f (form feed), \a (bell), \e (escape), \b (backspace) — they passed
# through as a literal backslash followed by the letter. Root cause:
# src/lexer.cpp's double-quoted-string escape switch (readString) only
# had cases for n t r 0 x \ ' " $ @. Fix added the four missing cases
# there, and mirrored the same fix in the separate qq{...} balanced-
# brace escape switch (a second, independent code path with the
# identical gap).
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

check('form_feed',  ord("\f") == 12);
check('bell',       ord("\a") == 7);
check('escape',     ord("\e") == 27);
check('backspace',  ord("\b") == 8);

my $combined = "a\fb\ac\ed\be";
check('combined_length', length($combined) == 9);
check('combined_bytes', join(",", map { ord($_) } split(//, $combined))
    eq "97,12,98,7,99,27,100,8,101");

# qq{...} balanced-brace form — a separate lexer code path with the
# identical gap
my $qq = qq{x\fy\az\eb\bc};
check('qq_brace_form', $qq eq "x\fy\az\eb\bc");

# unaffected: already-working escapes stay correct
check('existing_escapes_unaffected', "a\nb\tc\rd" eq "a\nb\tc\rd");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d108_string_escapes_done\n";
