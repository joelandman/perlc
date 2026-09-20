use JSON::PP;
my $h = { a => 1, b => [1,2,3], c => "x" };
my $j = JSON::PP->new->canonical->encode($h);
print "$j\n";
my $back = decode_json($j);
print "a=$back->{a} b=@{$back->{b}} c=$back->{c}\n";
print "done\n";
