use Text::CSV_PP;
my $c = Text::CSV_PP->new({quote_char => undef});
$c->parse("a,b");
print "nq=[", join("|", $c->fields), "]\n";
print "done\n";
