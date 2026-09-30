# D155 smoke: "\x{..}" / "\xH" / octal escapes, heredoc escapes (were not
# processed at all), and NUL-safe split // and join.
sub hx { join " ", map { sprintf "%02x", ord($_) } split //, $_[0] }
print hx("\x{41}\x7\101\0"), "\n";
print length("\x{263A}"), "\n";
my $v = "V";
print <<"EOT";
tab:\there \$v=$v hex:\x{42}
EOT
print length(join("", "a\0", "b")), "\n";
