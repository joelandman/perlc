use Text::CSV_PP;
my $csv = Text::CSV_PP->new;
$csv->parse("a,b,c");
print "fields=[", join("|", $csv->fields), "]\n";
$csv->combine("x", "y,z", "plain");
print "string=[", $csv->string, "]\n";
print "class=", ref($csv), "\n";
print "done\n";
