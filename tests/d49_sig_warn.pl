#!/usr/bin/perl
# Deep: D49 — %SIG __WARN__ / __DIE__
print "=== warn handler ===\n";
{
    my @w;
    $SIG{__WARN__} = sub { push @w, $_[0] };
    warn "hello";
    warn "world\n";
    print "n=", scalar(@w), "\n";
    print "0=$w[0]";
    print "1=$w[1]";
}

print "=== die handler in eval ===\n";
{
    eval {
        local $SIG{__DIE__} = sub { print "diehandler:$_[0]" };
        die "boom";
    };
    print "after eval\n";
}

print "=== local restore warn ===\n";
{
    my @outer;
    my @inner;
    $SIG{__WARN__} = sub { push @outer, "O" };
    {
        local $SIG{__WARN__} = sub { push @inner, "I" };
        warn "x";
    }
    warn "y";
    print "inner=", join(",", @inner), " outer=", join(",", @outer), "\n";
}

print "=== no handler does not crash ===\n";
{
    $SIG{__WARN__} = undef;
    # warn goes to stderr — just ensure it returns
    warn "to-stderr\n";
    print "survived\n";
}

print "d49_sig_warn_done\n";
