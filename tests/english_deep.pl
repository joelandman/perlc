use English;
print "pid_eq=", ($PID == $$ ? 1 : 0), "\n";
print "proc_eq=", ($PROCESS_ID == $$ ? 1 : 0), "\n";
print "os=$OSNAME\n";
open my $fh, "<", "/no/such/english_file";
print "errno_eq=", ($OS_ERROR eq $ERRNO ? 1 : 0), "\n";
print "err_nonempty=", ($OS_ERROR ne "" ? 1 : 0), "\n";
print "done\n";
