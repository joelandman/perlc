use feature "say";
#!/usr/bin/perl
use strict;
use warnings;

# ── 1. caller() ──────────────────────────────────────────────────────────────
sub inner {
    my @c = caller();
    return @c;
}

sub outer {
    return inner();
}

my @c = outer();
print "caller pkg:  $c[0]\n";   # main
print "caller file: ", ($c[1] =~ /\.pl$/ ? "ok" : "fail"), "\n";  # ok
print "caller line: ", ($c[2] > 0 ? "ok" : "fail"), "\n";         # ok

# caller(0) vs caller(1)
sub level1 {
    my @c0 = caller(0);
    print "c0_pkg: $c0[0]\n";   # main
}
level1();

# ── 2. local @array ──────────────────────────────────────────────────────────
our @G = (1, 2, 3);

sub show_G { join(",", @G) }

print show_G(), "\n";   # 1,2,3
{
    local @G = (7, 8, 9);
    print show_G(), "\n";   # 7,8,9
}
print show_G(), "\n";   # 1,2,3  (restored)

# local @ARGV
our @ARGV_copy;
{
    local @ARGV = ("--foo", "--bar");
    @ARGV_copy = @ARGV;
}
print "argv: @ARGV_copy\n";   # --foo --bar

# ── 3. local %hash ───────────────────────────────────────────────────────────
our %H = (a => 1, b => 2);

sub show_H { join(",", map { "$_=$H{$_}" } sort keys %H) }

print show_H(), "\n";   # a=1,b=2
{
    local %H = (x => 10, y => 20);
    print show_H(), "\n";   # x=10,y=20
}
print show_H(), "\n";   # a=1,b=2  (restored)

# local @arr / %hash scoped inside a sub (not just a bare block) — D3
sub localize_G { local @G = (7, 8, 9); return show_G(); }
print localize_G(), "\n";   # 7,8,9
print show_G(), "\n";       # 1,2,3  (restored after sub returns)

sub localize_H { local %H = (x => 10); return show_H(); }
print localize_H(), "\n";   # x=10
print show_H(), "\n";       # a=1,b=2  (restored after sub returns)

# local @arr / %hash restored even when unwound via die/eval
sub localize_and_die {
    local @G = (0, 0, 0);
    local %H = (z => 99);
    die "boom\n";
}
eval { localize_and_die(); };
print "caught: $@";
print show_G(), "\n";   # 1,2,3  (restored despite die)
print show_H(), "\n";   # a=1,b=2  (restored despite die)

# ── 4. AUTOLOAD ──────────────────────────────────────────────────────────────
package Magic;
our $AUTOLOAD;   # package-level declaration
sub new { bless {}, shift }
sub AUTOLOAD {
    my $name = $AUTOLOAD;
    $name =~ s/.*:://;
    return "AUTOLOAD:$name";
}
sub DESTROY {}   # prevent DESTROY triggering AUTOLOAD

package main;
my $m = Magic->new();
print $m->foo(), "\n";    # AUTOLOAD:foo
print $m->bar(1,2), "\n"; # AUTOLOAD:bar

# ── 5. pos() write ───────────────────────────────────────────────────────────
my $str = "aababc";
$str =~ /a/g;
print "pos1: ", pos($str), "\n";   # 1
pos($str) = 3;
$str =~ /a/g;
print "pos2: ", pos($str), "\n";   # 4  (found 'a' at offset 3)

# ── 6. Runtime require ───────────────────────────────────────────────────────
# Write a temp module and require it
{
    open my $fh, '>', '/tmp/perlc_test_mod.pm' or die;
    print $fh 'package PerlcTestMod; sub greet { "hello from required module" } 1;';
    close $fh;
}
require '/tmp/perlc_test_mod.pm';
print PerlcTestMod::greet(), "\n";   # hello from required module

print "done\n";
