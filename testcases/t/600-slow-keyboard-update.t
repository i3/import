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
# Test that updating the keyboard state is fast.
# Ticket: #3924
# Bug still in: 4.19.2-70-g42c3dbe0
use i3test;

my $start = time();

# The following causes an X11 event per line. Before the fix, running this took
# about 13 seconds until i3 became responsive again.
system(q@for x in $(seq 1 5000); do echo "keycode 107 = parenleft" ; done | xmodmap -@);
sync_with_i3;

my $delay = time() - $start;
ok($delay <= 2, 'Test finishes quickly');

done_testing;
