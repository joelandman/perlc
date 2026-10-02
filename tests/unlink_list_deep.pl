# Deep test for D187: `unlink @names;` / `unlink LIST` -- any
# list-producing argument, not just a bare scalar filename -- silently
# deleted NOTHING. `case NK::UnlinkFunc`'s codegen pushed every
# argument via plain emitExpr(), which on an array-variable-shaped
# node doesn't yield the flattened filename list perl_unlink_files()
# needs, just some other (wrong) value -- so the underlying unlink(2)
# calls never happened at all, with no error of any kind. Found via a
# repo-root file cleanup bug in this project's own test suite: a test
# script's own `unlink @names;` cleanup line (mirroring this exact
# shape) left its temp files behind in the working directory after a
# full test-all run.

my @names = ('unlink_list_deep_a.tmp', 'unlink_list_deep_b.tmp',
             'unlink_list_deep_c.tmp');
for my $n (@names) {
    open(my $fh, '>', $n) or die "open $n: $!";
    close $fh;
}
print "before=", join(",", map { -e $_ ? "yes" : "no" } @names), "\n";
unlink @names;
print "after=", join(",", map { -e $_ ? "yes" : "no" } @names), "\n";

# regression: multi-argument scalar-list form (no array variable)
open(my $f1, '>', 'unlink_list_deep_x.tmp') or die;
close $f1;
open(my $f2, '>', 'unlink_list_deep_y.tmp') or die;
close $f2;
unlink 'unlink_list_deep_x.tmp', 'unlink_list_deep_y.tmp';
print "multiscalar=", join(",", map { -e $_ ? "yes" : "no" }
    ('unlink_list_deep_x.tmp', 'unlink_list_deep_y.tmp')), "\n";

# regression: single bare-scalar form
open(my $f3, '>', 'unlink_list_deep_z.tmp') or die;
close $f3;
unlink 'unlink_list_deep_z.tmp';
print "single=", (-e 'unlink_list_deep_z.tmp' ? "yes" : "no"), "\n";

print "done\n";
