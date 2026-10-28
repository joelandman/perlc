#!/usr/bin/perl
# Regression guard (perlc D190) for the `$;`-joined composite-key hash
# subscript, the `$h{"a","b"}` idiom used by real scripts such as
# /usr/bin/callgrind_annotate. A multi-key subscript must index the same
# entry real Perl does (the keys joined by $SUBSEP), and a write through the
# same composite key must round-trip. Output must match real perl
# byte-for-byte.
use strict;
use warnings;

my %h;
$h{"a", "b"} = 42;
print "lookup:", $h{"a", "b"}, "\n";
print "get:", (defined $h{"a", "b"} ? "defined" : "undef"), "\n";

# a fresh key does not exist before assignment
print "missing:", (defined $h{"x", "y"} ? "defined" : "undef"), "\n";

# a write through the composite key round-trips through a read
$h{"p", "q"} = "hello";
print "roundtrip:", $h{"p", "q"}, "\n";
print "d190_composite_key_done\n";
