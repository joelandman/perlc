use Time::Piece;
use Time::Seconds;
# gmtime/strptime only throughout — deliberately never `localtime` or a
# no-arg "now" call, so this test is reproducible on any machine
# regardless of host timezone or wall-clock time (the exact class of
# mistake logged as D138/File::Temp earlier this session).

my $t = gmtime(0);
print "str=[$t]\n";
print "cdate=[", $t->cdate, "]\n";
print "datetime=[", $t->datetime, "]\n";
print "ymd=[", $t->ymd, "]\n";
print "mdy=[", $t->mdy, "]\n";
print "dmy=[", $t->dmy, "]\n";
print "hms=[", $t->hms, "]\n";
print "monname=[", $t->monname, "]\n";
print "fullmonth=[", $t->fullmonth, "]\n";
print "wdayname=[", $t->wdayname, "]\n";
print "fullday=[", $t->fullday, "]\n";
print "mon=", $t->mon, " _mon=", $t->_mon, " year=", $t->year, " _year=", $t->_year, "\n";
print "wday=", $t->wday, " _wday=", $t->_wday, " yday=", $t->yday, "\n";
print "epoch=", $t->epoch, "\n";
print "strftime_default=[", $t->strftime, "]\n";
print "strftime_custom=[", $t->strftime("%Y/%m/%d"), "]\n";
print "is_leap=", $t->is_leap_year, "\n";
print "bool=", ($t ? 1 : 0), "\n";

# day/monname name-table override
my @names = qw(Su Mo Tu We Th Fr Sa);
print "wd_override=", $t->day(@names), "\n";

# strptime (always UTC per real Time::Piece)
my $g = Time::Piece->strptime("2024-03-15 13:45:30", "%Y-%m-%d %H:%M:%S");
print "strptime_epoch=", $g->epoch, " ymd=", $g->ymd, " hms=", $g->hms, "\n";

# Time::Piece->new
my $n = Time::Piece->new(1000000000);
print "new_epoch=", $n->epoch, " ymd=", $n->ymd, "\n";

# leap year boundary cases
print "leap2000=", Time::Piece->strptime("2000-01-01", "%Y-%m-%d")->is_leap_year, "\n";
print "leap1900=", Time::Piece->strptime("1900-01-01", "%Y-%m-%d")->is_leap_year, "\n";
print "leap2024=", Time::Piece->strptime("2024-01-01", "%Y-%m-%d")->is_leap_year, "\n";
print "leap2023=", Time::Piece->strptime("2023-01-01", "%Y-%m-%d")->is_leap_year, "\n";

# arithmetic: TP +/- N -> TP; TP - TP -> Time::Seconds; chained
my $t2 = $t + 500;
my $t3 = $t2 - 200;
print "t1=", $t->epoch, " t2=", $t2->epoch, " t3=", $t3->epoch, " t3_class=", ref($t3), "\n";
my $diff = $t2 - $t;
print "diff_class=", ref($diff), " diff_secs=", $diff->seconds, "\n";

# comparisons: <=>, ==, !=, <, sort
print "cmp=", ($t <=> $t2), " lt=", ($t < $t2 ? 1 : 0), "\n";
print "eq_same_epoch=", ($t == gmtime(0) ? 1 : 0), "\n";
print "ne_diff_epoch=", ($t != $t2 ? 1 : 0), "\n";

my @times = map { scalar gmtime($_) } (300, 100, 200);
my @sorted = sort { $a <=> $b } @times;
print "sorted=", join(",", map { $_->epoch } @sorted), "\n";

# a Time::Piece held in a file-scope var, read from inside a sub
my $captured = gmtime(12345);
sub show_epoch { return $captured->epoch; }
print "from_sub=", show_epoch(), "\n";

# Time::Seconds: pretty-printing across every unit boundary + negative
for my $s (0, 1, 59, 60, 61, 3600, 3661, 86400, 90061, -3661) {
    print "pretty($s)=", Time::Seconds->new($s)->pretty, "\n";
}
my $ts = Time::Seconds->new(90061);
print "ts_seconds=", $ts->seconds, " minutes=", $ts->minutes,
      " hours=", $ts->hours, " days=", $ts->days, "\n";
print "ts_str=[$ts]\n";
print "ts_num=", ($ts + 0), "\n";

# Time::Seconds ONE_* constants
print "ONE_MINUTE=", ONE_MINUTE, " ONE_HOUR=", ONE_HOUR, " ONE_DAY=", ONE_DAY, "\n";
print "ONE_WEEK=", ONE_WEEK, " ONE_MONTH=", ONE_MONTH, " ONE_YEAR=", ONE_YEAR, "\n";
print "ONE_FINANCIAL_MONTH=", ONE_FINANCIAL_MONTH,
      " LEAP_YEAR=", LEAP_YEAR, " NON_LEAP_YEAR=", NON_LEAP_YEAR, "\n";

print "done\n";
