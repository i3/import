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
# Test dragging containers.

my ($width, $height) = (1000, 500);

# use i3test i3_config => <<"EOT";
my $config = <<"EOT";
# i3 config file (v4)
font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1

focus_follows_mouse no
floating_modifier Mod1

# 2 side by side outputs
fake-outputs ${width}x${height}+0+0P,${width}x${height}+${width}+0

bar {
    output primary
}
EOT
use i3test i3_autostart => 0;
use i3test::XTEST;
my $pid = launch_with_config($config);

sub start_drag {
    my ($pos_x, $pos_y) = @_;
    die "Drag outside of bounds!" unless $pos_x < $width * 2 && $pos_y < $height;

    $x->root->warp_pointer($pos_x, $pos_y);
    sync_with_i3;

    xtest_key_press(64);        # Alt_L
    xtest_button_press(1, $pos_x, $pos_y);
    xtest_sync_with_i3;
}

sub end_drag {
    my ($pos_x, $pos_y) = @_;
    die "Drag outside of bounds!" unless $pos_x < $width * 2 && $pos_y < $height;

    $x->root->warp_pointer($pos_x, $pos_y);
    sync_with_i3;

    xtest_button_release(1, $pos_x, $pos_y);
    xtest_key_release(64);      # Alt_L
    xtest_sync_with_i3;
}

my ($ws1, $ws2);
my ($A, $B, $tmp);
my ($A_id, $B_id);

sub move_subtest {
    my ($cb, $win) = @_;

    my @events = events_for($cb, 'window');
    my @move = grep { $_->{change} eq 'move' } @events;

    is(scalar @move, 1, 'Received 1 window::move event');
    is($move[0]->{container}->{window}, $A->{id}, "window id matches");
}

###############################################################################
# Drag floating container onto an empty workspace.
###############################################################################

$ws2 = fresh_workspace(output => 1);
$ws1 = fresh_workspace(output => 0);
$A = open_floating_window(rect => [ 30, 30, 50, 50 ]);

start_drag(40, 40);
end_drag(1050, 50);

is($x->input_focus, $A->id, 'Floating window moved to the right workspace');
is($ws2, focused_ws, 'Empty workspace focused after floating window dragged to it');

###############################################################################
# Drag tiling container onto an empty workspace.
###############################################################################

subtest "Draging tiling container onto an empty workspace produces move event", \&move_subtest,
sub {

$ws2 = fresh_workspace(output => 1);
$ws1 = fresh_workspace(output => 0);
$A = open_window;

start_drag(50, 50);
end_drag(1050, 50);

is($x->input_focus, $A->id, 'Tiling window moved to the right workspace');
is($ws2, focused_ws, 'Empty workspace focused after tiling window dragged to it');

};

###############################################################################
# Drag tiling container onto a container that closes before the drag is
# complete.
###############################################################################

$ws1 = fresh_workspace(output => 0);
$A = open_window;
open_window;

start_drag(600, 300);  # Start dragging the second window.

# Try to place it on the first window.
$x->root->warp_pointer(50, 50);
sync_with_i3;

cmd '[id=' . $A->id . '] kill';
sync_with_i3;
end_drag(50, 50);

is(@{get_ws_content($ws1)}, 1, 'One container left in ws1');

###############################################################################
# Drag tiling container onto a tiling container on an other workspace.
###############################################################################

subtest "Draging tiling container onto a tiling container on an other workspace produces move event", \&move_subtest,
sub {

$ws2 = fresh_workspace(output => 1);
open_window;
$B_id = get_focused($ws2);
$ws1 = fresh_workspace(output => 0);
$A = open_window;
$A_id = get_focused($ws1);

start_drag(50, 50);
end_drag(1500, 250);  # Center of right output, inner region.

is($ws2, focused_ws, 'Workspace focused after tiling window dragged to it');
$ws2 = get_ws($ws2);
is($ws2->{focus}[0], $A_id, 'A focused first, dragged container kept focus');
is($ws2->{focus}[1], $B_id, 'B focused second');

};

###############################################################################
# Drag tiling container onto a floating container on an other workspace.
###############################################################################

subtest "Draging tiling container onto a floating container on an other workspace produces move event", \&move_subtest,
sub {

$ws2 = fresh_workspace(output => 1);
open_floating_window;
$B_id = get_focused($ws2);
$ws1 = fresh_workspace(output => 0);
$A = open_window;
$A_id = get_focused($ws1);

start_drag(50, 50);
end_drag(1500, 250);

is($ws2, focused_ws, 'Workspace with one floating container focused after tiling window dragged to it');
$ws2 = get_ws($ws2);
is($ws2->{focus}[0], $A_id, 'A focused first, dragged container kept focus');
is($ws2->{floating_nodes}[0]->{nodes}[0]->{id}, $B_id, 'B exists & floating');

};

###############################################################################
# Drag tiling container onto a bar.
###############################################################################

