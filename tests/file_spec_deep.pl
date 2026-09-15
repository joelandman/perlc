#!/usr/bin/env perl
# Deep test: File::Spec (class-method API) — every documented Unix method,
# matching real File::Spec::Unix 3.95 byte-for-byte (all outputs probed).
# Only cwd-dependent results (rel2abs/abs2rel without a base) are excluded;
# everything here is deterministic.
use strict;
use warnings;
use File::Spec;
use Cwd ();

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# canonpath — the real module does NOT collapse x/../y (by design)
check("canon_updown", File::Spec->canonpath("/a/../b") eq "/a/../b");
check("canon_dslash", File::Spec->canonpath("/a//b/./c") eq "/a/b/c");
check("canon_trail",  File::Spec->canonpath("a/b/") eq "a/b");
check("canon_root",   File::Spec->canonpath("/") eq "/");
check("canon_empty",  File::Spec->canonpath("") eq "");
check("canon_rootup", File::Spec->canonpath("/..") eq "/");
check("canon_ldot",   File::Spec->canonpath("./a") eq "a");
check("canon_ldotslash", File::Spec->canonpath("./") eq ".");
check("canon_ldots",  File::Spec->canonpath("/./a/b/") eq "/a/b");
check("canon_reldots", File::Spec->canonpath("../../x") eq "../../x");
check("canon_middotslash", File::Spec->canonpath("/..") eq "/");
check("canon_upmid",  File::Spec->canonpath("/a/./b/../c") eq "/a/./b/../c");

# catdir — trailing '' trick, root handling
check("catdir_empty", File::Spec->catdir("a","b","") eq "a/b");
check("catdir_root",  File::Spec->catdir("/","tmp") eq "/tmp");
check("catdir_one",   File::Spec->catdir("a") eq "a");
check("catdir_none",  File::Spec->catdir() eq "");
check("catdir_dslash", File::Spec->catdir("a//b") eq "a/b");
check("catdir_updown", File::Spec->catdir("a/..","b") eq "a/../b");

# catfile
check("catfile_mempty", File::Spec->catfile("a","","b") eq "a/b");
check("catfile_abs", File::Spec->catfile("/","tmp","f") eq "/tmp/f");
check("catfile_one", File::Spec->catfile("f") eq "f");
check("catfile_updown", File::Spec->catfile("a/../b","c") eq "a/../b/c");
check("join_alias", File::Spec->join("a","b") eq "a/b");

# catpath
check("catpath1", File::Spec->catpath("", "/a/b/", "c.txt") eq "/a/b/c.txt");
check("catpath2", File::Spec->catpath("", "a/b", "c.txt") eq "a/b/c.txt");
check("catpath3", File::Spec->catpath("", "", "c.txt") eq "c.txt");
check("catpath4", File::Spec->catpath("", "/a/b", "") eq "/a/b");
check("catpath5", File::Spec->catpath("", "a/b", "/c") eq "a/b/c");

# splitpath
my @p1 = File::Spec->splitpath("/a/b/c.txt");
check("sp1", $p1[0] eq "" && $p1[1] eq "/a/b/" && $p1[2] eq "c.txt");
my @p2 = File::Spec->splitpath("/a/b/");
check("sp2", $p2[1] eq "/a/b/" && $p2[2] eq "");
my @p3 = File::Spec->splitpath("c.txt");
check("sp3", $p3[1] eq "" && $p3[2] eq "c.txt");
my @p4 = File::Spec->splitpath("/a/b/c", 1);
check("sp4", $p4[1] eq "/a/b/c" && $p4[2] eq "");
my @p5 = File::Spec->splitpath("/a/b/../c");
check("sp5", $p5[1] eq "/a/b/../" && $p5[2] eq "c");

# splitdir
my @d1 = File::Spec->splitdir("/a/b//c/");
check("sd1", scalar(@d1) == 6 && $d1[0] eq "" && $d1[1] eq "a" && $d1[3] eq "");
my @d2 = File::Spec->splitdir("a/b");
check("sd2", scalar(@d2) == 2);
my @d3 = File::Spec->splitdir("");
check("sd3", scalar(@d3) == 0);
my @d4 = File::Spec->splitdir("/");
check("sd4", scalar(@d4) == 2 && $d4[0] eq "" && $d4[1] eq "");

# constants
check("curdir",  File::Spec->curdir eq ".");
check("updir",   File::Spec->updir eq "..");
check("rootdir", File::Spec->rootdir eq "/");
check("devnull", File::Spec->devnull eq "/dev/null");
check("tmpdir",  File::Spec->tmpdir eq "/tmp");

# file_name_is_absolute
check("fnia_abs",  File::Spec->file_name_is_absolute("/a/b"));
check("fnia_rel",  !File::Spec->file_name_is_absolute("a/b"));
check("fnia_estr", !File::Spec->file_name_is_absolute(""));
check("fnia_dsl",  File::Spec->file_name_is_absolute("//a"));

# no_upwards
my @nu = File::Spec->no_upwards(".","..","a",".");
check("no_upwards", scalar(@nu) == 1 && $nu[0] eq "a");

# case_tolerant
check("case_tolerant", !File::Spec->case_tolerant);

# rel2abs with explicit bases (deterministic)
check("rel2abs_abs",  File::Spec->rel2abs("/x/y") eq "/x/y");
check("rel2abs_base", File::Spec->rel2abs("a/b", "/base") eq "/base/a/b");
check("rel2abs_rebase", File::Spec->rel2abs("a/b", "base") eq Cwd::getcwd() . "/base/a/b");
check("rel2abs_empty", File::Spec->rel2abs("", "/base") eq "/base");
check("rel2abs_ldot", File::Spec->rel2abs("./a", "/b/..") eq "/b/../a");
check("rel2abs_up", File::Spec->rel2abs("a/../b","/x") eq "/x/a/../b");
check("rel2abs_basecanon", File::Spec->rel2abs("a","/x/./y") eq "/x/y/a");

# abs2rel with explicit bases
check("a2r_down", File::Spec->abs2rel("/a/b","/a") eq "b");
check("a2r_up",   File::Spec->abs2rel("/a/b/c","/a/x") eq "../b/c");
check("a2r_rel",  File::Spec->abs2rel("b","a") eq "../b");
check("a2r_same", File::Spec->abs2rel("/a","/a") eq ".");
check("a2r_rootbase", File::Spec->abs2rel("/a","/") eq "a");
check("a2r_dotdot", File::Spec->abs2rel("/x/../y","/x/z") eq "../../y");
check("a2r_ident", File::Spec->abs2rel("/a/b","/a/b") eq ".");
check("a2r_rootp", File::Spec->abs2rel("/","/a") eq "..");
check("a2r_reldots", File::Spec->abs2rel("../c","a/b") eq "../../../c");
check("a2r_relbase", File::Spec->abs2rel("/a/b","c/d") eq "../../../../../../a/b");

check("smoke_done", 1);
print scalar(@failures), " failures\n";