# Deep test for D176: a QUOTED heredoc delimiter containing
# non-alnum/underscore characters (<<'!END!', <<"TAG WITH SPACES")
# was a hard "unexpected token '<<'" parse error -- the delimiter
# scan enforced the stricter alnum/underscore-only rule even for the
# quoted form, when real Perl allows ANY character up to the closing
# quote there. Found recurring across two separate real-script
# surveys, in a real /usr/lib/.../Config_heavy.pl -- part of Perl's
# OWN generated Config.pm support files, not an obscure third-party
# script: `our $summary = <<'!END!';`.
my $punct = <<'!END!';
line one
line two
!END!
print "punct=[$punct]";

my $spaces = <<"TAG WITH SPACES";
double quoted with spaces in the tag
TAG WITH SPACES
print "spaces=[$spaces]";

# indented form (<<~) with a punctuation-only quoted delimiter
my $indented = <<~'!IND!';
    first
    second
    !IND!
print "indented=[$indented]";

# interpolation still works inside a punctuation-delimited heredoc
my $name = "World";
my $interp = <<"!HI!";
Hello, $name!
!HI!
print "interp=[$interp]";

# single-quoted punctuation delimiter does NOT interpolate (matching
# real Perl's ordinary single-quote-string convention)
my $noninterp = <<'!HI!';
Hello, $name!
!HI!
print "noninterp=[$noninterp]";

# regression: plain bareword-identifier delimiters (quoted and
# unquoted) are completely unaffected
my $bare = <<EOT;
plain bareword
EOT
print "bare=[$bare]";
my $qbare = <<'EOT';
quoted bareword
EOT
print "qbare=[$qbare]";

print "done\n";
