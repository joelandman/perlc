# ${name} ends the scalar. A following character class stays literal.
my $termpat = "foo";
print "${termpat}[:|]\n";
my @arr = (10, 20);
print "$arr[1]\n";
