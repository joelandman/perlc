# Deep test for D175: $] (the "oldstyle" decimal Perl version number,
# distinct from $^V's "v5.44.0" dotted-string form) was not
# implemented AT ALL -- not just missing from string interpolation,
# but completely broken even as a bare expression (`my $v = $];`
# silently gave undef/empty, and `print $];` printed nothing). Found
# via a real /usr/bin/gprofng-display-html script's
# `version->parse("$]")->normal`.
#
# Two separate, independent fixes were needed:
# 1. The lexer/parser: $] lexes as a single mutated SCALAR token
#    (text="]", mirroring the pre-existing $+ convention) and needed
#    an explicit parsePrimary() check for that text BEFORE the
#    generic "advance past $, read the next token as the name" path
#    (which has no second token to read for this single-token form).
# 2. The STRING INTERPOLATION scanner (parseStringInterp) is a wholly
#    separate, raw-text-based system from the main lexer/parser and
#    needed its own, separate fix to the "$. $, $\ $& $! $/" special-
#    single-char-variable list.
#
# NOTE: $]'s value is tied to the actual host perl's version (this
# project hardcodes it as a hardcoded string, the same pre-existing
# convention $^V's own implementation already uses) -- this test will
# need updating if the host perl is ever upgraded, same as any such
# hardcoded-constant test would.
my $bare = $];
print "bare=$bare\n";

print "print_direct=", $], "\n";

my $interp = "$]";
print "interp=$interp\n";

# numeric coercion: $] is a genuine number, not just a string that
# happens to look like one
my $plus_one = $] + 1;
print "numeric=$plus_one\n";

# used inside a larger interpolated string, combined with other text
print "combined: version is $] today\n";

print "done\n";
