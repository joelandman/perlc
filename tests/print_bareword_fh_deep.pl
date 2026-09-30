# D153 deep: bareword filehandles in print/say/printf — never opened, opened
# later (inside a sub called afterwards), reopened; subs, native exports and
# lexical handles keep their meaning.
use strict;
use feature 'say';
use File::Basename;
my $t = "/tmp/perlc_pbfh_deep_$$";

sub label { return "<" . join(",", @_) . ">" }
sub open_late { open(LATE, ">", $t) or die "open: $!" }

# never opened: every form prints nothing and does not die
my $x = 42;
print NEVER "lit";
print NEVER $x;
print NEVER "a", $x, "c";
say NEVER "s";
printf NEVER "%d-%s", 1, "two";
print "never: done\n";

# declared subs and native exports are still calls
print label "p", "q";
print "\n";
print basename "/a/b/c.txt";
print "\n";

# opened later in the source (inside a sub called at runtime)
open_late();
print LATE "one|";
printf LATE "%02d|", 3;
say LATE "three";
close LATE;
open(my $in, "<", $t) or die;
my $content = do { local $/; <$in> };
close $in;
print "late: [$content]";

# reopen for append via the same bareword
open(LATE, ">>", $t) or die;
print LATE "more\n";
close LATE;
open($in, "<", $t) or die;
my @lines = <$in>;
close $in;
print "lines: ", scalar(@lines), " last=$lines[-1]";

# printf to a bareword handle writes to the file, not STDOUT
open(PF, ">", $t) or die;
printf PF "%s=%d\n", "k", 9;
printf(PF "%s\n", "paren");
close PF;
open($in, "<", $t) or die;
print "printf-file: ", join("", <$in>);
close $in;

# lexical and STD handles unaffected
open(my $lex, ">", $t) or die;
print $lex "lex";
printf $lex "-%d", 5;
close $lex;
open($in, "<", $t) or die;
print "lex: ", <$in>, "\n";
close $in;
printf STDOUT "%s\n", "stdout-ok";
unlink $t;
