use Time::Piece;
# gmtime/strptime only, deliberately avoiding localtime — TZ-independent,
# so this test is reproducible on any machine regardless of host timezone.
my $t = gmtime(0);
print "str=[$t]\n";
print "ymd=", $t->ymd, " hms=", $t->hms, "\n";
print "epoch=", $t->epoch, "\n";
print "class=", ref($t), "\n";
print "done\n";
