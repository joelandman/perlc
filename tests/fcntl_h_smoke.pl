use POSIX qw(:fcntl_h);
print O_CREAT(), "\n";
my @st = stat("/etc/passwd");
print S_ISREG($st[2]) ? "reg\n" : "notreg\n";
