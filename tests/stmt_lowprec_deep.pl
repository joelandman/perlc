# D150 deep: low-precedence or/and/xor after non-expression statements.
# The statement's arguments stop at the operator; the statement's own value
# (push/unshift: new length, local/state: the assigned value, warn: 1) is
# the LHS; return/last/next/die never reach the RHS.
use strict;
use feature 'state';
local $SIG{__WARN__} = sub { };

our ($g, @ga, %gh, @gl);
our %cfg = (k => 'orig');
our @arr = (10, 20, 30);
my $n = 0;
sub hit { $n++; print "hit($_[0])\n"; return $_[0] }

# ---- push / unshift: value is the new length, args are list-op precedence
my @a;
push @a, 0 and hit("push0");                 # pushes 0, length 1 -> true
push @a, 0, 0 or hit("never");
unshift @a, "" and hit("unshift-empty-str");
print "a=[", join(",", @a), "] len=", scalar(@a), "\n";
my @e;
push @e, () or hit("push-nothing");          # length 0 -> false
unshift @e, () or hit("unshift-nothing");
my $ar = [];
push @$ar, 5 and hit("push-ref");
my %h;
push @{ $h{list} }, 1, 2 and hit("push-autoviv");
print "h{list}=[@{$h{list}}]\n";
push(@a, 9) and hit("push-paren");
push @a, map { $_ * 2 } 1 .. 3 and hit("push-map");   # map's list absorbs
print "a=[", join(",", @a), "]\n";

# ---- local: value is the assigned value, dynamic scope preserved
sub show { print "g=", (defined $g ? $g : "u"), " cfg=$cfg{k} arr1=$arr[1]\n" }
sub L {
    local $g = 0 or hit("local-0");
    local $cfg{k} = "" or hit("local-hash-elem");
    local $arr[1] = 7 and hit("local-arr-elem");
    local @gl = (1) and hit("local-array");
    local %gh = () or hit("local-hash-empty");
    show();
}
L();
show();
sub LU { local $_ = "x" and hit("local-underscore=$_") }
LU();

# ---- state: value is the variable, initialized once
sub S { state $s = 0 or hit("state-init"); return ++$s }
print "S=", S(), "\n";
print "S=", S(), "\n";

# ---- warn: always true
warn "w\n" and hit("warn-and");
warn "w\n" or hit("never");

# ---- return / die / next / last: RHS unreachable
sub R1 { return 0 or hit("never") }
sub R2 { return (1, 2) and hit("never") }
sub R3 { my $x = shift; return not $x }
print "R1=", R1(), " R2=", join("+", R2()), " R3=", (R3(0) ? 1 : 0), (R3(1) ? 1 : 0), "\n";
eval { die "boom\n" or hit("never") };
print "die: $@";
for my $i (1 .. 3) {
    next and hit("never") if $i == 1;
    last or hit("never") if $i == 3;
    print "loop i=$i\n";
}

# ---- chains and modifiers
push @a, 1 and hit("chain1") and hit("chain2") or hit("never");
push @e, () or hit("fallback") and hit("after-fallback");
push @a, 2 and hit("modifier-true") if 1;
push @a, 3 and hit("never") if 0;
push @a, 4 xor hit("xor-rhs");
print "n=$n a-len=", scalar(@a), "\n";

# ---- other list operators without parens also stop at and/or
my @r = reverse 1, 2 or hit("never"); print "r=@r\n";
my @m = map { $_ + 1 } 1, 2 and hit("map-and"); print "m=@m\n";
my @so = sort { $b <=> $a } 1, 3, 2 or hit("never"); print "so=@so\n";
my @gr = grep { $_ } 0, 1, 2 and hit("grep-and"); print "gr=@gr\n";
my $str = sprintf "%s-%s", "a", "b" or hit("never"); print "str=$str\n";
sub cnt { return scalar @_ }
my $k = 0;
cnt 1, 2 and $k = 7;
print "k=$k\n";
my $j = join ",", 1, 2 or hit("never"); print "j=$j\n";
print "n=$n\n";

# ---- `FUNC ARGS or die` idioms: the failure branch must run
my $bad = "/nonexistent/perlc/dir";
my $r = "";
open my $fh, "<", $bad or $r .= "open ";
open(my $fh2, "<", $bad) or $r .= "open-paren ";
open BADFH, "<$bad" or $r .= "open-bare ";
mkdir $bad or $r .= "mkdir ";
mkdir $bad, 0755 or $r .= "mkdir-mode ";
chdir $bad or $r .= "chdir ";
unlink $bad or $r .= "unlink ";
rmdir $bad or $r .= "rmdir ";
opendir my $dh, $bad or $r .= "opendir ";
rename $bad, "$bad.x" or $r .= "rename ";
chmod 0644, $bad or $r .= "chmod ";
print "failed: $r\n";
my $ok = "";
my $tmp = "/tmp/perlc_stmt_lowprec_$$.txt";
open my $out, ">", $tmp or die "cannot write $tmp";
print $out "DATA-LINE\n" or die;
close $out or die;
open my $in, "<", $tmp or die "cannot open $tmp";
binmode $in or $ok .= "never ";
read $in, my $buf, 4 or $ok .= "never ";
seek $in, 0, 0 or $ok .= "never ";
close $in or $ok .= "never ";
print "ok: [$ok] buf=[$buf]\n";
unlink $tmp or die "unlink";

# ---- declarations: (my $x = INIT) or RHS — the assignment is the LHS
sub zero { return 0 } sub nothing { return () } sub pair { return (0, 0) }
my $dr = "";
my $d1 = zero() or $dr .= "scalar0 ";
my @d2 = reverse 1, 2 or $dr .= "never ";
my @d3 = nothing() or $dr .= "arr-empty ";
my %d4 = (k => 0) and $dr .= "hash-full ";
my ($d5) = zero() and $dr .= "list-one-zero ";
my ($d6, $d7) = pair() and $dr .= "list-two ";
my ($d8) = nothing() or $dr .= "list-empty ";
print "decl: $dr| d2=@d2\n";
my (@x, %y);
my $cnt = (@x = (5, 6, 7)); my $hc = (%y = (a => 1, b => 2, a => 3));
print "counts: $cnt $hc keys=", scalar(keys %y), "\n";
@x = () or print "empty-array-assign\n";
my @srt = sort { $a <=> $b } 3, 1, 2; print "srt=@srt\n";
eval { die }; print "bare-die: ", ($@ =~ /^Died at/ ? "ok" : "bad:$@"), "\n";
