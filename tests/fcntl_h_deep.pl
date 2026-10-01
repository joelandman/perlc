# Deep test for D165: POSIX's :fcntl_h import tag wasn't recognized
# at all (found via a real /usr/bin/dpkg-genbuildinfo script:
# `use POSIX qw(:fcntl_h :locale_h strftime);`). Covers the tag's
# plain constants (some new: O_ACCMODE/O_NOCTTY; most already existed
# under Fcntl's own tags and are just now also reachable via POSIX's
# :fcntl_h) and its real functions: the S_IS* mode-testing predicates
# (S_ISREG/S_ISDIR/etc. -- real functions, not constants, despite
# similar S_IS*-vs-S_IxUSR naming) and creat().
use POSIX qw(:fcntl_h);

print "O_CREAT=", O_CREAT(), "\n";
print "O_ACCMODE=", O_ACCMODE(), "\n";
print "O_NOCTTY=", O_NOCTTY(), "\n";
print "O_RDONLY=", O_RDONLY(), "\n";
print "O_WRONLY=", O_WRONLY(), "\n";
print "O_RDWR=", O_RDWR(), "\n";
print "O_APPEND=", O_APPEND(), "\n";
print "O_EXCL=", O_EXCL(), "\n";
print "O_TRUNC=", O_TRUNC(), "\n";
print "FD_CLOEXEC=", FD_CLOEXEC(), "\n";
print "SEEK_SET=", SEEK_SET(), "\n";
print "SEEK_CUR=", SEEK_CUR(), "\n";
print "SEEK_END=", SEEK_END(), "\n";
print "S_IRUSR=", S_IRUSR(), "\n";
print "S_IWUSR=", S_IWUSR(), "\n";

my @st = stat("/etc/passwd");
print "S_ISREG_file=", (S_ISREG($st[2]) ? "1" : "0"), "\n";
print "S_ISDIR_file=", (S_ISDIR($st[2]) ? "1" : "0"), "\n";
print "S_ISCHR_file=", (S_ISCHR($st[2]) ? "1" : "0"), "\n";
print "S_ISBLK_file=", (S_ISBLK($st[2]) ? "1" : "0"), "\n";
print "S_ISFIFO_file=", (S_ISFIFO($st[2]) ? "1" : "0"), "\n";

my @dst = stat("/etc");
print "S_ISDIR_dir=", (S_ISDIR($dst[2]) ? "1" : "0"), "\n";
print "S_ISREG_dir=", (S_ISREG($dst[2]) ? "1" : "0"), "\n";

# creat(): real function (not a constant), creates the file
my $path = "/tmp/perlc_d165_creat_test.$$";
my $fd = creat($path, 0644);
print "creat_fd_ok=", (($fd >= 0) ? "1" : "0"), "\n";
print "creat_file_exists=", ((-e $path) ? "1" : "0"), "\n";
unlink $path;

# D165 companion fix found in the same investigation: heredoc
# delimiters tolerate whitespace before a QUOTED delimiter
# (`<< "EOT"`, `<<~ "EOT"`) -- real Perl allows this but perlc's
# lexer required the quote to immediately follow `<<`/`<<~`. Found
# via a real /usr/bin/linux-version script
# (`print $fh (<< "EOT");`).
print <<   "QQ";
quoted with spaces before it
QQ
print <<~ "RR";
    indented quoted with space before it
    RR
