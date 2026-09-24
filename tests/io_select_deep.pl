use IO::Select;
use SelectSaver;
my ($R, $W);
pipe($R, $W);
my $s = IO::Select->new;
$s->add($R);
print "count=", $s->count, "\n";
print $W "xy";
close $W;
my @r = $s->can_read(1);
print "nread=", scalar(@r), "\n";
my $path = "/tmp/perlc_selectsaver.txt";
open my $fh, ">", $path;
{
    my $ss = SelectSaver->new($fh);
    print "IN";
}
close $fh;
open my $in, "<", $path;
my $got = <$in>;
close $in;
unlink $path;
print "saved=[$got]\n";
print "done\n";
