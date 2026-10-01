use POSIX qw(:errno_h :sys_wait_h);
print EINVAL, "\n";
print WNOHANG, "\n";
system("true");
print WIFEXITED($?) ? "ok\n" : "FAIL\n";
