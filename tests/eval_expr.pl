#!/usr/bin/perl
# Deep: string eval — return value, $@, nested, subs, dynamic.
use strict;
use warnings;
# Comma lists / `1; 2; 3` warn "useless use of a constant in void context"
# under real perl; perlc has no void-context diagnostic. Suppress so the
# harness compares the eval results, not the warning stream.
no warnings 'void';

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

check('empty', !defined(eval("")));
check('zero', eval("0") eq "0" || eval("0") == 0);
check('last_stmt', eval("1; 2; 3") == 3);
check('success_clears_at', do { eval "1+1"; $@ eq "" });
check('block_still_works', eval { 7 } == 7);

{
    my $a = 3;
    my $b = 4;
    check('two_lexicals', eval('$a * $b') == 12);
}

{
    eval 'sub evaled_add { $_[0] + $_[1] }';
    check('eval_defines_sub', evaled_add(10, 5) == 15);
}

{
    my $src = 'my $n = 0; $n += $_ for 1..4; $n';
    check('dyn_loop', eval($src) == 10);
}

check('list_ctx', join(",", eval("(1,2,3)")) eq "1,2,3");
check('list_assign', do { my @a = eval("(9,8,7)"); join(":", @a) eq "9:8:7" });
check('scalar_comma', eval("(1,2,3)") == 3);
{
    my $src = '(4,5,6)';
    check('dyn_list', join(",", eval($src)) eq "4,5,6");
}
check('wa_scalar', eval('wantarray ? "L" : "S"') eq "S");
check('wa_list', join(",", eval('wantarray ? (1,2) : 9')) eq "1,2");

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "eval_expr_done\n";
