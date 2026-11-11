use strict;
use Encode;

sub show {
    my ($label, $e) = @_;
    if (!defined $e) { print "$label UNDEF\n"; return; }
    print "$label ref=", ref($e), " name=", $e->name, "\n";
}

show("ascii", Encode->getEncoding("ascii"));
show("ASCII", Encode->getEncoding("ASCII"));
show("us", Encode->getEncoding("us-ascii"));
show("utf8", Encode->getEncoding("utf8"));
show("UTF8", Encode->getEncoding("UTF8"));
show("utf-8", Encode->getEncoding("utf-8"));
show("strict", Encode->getEncoding("utf-8-strict"));
show("latin", Encode->getEncoding("latin1"));
show("ws", Encode->getEncoding("  ISO-8859-1\t"));
show("iso", Encode->getEncoding("iso8859-1"));
show("u16", Encode->getEncoding("UTF-16"));
show("u16le", Encode->getEncoding("utf-16le"));
show("u32be", Encode->getEncoding("UTF-32BE"));
show("blank", Encode->getEncoding("   "));
show("empty", Encode->getEncoding(""));
show("undef", Encode->getEncoding(undef));
show("nope", Encode->getEncoding("no-such-enc"));
show("fn1", Encode::getEncoding("ascii"));
show("fn2", Encode::getEncoding("Encode", "UTF-8"));

my $f = Encode::find_encoding("ASCII");
print "find=", (defined $f ? $f->name : "UNDEF"), " ref=", (defined $f ? ref($f) : ""), "\n";
my $miss = Encode::find_encoding("no-such-enc");
print "findmiss=", defined($miss) ? 1 : 0, "\n";

my $e = Encode->getEncoding("utf8");
print "round=", $e->decode($e->encode("Hi")), "\n";
my $s = Encode->getEncoding("utf-8");
print "strictround=", $s->decode($s->encode("Hi")), "\n";

my $bad = "nope";
eval { Encode->getEncoding($bad) || die "Unknown encoding '$bad'" };
print "die=$@";

# piconv's `push @ARGV, \*STDIN` then `while (<$ifh>)`. A named glob
# stands in for STDIN so the harness does not have to feed a pipe.
my $path = "/tmp/perlc_d260_in.txt";
open my $w, ">", $path or die "write $path: $!";
print $w "abc";
close $w;
open IN260, "<", $path or die "open $path: $!";
my @argv;
push @argv, \*IN260;
my $argv = $argv[0];
my $ifh = ref($argv) ? $argv : undef;
die "noref\n" unless defined $ifh;
binmode $ifh;
my $got = "";
while (<$ifh>) {
    Encode::from_to($_, "ascii", "ascii");
    $got .= $_;
}
close IN260;
unlink $path;
print "globread=[$got]\n";
print "done\n";
