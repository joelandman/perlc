#!/usr/bin/perl
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $dbfile = "/tmp/perlc_dbi_$$.sqlite";
unlink $dbfile;

my $dbh = DBI->connect("dbi:SQLite:dbname=$dbfile", "", "");
check('dbi_connect_method', defined($dbh));

my $dbh2 = DBI::connect("dbi:SQLite:dbname=$dbfile", "", "");
check('dbi_connect_function', defined($dbh2));
$dbh2->disconnect() if defined($dbh2);

my $created = $dbh->do("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, qty INTEGER)");
check('dbi_do_create', defined($created));

my $ins = $dbh->prepare("INSERT INTO items (name, qty) VALUES (?, ?)");
check('dbi_prepare_insert', defined($ins));
check('dbi_execute_insert_1', defined($ins->execute("apple", 3)));
check('dbi_execute_insert_2', defined($ins->execute("pear", 5)));

my $sel = $dbh->prepare("SELECT name, qty FROM items ORDER BY id");
check('dbi_prepare_select', defined($sel));
check('dbi_execute_select', defined($sel->execute()));

my $row1 = $sel->fetchrow_arrayref();
check('dbi_fetchrow_arrayref_1_defined', defined($row1));
check('dbi_fetchrow_arrayref_1_values', $row1->[0] eq 'apple' && $row1->[1] == 3);

my $row2 = $sel->fetchrow_arrayref();
check('dbi_fetchrow_arrayref_2_defined', defined($row2));
check('dbi_fetchrow_arrayref_2_values', $row2->[0] eq 'pear' && $row2->[1] == 5);

my $sel_all = $dbh->prepare("SELECT name, qty FROM items ORDER BY id");
$sel_all->execute();
my $all = $sel_all->fetchall_arrayref();
check('dbi_fetchall_arrayref_defined', defined($all));
check('dbi_fetchall_arrayref_count', scalar(@$all) == 2);
check('dbi_fetchall_arrayref_values', $all->[0]->[0] eq 'apple' && $all->[1]->[1] == 5);

my $upd = $dbh->prepare("UPDATE items SET qty = qty + 1");
$upd->execute();
check('dbi_rows_update', $upd->rows() == 2);

my $bad = $dbh->prepare("SELECT nope FROM missing_table");
check('dbi_prepare_bad_undef', !defined($bad));
check('dbi_errstr_nonempty', length($dbh->errstr()) > 0);

check('dbi_disconnect', $dbh->disconnect() == 1);
unlink $dbfile;

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "dbi_sqlite_done\n";
