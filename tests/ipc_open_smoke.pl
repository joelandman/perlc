use IPC::Open2;
my ($r, $w);
open2($r, $w, "/bin/echo", "hello");
my $line = <$r>;
print "echo=$line";
close $r;
close $w;
print "done\n";
