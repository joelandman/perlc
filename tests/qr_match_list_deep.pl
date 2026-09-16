my $s = "hello-42";
my @m = ($s =~ /(\w+)-(\d+)/);
print "m: @m\n";
my @m2 = ($s =~ /(\d+)/);
print "m2: @m2\n";
my @m3 = ($s =~ /nope/);
print "m3: ", scalar(@m3), "\n";
my @m4 = ($s =~ /(\w+)/);
print "m4: @m4\n";
my $re = qr/(\d+)/;
my @m5 = ($s =~ $re);
print "m5: @m5\n";
# $1 side effects from a list-context match:
my @m6 = ($s =~ /(\w+)-(\d+)/);
print "d1: $1 $2\n";
# /g list form still loops (regression guard):
my @all = ($s =~ /(\w+)/g);
print "all: @all\n";
# scalar context unchanged:
print "sc: ", ($s =~ /(\d+)/) ? 1 : 0, "\n";
print "deep_done\n";
