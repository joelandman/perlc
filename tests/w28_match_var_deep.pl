my $s = "xaaay";
my $p = "a+";
if ($s =~ $p) { print "MATCH\n"; } else { print "NOMATCH\n"; }
my $n = ($s =~ $p);
print "n=$n\n";
if ($s !~ $p) { print "NO\n"; } else { print "YES\n"; }
# pattern from hash elem
my %h = (p => "a+");
print "h=", ($s =~ $h{p} ? "M" : "N"), "\n";
# pattern from concat
print "c=", ($s =~ ("a" . "+") ? "M" : "N"), "\n";
# pattern from function call
sub getpat { return "a+"; }
print "f=", ($s =~ getpat() ? "M" : "N"), "\n";
# captures populate from the variable pattern
if ($s =~ $p) { print "cap=[$1]\n"; }
# scalar context count
my $c = () = ($s =~ $p);
print "listctx=$c\n";
# string that is not a regex yet (metachars are compiled)
my $q = "a{1,2}";
print "q=", ($s =~ $q ? "M" : "N"), "\n";