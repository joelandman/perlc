#!/usr/bin/perl
# D269: multi-item foreach flattens lists, aliases real arrays, and a
# still-negative store dies instead of writing off the front of the array.
use strict;
use warnings;

sub build_index {
    my ($to_index) = @_;
    my @out;
    my $level  = 0;
    my $indent = -1;
    my $id     = ' id="index"';
    for my $h (@{ $to_index }, [0]) {
        my $target_level = $h->[0];
        if ($level == $target_level) {
            $out[-1] .= '</li>';
        } elsif ($level > $target_level) {
            $out[-1] .= '</li>' if $out[-1] =~ /^\s+<li>/;
            while ($level > $target_level) {
                --$level;
                push @out, ('  ' x --$indent) . '</li>' if @out && $out[-1] =~ m{^\s+<\/ul};
                push @out, ('  ' x --$indent) . '</ul>';
            }
            push @out, ('  ' x --$indent) . '</li>' if $level;
        } else {
            while ($level < $target_level) {
                ++$level;
                push @out, ('  ' x ++$indent) . '<li>' if @out && $out[-1] =~ /^\s*<ul/;
                push @out, ('  ' x ++$indent) . "<ul$id>";
                $id = '';
            }
            ++$indent;
        }
        next unless $level;
        my $space = '  ' x $indent;
        push @out, sprintf '%s<li><a href="#%s">%s</a>',
            $space, $h->[1], $h->[2];
    }
    return join "\n", @out;
}

my $page = build_index([[1, "T", "T"], [2, "S", "S"]]);
print "PAGE<<$page>>\n";

my $one = [1, "keep"];
my $lv = $one->[0];
print "lv=$lv cmp=", ($lv == 1 ? "eq" : "ne"), "\n";

my $s = "";
for my $x ((1, 2), 3) { $s .= $x; }
print "s=$s\n";

my ($p, $q) = (4, 5);
for my $x ($p, $q) { $x++; }
print "pq=$p$q\n";

my @m = ("m");
for my $x (@m, "z") { $x .= "!" if $x eq "m"; }
print "m=$m[0]\n";

my @e;
eval { $e[-1] .= "</li>"; };
print "empty=$@";
my @one = (1);
eval { $one[-3] = 9; };
print "far=$@";
print "onen=", scalar(@one), "\n";
my @ok = ("x");
$ok[-1] .= "y";
print "ok0=$ok[0]\n";
