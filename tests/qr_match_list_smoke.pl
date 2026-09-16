my $s = "hello-42";
my @m = ($s =~ /(\w+)-(\d+)/);
print "m: @m\n";
print scalar(@m), "\n";
print "smoke_done\n";
