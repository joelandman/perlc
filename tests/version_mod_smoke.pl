use version;
my $v = version->parse("1.2.3");
print "str=$v num=", $v->numify, "\n";
print "done\n";
