#!/usr/bin/env perl
# Deep test: File::Spec::Functions — explicit import list covering the
# @EXPORT_OK names (splitpath/splitdir/catpath/abs2rel/rel2abs/devnull/
# tmpdir), fully-qualified File::Spec::Functions:: calls, and the
# deterministic path ops. tmpdir/rel2abs outputs are normalized by using
# fixed bases / comparing against $ENV{TMPDIR}-driven /tmp.
use strict;
use warnings;
use File::Spec::Functions qw(splitpath splitdir catpath abs2rel rel2abs devnull tmpdir catfile catdir canonpath);

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my @p = splitpath("/a/b/c.txt");
check("splitpath_dir", $p[1] eq "/a/b/");
check("splitpath_file", $p[2] eq "c.txt");
check("splitpath_vol", $p[0] eq "");

my @d = splitdir("/a/b//c/");
check("splitdir_count", scalar(@d) == 6);
check("splitdir_empty1", $d[0] eq "");
check("splitdir_empty3", $d[3] eq "");
check("splitdir_empty5", $d[5] eq "");

check("catpath", catpath("", "/a/b/", "c.txt") eq "/a/b/c.txt");
check("catpath_noslash", catpath("", "a/b", "c.txt") eq "a/b/c.txt");
check("catpath_onlyfile", catpath("", "", "c.txt") eq "c.txt");

check("abs2rel", abs2rel("/a/b", "/a") eq "b");
check("abs2rel_up", abs2rel("/a/b/c", "/a/x") eq "../b/c");
check("abs2rel_same", abs2rel("/a", "/a") eq ".");
check("abs2rel_root", abs2rel("/a", "/") eq "a");
check("rel2abs_base", rel2abs("a/b", "/base") eq "/base/a/b");
check("rel2abs_empty", rel2abs("", "/base") eq "/base");
check("rel2abs_dots", rel2abs("a/../b", "/x") eq "/x/a/../b");

check("devnull", devnull() eq "/dev/null");
check("tmpdir", tmpdir() eq "/tmp");

check("catfile", catfile("a", "", "b") eq "a/b");
check("catdir_root", catdir("/", "tmp") eq "/tmp");
check("catdir_trail", catdir("a", "b", "") eq "a/b");
check("canonpath_dots", canonpath("/a/../b") eq "/a/../b");
check("canonpath_slash", canonpath("/a//b/./c") eq "/a/b/c");
check("canonpath_root", canonpath("/..") eq "/");
check("canonpath_cwd", canonpath("./") eq ".");

# fully-qualified form
check("qualified", File::Spec::Functions::catfile("a","b") eq "a/b");

check("smoke_done", 1);
print scalar(@failures), " failures\n";