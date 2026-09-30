# D155 deep: string escapes (\x{..}, \xH, bare \x, octal), q{} vs qq{},
# heredoc escape processing, wide characters, NUL-safe split/join.
binmode STDOUT, ':utf8';
sub h { join " ", map { sprintf "%02x", ord($_) } split //, $_[0] }
my $v = "V";
print h("\x41\x7\x{42}\x{0043}\x"), "\n";
print h("\101\60\0\012\1a"), "\n";
print h(qq{a\tb\x{41}\101}), "\n";
print q{a\nb\\c\}d}, "\n";
print qq{cost \$5 \@x $v}, "\n";
print "cost \$5 \@x $v\n";
my $w = "\x{263A}";
print length($w), " ", length("a\x{263A}b"), " ", length("x${v}\x{263A}"), " ", ord($w), "\n";
print "smile: $w and \x{2603}!\n";
print length("\x{e9}"), " ", ord("\x{e9}"), "\n";
print "tab\there", "\n";
my $re = "a\x{41}b"; print(($re =~ /aAb/) ? "re ok\n" : "re bad\n");
my $here = <<"EOT";
h\x{41}\101 $v
EOT
print $here;
print 'single \x41 \n', "\n";
# NUL-safe split / join, and character-wise split of wide strings
my $bin = "a\0b\0\0c";
my @f = split //, $bin;
print scalar(@f), " ", join(",", map { ord($_) } @f), "\n";
my @g = split /\0/, $bin;
print scalar(@g), " [", join("|", @g), "]\n";
my $j = join("\0", "x", "y");
print length($j), " ", ord(substr($j, 1, 1)), "\n";
my @w = split //, "\x{263A}a\x{2603}";
print scalar(@w), " ", length($w[0]), "\n";
my @parts = split(/(b)/, "a\0bc");
print length(join("|", @parts)), "\n";
my @trail = split //, "ab\0";
print scalar(@trail), "\n";
print <<'RAW';
raw \t \x41 $v stays literal
RAW
print <<PLAIN;
plain\t\x{43}\$v
PLAIN
