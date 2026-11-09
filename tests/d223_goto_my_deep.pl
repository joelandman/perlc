#!/usr/bin/perl
# D223: goto that does not fire still runs the initializer; a loop that
# jumps to a later label and back stays dominated; `do { }` conditions
# and a second lexical after the label stay distinct.
use strict;
use warnings;
sub taken {
    if (1) {
        goto set_utf8 if 0;
        my $copy = "yes";
        set_utf8:
        print "$copy\n";
    }
}
sub looped {
    my $n = 0;
    AGAIN:
    goto DONE if $n >= 2;
    my $tick = $n;
    $n++;
    goto AGAIN;
    DONE:
    print "$n\n";
}
sub version_cond {
    goto set_1252 if do { no integer; "$]" < 5.006_000 };
    my $copy = "late";
    set_1252:
    print defined($copy) ? "$copy\n" : "skip\n";
}
sub shadow {
    my $encoding = "outer";
    if (1) {
        goto L if 1;
        my $encoding = "inner";
        L:
        print defined($encoding) ? "$encoding\n" : "undef-enc\n";
    }
    print "$encoding\n";
}
taken();
looped();
version_cond();
shadow();
