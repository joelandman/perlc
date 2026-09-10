#!/usr/bin/perl
# Smoke test for D107: the replacement text of s/PATTERN/REPLACEMENT/
# was never processed for backslash escapes — `\\` (an escaped
# backslash, one literal `\`) stayed as two literal backslash
# characters, and `\n`/`\t` stayed as literal 2-char backslash+letter
# instead of becoming an actual newline/tab.
use strict;
use warnings;

$_ = "hello world\n";
s/\\/\\\\/g;
s/\n/\\n/g;
print "$_\n";
print(($_ eq "hello world\\n") ? "ok\n" : "FAIL\n");
