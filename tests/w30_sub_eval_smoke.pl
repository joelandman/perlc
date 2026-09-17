my $nm = "MyConst";
eval "sub $nm() { 42 }";
print "c=", MyConst(), "\n";
print "smoke_done\n";
