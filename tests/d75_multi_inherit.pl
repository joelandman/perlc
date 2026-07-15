#!/usr/bin/perl
# In-depth test suite for D75: multiple-inheritance method resolution
# picked the wrong parent.
#
# Root cause: `perl_set_isa(child, parent)` (runtime.c) "updated if already
# registered" — i.e. it overwrote any existing entry for `child` in place
# instead of adding a new one. `our @ISA = ('B','C')` calls this once per
# @ISA element in @ISA's own order (codegen.cpp already did this
# correctly), so for a class with 2+ parents, only the LAST call's parent
# ever survived in the ISA table; `perl_find_method`/`perl_isa_check`
# (the ->method and ->isa()/->can() implementations) then only ever walked
# that single remembered parent as a plain linear chain, never a true
# multi-parent tree. `D->@ISA=('B','C'); D->new->hello;` therefore
# resolved to whatever "hello" C (or C's own ancestry) provided, silently
# ignoring B entirely — even though B was registered first and real Perl's
# default MRO should have tried B's entire subtree before ever considering
# C.
#
# Fixed by: making perl_set_isa always append a new (child,parent) entry;
# adding a perl_isa_direct_parents() helper that collects ALL of a class's
# registered parents in @ISA order; and rewriting perl_find_method (method
# dispatch), perl_isa_check (->isa()), and perl_dispatch_method_super
# (SUPER::) to do a genuine depth-first search over the full @ISA tree —
# checking a class itself, then recursively exhausting each direct
# parent's entire subtree in left-to-right order before trying the next
# parent — matching real Perl's actual default (non-C3) MRO exactly,
# including its well-known "diamond problem" quirk (Section 6 below).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original repro — DFS dispatch with SUPER:: ─────────────
package D75A;
sub hello     { return "A::hello"; }
sub only_in_a { return "only_a"; }

package D75B;
our @ISA = ('D75A');
sub hello     { my $self = shift; return "B::hello->" . $self->SUPER::hello(); }
sub only_in_b { return "only_b"; }

package D75C;
our @ISA = ('D75A');
sub hello     { return "C::hello"; }
sub only_in_c { return "only_c"; }

package D75D;
our @ISA = ('D75B', 'D75C');
sub new { return bless {}, shift; }

package main;

{
    my $d = D75D->new;
    check('dfs_dispatch_with_super', $d->hello eq "B::hello->A::hello");
}

# ── Section 2: a method defined only in the SECOND parent's subtree is
#    still found (confirms the second parent isn't just ignored) ─────────
{
    my $d = D75D->new;
    check('method_only_in_second_parent', $d->only_in_c eq "only_c");
    check('method_only_in_first_parent',  $d->only_in_b eq "only_b");
    check('method_in_shared_grandparent', $d->only_in_a eq "only_a");
}

# ── Section 3: ->isa() recognizes BOTH direct parents and the shared
#    grandparent, not just the first-registered one ────────────────────────
{
    my $d = D75D->new;
    check('isa_first_parent',  $d->isa('D75B'));
    check('isa_second_parent', $d->isa('D75C'));
    check('isa_grandparent',   $d->isa('D75A'));
    check('isa_self',          $d->isa('D75D'));
    check('isa_unrelated_false', !$d->isa('D75NoSuchClass'));
}

# ── Section 4: ->can() likewise searches the full @ISA tree ────────────────
{
    my $d = D75D->new;
    check('can_second_parent_method', defined($d->can('only_in_c')));
    check('can_first_parent_method',  defined($d->can('only_in_b')));
    check('can_nonexistent_false',    !defined($d->can('nonexistent_method')));
}

# ── Section 5: regression — plain single-inheritance linear chains
#    (3 levels deep) still resolve correctly ───────────────────────────────
package D75E;
sub e_method { return "E::e_method"; }
package D75F;
our @ISA = ('D75E');
package D75G;
our @ISA = ('D75F');
package main;
{
    my $g = bless {}, 'D75G';
    check('single_inheritance_regression', $g->e_method eq "E::e_method");
    check('single_inheritance_isa_regression', $g->isa('D75E'));
}

# ── Section 6: diamond inheritance — real Perl's default (non-C3) MRO
#    finds Top::greet via the Left branch (which has no override) BEFORE
#    ever trying the Right branch (which does have one) — this looks like
#    a "wrong" answer compared to C3 linearization, but it's real Perl's
#    actual, documented default behavior, and this fix must reproduce it
#    exactly, not "improve" on it ───────────────────────────────────────────
package D75Top;
sub greet { return "Top::greet"; }
package D75Left;
our @ISA = ('D75Top');
package D75Right;
our @ISA = ('D75Top');
sub greet { return "Right::greet"; }
package D75Bottom;
our @ISA = ('D75Left', 'D75Right');
package main;
{
    my $b = bless {}, 'D75Bottom';
    check('diamond_dfs_quirk', $b->greet eq "Top::greet");
}

# ── Section 7: three-parent @ISA (not just two) ─────────────────────────────
package D75X;
sub only_x { return "x"; }
package D75Y;
sub only_y { return "y"; }
package D75Z;
sub only_z { return "z"; }
package D75Triple;
our @ISA = ('D75X', 'D75Y', 'D75Z');
package main;
{
    my $t = bless {}, 'D75Triple';
    check('three_parent_first',  $t->only_x eq "x");
    check('three_parent_second', $t->only_y eq "y");
    check('three_parent_third',  $t->only_z eq "z");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d75_multi_inherit_done\n";
