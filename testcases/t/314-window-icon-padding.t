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
# Verifies title_window_icon behavior.
use i3test i3_autostart => 0;

sub window_icon_padding {
    my ($ws) = @_;
    my ($nodes, $focus) = get_ws_content($ws);
    ok(@{$nodes} == 1, 'precisely one container on workspace');
    return $nodes->[0]->{'window_icon_padding'};
}

sub window_icon_position {
    my ($ws) = @_;
    my ($nodes, $focus) = get_ws_content($ws);
    ok(@{$nodes} == 1, 'precisely one container on workspace');
    return $nodes->[0]->{'window_icon_position'};
}

my $config = <<"EOT";
# i3 config file (v4)
font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1
EOT
my $pid = launch_with_config($config);

my $tmp = fresh_workspace;

cmd 'open';
is(window_icon_padding($tmp), -1, 'window_icon_padding defaults to -1');
is(window_icon_position($tmp), 'title', 'window_icon_position defaults to title');

cmd 'title_window_icon on';
isnt(window_icon_padding($tmp), -1, 'window_icon_padding no longer -1');
is(window_icon_position($tmp), 'title', 'window_icon_position defaults to title');

cmd 'title_window_icon position left';
is(window_icon_position($tmp), 'left', 'window_icon_position is now left');

cmd 'title_window_icon position right';
is(window_icon_position($tmp), 'right', 'window_icon_position is now right');

exit_gracefully($pid);

################################################################################
# Verify title_window_icon can be used with for_window as expected
################################################################################

$config = <<"EOT";
# i3 config file (v4)
font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1

for_window [class=".*"] title_window_icon padding 3px
for_window [class=".*"] title_window_icon position right
EOT
$pid = launch_with_config($config);

$tmp = fresh_workspace;

open_window;
is(window_icon_padding($tmp), 3, 'window_icon_padding set to 3');
is(window_icon_position($tmp), 'right', 'window_icon_position set to right');

exit_gracefully($pid);

done_testing;
