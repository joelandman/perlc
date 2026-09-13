#!/usr/bin/perl
# Deep test for D104: `<<~IDENT` (Perl 5.26+ indented heredoc). Root
# cause: src/lexer.cpp's readHeredoc() never recognized the `~`
# variant at all (grepping "<<~" found zero hits) — only `<<IDENT`,
# `<<"IDENT"`, `<<'IDENT'` were handled. Fix: readHeredoc() now checks
# for a leading `~` right after the second `<`, and when present,
# matches the terminator line with its own leading whitespace
# stripped, remembers that stripped prefix, and removes it from every
# line of the body (real Perl strips the terminator line's own
# indentation from the whole body; a body line less indented than the
# terminator is a real-Perl fatal error — this fix is permissive there,
# stripping only as much of the prefix as a given line actually has).
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub multiline {
    my $x = <<~END;
        hello
        world
        END
    return $x;
}
check('basic_multiline', multiline() eq "hello\nworld\n");

# interpolating form
my $name = "Joe";
my $greeting = <<~"GREETING";
    Hi $name
    bye
    GREETING
check('interpolating', $greeting eq "Hi Joe\nbye\n");

# non-interpolating form
my $noninterp = <<~'RAW';
    literal $name here
    RAW
check('non_interpolating', $noninterp eq "literal \$name here\n");

# body indented MORE than the terminator: only the terminator's own
# (lesser) indentation is stripped, leaving residual leading spaces —
# real Perl requires every body line to be indented at least as much
# as the terminator (less is a fatal error), so this must stay on the
# "more-or-equal" side of that rule.
my $residual = <<~RESIDUAL;
      indented twice
  RESIDUAL
check('residual_indent_kept', $residual eq "    indented twice\n");

# bare (unquoted) form, same as <<IDENT but indented
my $bare = <<~BARE;
    plain text
    BARE
check('bare_form', $bare eq "plain text\n");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d104_indented_heredoc_done\n";
