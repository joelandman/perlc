# D84 smoke: % by zero should die
eval { my $z = 5 % 0; };
print $@ ? "died\n" : "no-die\n";
