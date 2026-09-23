use IO::File;
use IO::Handle;
my $path = "/tmp/perlc_iofile_deep.txt";
unlink $path;
my $fh = IO::File->new($path, "w");
print "opened=", ($fh ? 1 : 0), "\n";
print "isa_handle=", ($fh->isa("IO::Handle") ? 1 : 0), "\n";
$fh->print("ab");
$fh->say("c");
$fh->seek(0, 0);
$fh->close;

my $r = IO::File->new($path, "r");
my $all = "";
while (my $ln = $r->getline) { $all .= $ln }
print "all=[$all]\n";
$r->seek(0, 0);
print "tell0=", $r->tell, "\n";
my $buf = "";
my $n = $r->read($buf, 2);
print "readn=$n buf=$buf\n";
print "fileno_ok=", ($r->fileno >= 0 ? 1 : 0), "\n";
print "opened_flag=", $r->opened, "\n";
$r->close;
print "opened_after=", $r->opened, "\n";

open my $plain, ">", $path;
$plain->autoflush(1);
print $plain "z";
close $plain;
open my $chk, "<", $path;
my $got = <$chk>;
close $chk;
print "plain=[$got]\n";
unlink $path;
print "done\n";
