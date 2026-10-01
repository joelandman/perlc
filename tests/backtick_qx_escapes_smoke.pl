my $name = "world";
my $r = `echo "\$name hello"`;
print $r;
my $r2 = qx(echo plain);
print $r2;
