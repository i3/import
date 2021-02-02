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
# Tests whether workspace_layout is properly set after startup.
#
use List::Util qw(first);
use i3test i3_autostart => 0;

################################################################################
# Test that workspace_layout is properly set
################################################################################

my $config = <<EOT;
# i3 config file (v4)
font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1

fake-outputs 1024x768+0+0
workspace_layout tabbed
EOT

my $pid = launch_with_config($config);

is(focused_ws, '1', 'starting on workspace 1');
my $ws = get_ws(1);
is($ws->{workspace_layout}, 'tabbed', 'workspace layout is "tabbed"');
is($ws->{layout_fill_order}, 'default', 'unspecified workspace layout fill order reverts to "default"');

exit_gracefully($pid);

################################################################################
# Test that workspace_layout fill order is properly set
################################################################################

$config = <<EOT;
# i3 config file (v4)
font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1

fake-outputs 1024x768+0+0
workspace_layout stacked reverse
EOT

$pid = launch_with_config($config);

my $tmp = fresh_workspace;
$ws = get_ws($tmp);

is($ws->{workspace_layout}, 'stacked', 'workspace layout is "stacked"');
is($ws->{layout_fill_order}, 'reverse', 'workspace layout fill order is "reverse"');

exit_gracefully($pid);

################################################################################
# Test that workspace_layout fill order falls back to default on unknown value
################################################################################

$config = <<EOT;
# i3 config file (v4)
font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1

fake-outputs 1024x768+0+0
workspace_layout stacked foobar
EOT

$pid = launch_with_config($config);

$tmp = fresh_workspace;
$ws = get_ws($tmp);

is($ws->{layout_fill_order}, 'default', 'unknown workspace layout fill order config is translated to "default"');

exit_gracefully($pid);

done_testing;