subtest "Draging tiling container onto a bar produces move event", \&move_subtest,
sub {

$ws1 = fresh_workspace(output => 0);
open_window;
$B_id = get_focused($ws1);
$ws2 = fresh_workspace(output => 1);
$A = open_window;
$A_id = get_focused($ws2);

start_drag(1500, 250);
end_drag(1, 498);  # Bar on bottom of left output.

is($ws1, focused_ws, 'Workspace focused after tiling window dragged to its bar');
$ws1 = get_ws($ws1);
is($ws1->{focus}[0], $A_id, 'B focused first, dragged container kept focus');
is($ws1->{focus}[1], $B_id, 'A focused second');

};

###############################################################################
# Drag an unfocused tiling container onto it's self.
###############################################################################

$ws1 = fresh_workspace(output => 0);
open_window;
$A_id = get_focused($ws1);
open_window;
$B_id = get_focused($ws1);

start_drag(50, 50);
end_drag(450, 450);

$ws1 = get_ws($ws1);
is($ws1->{focus}[0], $B_id, 'B focused first, kept focus');
is($ws1->{focus}[1], $A_id, 'A focused second, unfocused dragged container didn\'t gain focus');

###############################################################################
# Drag an unfocused tiling container onto an occupied workspace.
###############################################################################

subtest "Draging unfocused tiling container onto an occupied workspace produces move event", \&move_subtest,
sub {

$ws1 = fresh_workspace(output => 0);
$A = open_window;
$A_id = get_focused($ws1);
$ws2 = fresh_workspace(output => 1);
open_window;
$B_id = get_focused($ws2);

start_drag(50, 50);
end_drag(1500, 250);  # Center of right output, inner region.

is($ws2, focused_ws, 'Workspace remained focused after dragging unfocused container');
$ws2 = get_ws($ws2);
is($ws2->{focus}[0], $B_id, 'B focused first, kept focus');
is($ws2->{focus}[1], $A_id, 'A focused second, unfocused container didn\'t steal focus');

};

###############################################################################
# Drag fullscreen container onto window in same workspace.
###############################################################################

$ws1 = fresh_workspace(output => 0);
open_window;
$A = open_window;
cmd 'fullscreen enable';

start_drag(900, 100);  # Second window
end_drag(50, 50);  # To first window

is($ws1, focused_ws, 'Workspace remained focused after dragging fullscreen container');
is_num_fullscreen($ws1, 1, 'Container still fullscreened');
is($x->input_focus, $A->id, 'Fullscreen container still focused');

###############################################################################
# Drag unfocused fullscreen container onto window in other workspace.
###############################################################################

subtest "Draging unfocused fullscreen container onto window in other workspace produces move event", \&move_subtest,
sub {

$ws1 = fresh_workspace(output => 0);
$A = open_window;
cmd 'fullscreen enable';
$ws2 = fresh_workspace(output => 1);
open_window;
open_window;

start_drag(900, 100);
end_drag(1000 + 500 * 0.15 + 10, 200);  # left of leftmost window

is($ws2, focused_ws, 'Workspace still focused after dragging fullscreen container to it');
is_num_fullscreen($ws1, 0, 'No fullscreen container in first workspace');
is_num_fullscreen($ws2, 1, 'Moved container still fullscreened');
is($x->input_focus, $A->id, 'Fullscreen container now focused');
$ws2 = get_ws($ws2);
is($ws2->{nodes}->[0]->{window}, $A->id, 'Fullscreen container now leftmost window in second workspace');

};

###############################################################################
# Drag unfocused fullscreen container onto left outter region of window in
# other workspace. The container shouldn't end up in $ws2 because it was
# dragged onto the outter region of the leftmost window. We must also check
# that the focus remains on the other window.
###############################################################################

subtest "Draging unfocused fullscreen container onto left outter region of window in other workspace produces move event", \&move_subtest,
sub {

$ws1 = fresh_workspace(output => 0);
open_window for (1..3);
$A = open_window;
$tmp = get_focused($ws1);
cmd 'fullscreen enable';
$ws2 = fresh_workspace(output => 1);
$B = open_window;

start_drag(990, 100);  # rightmost of $ws1
end_drag(1004, 100);  # outter region of window of $ws2

is($ws2, focused_ws, 'Workspace still focused after dragging fullscreen container to it');
is_num_fullscreen($ws1, 1, 'Fullscreen container still in first workspace');
is_num_fullscreen($ws2, 0, 'No fullscreen container in second workspace');
is($x->input_focus, $B->id, 'Window of second workspace still has focus');
is(get_focused($ws1), $tmp, 'Fullscreen container still focused in first workspace');
$ws1 = get_ws($ws1);
is($ws1->{nodes}->[3]->{window}, $A->id, 'Fullscreen container still rightmost window in first workspace');

};

###############################################################################
# Disable tiling drag through the config option.
###############################################################################

exit_gracefully($pid);

$config = <<"EOT";
# i3 config file (v4)
font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1

focus_follows_mouse no
tiling_drag no
floating_modifier Mod1

fake-outputs ${width}x${height}+0+0,${width}x${height}+${width}+0
EOT
$pid = launch_with_config($config);

$ws2 = fresh_workspace(output => 1);
$ws1 = fresh_workspace(output => 0);
open_window;
$A = get_focused($ws1);

start_drag(50, 50);
end_drag(1050, 50);

is(get_focused($ws1), $A, 'Tiling window still in first workspace after tiling drag disabled');

exit_gracefully($pid);

###############################################################################

done_testing;
