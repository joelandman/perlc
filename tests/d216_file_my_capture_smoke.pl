#!/usr/bin/perl
# D216: a file-scope `my` compiled via `do` (the --do-lib path) is visible
# from a named sub in that file, including through s/// interpolation.
use strict;
use warnings;

my $lib = 'd216_domy_smoke_lib.pl';
open my $fh, '>', $lib or die "Cannot create $lib: $!";
print $fh <<'END';
package D216;
my ($NBSP, $SHY);
$NBSP = "Z";
$SHY  = "-";
sub plain { return $NBSP . $SHY }
sub scrub {
    my ($text) = @_;
    if ($NBSP) { $text =~ s{$NBSP}{ }g; }
    if ($SHY)  { $text =~ s{$SHY}{}g; }
    return $text;
}
sub cmd_s {
    my ($text) = @_;
    $text =~ s{ \s }{$NBSP}xmsg;
    return $text;
}
sub set_nbsp { $NBSP = $_[0] }
print "during=", plain(), "\n";
1;
END
close $fh;

my $ret = do './d216_domy_smoke_lib.pl';
print "ret=", (defined $ret ? $ret : "UNDEF"), "\n";
print "err=", ($@ ? "ERR" : "ok"), "\n";
print "plain=[", D216::plain(), "]\n";
print "scrub=[", D216::scrub("aZb-c"), "]\n";
print "cmds=[", D216::cmd_s("a b"), "]\n";
D216::set_nbsp("Q");
print "after=[", D216::scrub("aQb"), "]\n";
print "cmds2=[", D216::cmd_s("a b"), "]\n";

unlink $lib;
