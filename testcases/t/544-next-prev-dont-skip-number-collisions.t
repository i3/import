#!perl
# vim:ts=4:sw=4:expandtab
#
# Please read the following documents before working on tests:
# • https://build.i3wm.org/docs/testsuite.html
#   (or docs/testsuite)
#
# • https://build.i3wm.org/docs/lib-i3test.html
#   (alternatively: perldoc ./testcases/lib/i3test.pm)
#
# • https://build.i3wm.org/docs/ipc.html
#   (or docs/ipc)
#
# • http://onyxneon.com/books/modern_perl/modern_perl_a4.pdf
#   (unless you are already familiar with Perl)
#
# Tests that switching to next/prev workspace doesn't skip those that have
# the same number as current.
use i3test;

cmd "workspace 1";
open_window;
cmd "workspace 2:bar";
open_window;
cmd "workspace 2:foo";
open_window;
cmd "workspace 3";
open_window;

cmd "workspace 2:foo";
cmd "workspace next";
is(focused_ws, "2:bar", "move from 2:foo to next is 2:bar");

cmd "workspace 2:bar";
cmd "workspace prev";
is(focused_ws, "2:foo", "move from 2:bar to prev is 2:foo");

cmd "workspace 2:foo";
cmd "workspace next_on_output";
is(focused_ws, "2:bar", "move from 2:foo to next_on_output is 2:bar");

cmd "workspace 2:bar";
cmd "workspace prev_on_output";
is(focused_ws, "2:foo", "move from 2:bar to prev_on_output is 2:foo");

done_testing;
