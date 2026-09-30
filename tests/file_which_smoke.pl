use File::Which qw(which);
my $p = which("perl");
print defined($p) ? "found\n" : "notfound\n";
print which("totally_bogus_cmd_xyz_123") // "undef", "\n";
