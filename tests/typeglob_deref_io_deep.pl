# Deep test for *{EXPR}{IO} -- the deref-glob form of D177's bareword
# *NAME{IO} (e.g. *STDERR{IO}), on an arbitrary expression instead of
# a literal bareword name. Scoped out of D177 since no real script
# needed it; this codebase's typeglob model still has no general
# multi-slot glob representation, so only {IO} is recognized, and the
# inner expression is resolved to a usable filehandle value at
# runtime (a scalar-ref like `\*STDOUT` is unwrapped; an
# already-bare filehandle value, e.g. a lexical `open`ed handle,
# passes through unchanged).

sub usage {
    my $fh = shift;
    print $fh "via_glob_ref\n";
}

# \*STDOUT -- a ref to a bareword glob
usage(*{\*STDOUT}{IO});

# a lexical scalar holding a ref to a glob
my $gref = \*STDOUT;
usage(*{$gref}{IO});

# a real lexical filehandle opened with `open my $fh, ...`
open(my $fh, '>', '/tmp/typeglob_deref_io_deep_out.txt') or die "open: $!";
usage(*{$fh}{IO});
close $fh;
open(my $rd, '<', '/tmp/typeglob_deref_io_deep_out.txt') or die "open: $!";
print "readback=", <$rd>;
close $rd;
unlink '/tmp/typeglob_deref_io_deep_out.txt';

# regression: D177's bareword *NAME{IO} form stays unaffected
usage(*STDOUT{IO});
# regression: plain bareword *NAME (no {IO} at all) stays unaffected
usage(*STDOUT);

print "done\n";
