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
# Tests the standalone parser binary to see if it calls the right code when
# confronted with various commands, if it prints proper error messages for
# wrong commands and if it terminates in every case.
#
use i3test i3_autostart => 0;
use IPC::Run qw(run);

sub parser_calls {
    my ($command) = @_;

    my $stdout;
    run [ 'test.config_parser', $command ],
        '>/dev/null',
        '2>', \$stdout;
    # TODO: use a timeout, so that we can error out if it doesn’t terminate

    # Filter out all debugging output.
    my @lines = split("\n", $stdout);
    @lines = grep { not /^# / } @lines;

    # The criteria management calls are irrelevant and not what we want to test
    # in the first place.
    @lines = grep { !(/cfg_criteria_init/ || /cfg_criteria_pop_state/) } @lines;
    return join("\n", @lines) . "\n";
}

my $config = <<'EOT';
mode "meh" {
    bindsym Mod1 + Shift +   x resize grow
    bindcode Mod1+44 resize shrink
    bindsym --release Mod1+x exec foo
    bindsym --whole-window button3 nop
    bindsym --release --whole-window button3 nop
    bindsym --border button3 nop
    bindsym --release --border button3 nop
    bindsym --exclude-titlebar button3 nop
    bindsym --whole-window --border --exclude-titlebar button3 nop
}
EOT

my $expected = <<'EOT';
cfg_enter_mode((null), meh)
cfg_mode_binding(bindsym, Mod1,Shift, x, (null), (null), (null), (null), resize grow)
cfg_mode_binding(bindcode, Mod1, 44, (null), (null), (null), (null), resize shrink)
cfg_mode_binding(bindsym, Mod1, x, --release, (null), (null), (null), exec foo)
cfg_mode_binding(bindsym, (null), button3, (null), (null), --whole-window, (null), nop)
cfg_mode_binding(bindsym, (null), button3, --release, (null), --whole-window, (null), nop)
cfg_mode_binding(bindsym, (null), button3, (null), --border, (null), (null), nop)
cfg_mode_binding(bindsym, (null), button3, --release, --border, (null), (null), nop)
cfg_mode_binding(bindsym, (null), button3, (null), (null), (null), --exclude-titlebar, nop)
cfg_mode_binding(bindsym, (null), button3, (null), --border, --whole-window, --exclude-titlebar, nop)
EOT

is(parser_calls($config),
   $expected,
   'mode bindings ok');

################################################################################
# exec and exec_always
################################################################################

$config = <<'EOT';
exec geeqie
exec --no-startup-id /tmp/foo.sh
exec_always firefox
exec_always --no-startup-id /tmp/bar.sh
EOT

$expected = <<'EOT';
cfg_exec(exec, (null), geeqie)
cfg_exec(exec, --no-startup-id, /tmp/foo.sh)
cfg_exec(exec_always, (null), firefox)
cfg_exec(exec_always, --no-startup-id, /tmp/bar.sh)
EOT

is(parser_calls($config),
   $expected,
   'exec okay');

################################################################################
# for_window
################################################################################

$config = <<'EOT';
for_window [] nop empty
for_window [class="^Chrome"] floating enable
for_window [class=^Chrome] floating enable
for_window [floating_from   = "auto" class= ==Class==  ] nop floating
for_window [tiling_from=auto class="==Class=="]nop floating
EOT

$expected = <<'EOT';
cfg_for_window(nop empty)
cfg_criteria_add(class, ^Chrome)
cfg_for_window(floating enable)
cfg_criteria_add(class, ^Chrome)
cfg_for_window(floating enable)
cfg_criteria_add(floating_from, auto)
cfg_criteria_add(class, ==Class==)
cfg_for_window(nop floating)
cfg_criteria_add(tiling_from, auto)
cfg_criteria_add(class, ==Class==)
cfg_for_window(nop floating)
EOT

is(parser_calls($config),
   $expected,
   'for_window okay');

$config = <<'EOT';
for_window [tiling_from=typo] nop typo
for_window [tiling_from="typo"] nop typo
EOT

$expected = <<'EOT';
ERROR: CONFIG: Expected one of these tokens: '"', 'auto', 'user'
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   1: for_window [tiling_from=typo] nop typo
ERROR: CONFIG:                                   ^^^^^^^^^^^^^^
ERROR: CONFIG: Line   2: for_window [tiling_from="typo"] nop typo
ERROR: CONFIG: Expected one of these tokens: 'auto', 'user'
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   1: for_window [tiling_from=typo] nop typo
ERROR: CONFIG: Line   2: for_window [tiling_from="typo"] nop typo
ERROR: CONFIG:                                    ^^^^^^^^^^^^^^^
EOT

is(parser_calls($config),
   $expected,
   'for_window errors okay');

################################################################################
# assign
################################################################################

$config = <<'EOT';
assign [class="^Chrome"] 4
assign [class="^Chrome"] workspace number 3
assign [class="^Chrome"] named workspace
assign [class="^Chrome"] "quoted named workspace"
assign [class="^Chrome"] → "quoted named workspace"
EOT

$expected = <<'EOT';
cfg_criteria_add(class, ^Chrome)
cfg_assign(4, 0)
cfg_criteria_add(class, ^Chrome)
cfg_assign(3, 1)
cfg_criteria_add(class, ^Chrome)
cfg_assign(named workspace, 0)
cfg_criteria_add(class, ^Chrome)
cfg_assign(quoted named workspace, 0)
cfg_criteria_add(class, ^Chrome)
cfg_assign(quoted named workspace, 0)
EOT

is(parser_calls($config),
   $expected,
   'for_window okay');

################################################################################
# floating_minimum_size / floating_maximum_size
################################################################################

$config = <<'EOT';
floating_minimum_size 80x55
floating_minimum_size 80    x  55
floating_maximum_size 73 x 10
EOT

$expected = <<'EOT';
cfg_floating_minimum_size(80, 55)
cfg_floating_minimum_size(80, 55)
cfg_floating_maximum_size(73, 10)
EOT

is(parser_calls($config),
   $expected,
   'floating_minimum_size ok');

################################################################################
# popup_during_fullscreen
################################################################################

$config = <<'EOT';
popup_during_fullscreen ignore
popup_during_fullscreen leave_fullscreen
popup_during_fullscreen SMArt
EOT

$expected = <<'EOT';
cfg_popup_during_fullscreen(ignore)
cfg_popup_during_fullscreen(leave_fullscreen)
cfg_popup_during_fullscreen(smart)
EOT

is(parser_calls($config),
   $expected,
   'popup_during_fullscreen ok');


################################################################################
# floating_modifier
################################################################################

$config = <<'EOT';
floating_modifier Mod1
floating_modifier mOd1
EOT

$expected = <<'EOT';
cfg_floating_modifier(Mod1)
cfg_floating_modifier(Mod1)
EOT

is(parser_calls($config),
   $expected,
   'floating_modifier ok');

################################################################################
# default_orientation
################################################################################

$config = <<'EOT';
default_orientation horizontal
default_orientation vertical
default_orientation auto
EOT

$expected = <<'EOT';
cfg_default_orientation(horizontal)
cfg_default_orientation(vertical)
cfg_default_orientation(auto)
EOT

is(parser_calls($config),
   $expected,
   'default_orientation ok');

################################################################################
# workspace_layout
################################################################################

$config = <<'EOT';
workspace_layout default
workspace_layout default reverse
workspace_layout stacked
workspace_layout stacked reverse
workspace_layout stacking
workspace_layout stacking reverse
workspace_layout tabbed
workspace_layout tabbed reverse
EOT

$expected = <<'EOT';
cfg_workspace_layout(default, (null))
cfg_workspace_layout(default, reverse)
cfg_workspace_layout(stacked, (null))
cfg_workspace_layout(stacked, reverse)
cfg_workspace_layout(stacking, (null))
cfg_workspace_layout(stacking, reverse)
cfg_workspace_layout(tabbed, (null))
cfg_workspace_layout(tabbed, reverse)
EOT

is(parser_calls($config),
   $expected,
   'workspace_layout ok');

################################################################################
# workspace assignments, with trailing whitespace (ticket #921)
################################################################################

$config = <<'EOT';
workspace "3" output DP-1
workspace "3" output     	VGA-1
EOT

$expected = <<'EOT';
cfg_workspace(3, DP-1)
cfg_workspace(3, VGA-1)
EOT

is(parser_calls($config),
   $expected,
   'workspace assignment ok');

################################################################################
# new_window
################################################################################

$config = <<'EOT';
new_window 1pixel
new_window normal
new_window none
default_border 1pixel
default_border normal
default_border none
new_float 1pixel
new_float normal
new_float none
default_floating_border 1pixel
default_floating_border normal
default_floating_border none
EOT

$expected = <<'EOT';
cfg_default_border(new_window, 1pixel, -1)
cfg_default_border(new_window, normal, 2)
cfg_default_border(new_window, none, -1)
cfg_default_border(default_border, 1pixel, -1)
cfg_default_border(default_border, normal, 2)
cfg_default_border(default_border, none, -1)
cfg_default_border(new_float, 1pixel, -1)
cfg_default_border(new_float, normal, 2)
cfg_default_border(new_float, none, -1)
cfg_default_border(default_floating_border, 1pixel, -1)
cfg_default_border(default_floating_border, normal, 2)
cfg_default_border(default_floating_border, none, -1)
EOT

# TODO: are there no tests for "border pixel 1" etc?

is(parser_calls($config),
   $expected,
   'new_window ok');

################################################################################
# hide_edge_borders
################################################################################

$config = <<'EOT';
hide_edge_borders none
hide_edge_borders vertical
hide_edge_borders horizontal
hide_edge_borders both
hide_edge_borders smart
EOT

$expected = <<'EOT';
cfg_hide_edge_borders(none)
cfg_hide_edge_borders(vertical)
cfg_hide_edge_borders(horizontal)
cfg_hide_edge_borders(both)
cfg_hide_edge_borders(smart)
EOT

is(parser_calls($config),
   $expected,
   'hide_edge_borders ok');

################################################################################
# focus_follows_mouse
################################################################################

$config = <<'EOT';
focus_follows_mouse yes
focus_follows_mouse no
EOT

$expected = <<'EOT';
cfg_focus_follows_mouse(yes)
cfg_focus_follows_mouse(no)
EOT

is(parser_calls($config),
   $expected,
   'focus_follows_mouse ok');

################################################################################
# mouse_warping
################################################################################

$config = <<'EOT';
mouse_warping output
mouse_warping none
EOT

$expected = <<'EOT';
cfg_mouse_warping(output)
cfg_mouse_warping(none)
EOT

is(parser_calls($config),
   $expected,
   'mouse_warping ok');

################################################################################
# force_display_urgency_hint
################################################################################

is(parser_calls('force_display_urgency_hint 300'),
   "cfg_force_display_urgency_hint(300)\n",
   'force_display_urgency_hint ok');

is(parser_calls('force_display_urgency_hint 500 ms'),
   "cfg_force_display_urgency_hint(500)\n",
   'force_display_urgency_hint ok');

is(parser_calls('force_display_urgency_hint 700ms'),
   "cfg_force_display_urgency_hint(700)\n",
   'force_display_urgency_hint ok');

$config = <<'EOT';
force_display_urgency_hint 300
force_display_urgency_hint 500 ms
force_display_urgency_hint 700ms
force_display_urgency_hint 700
EOT

$expected = <<'EOT';
cfg_force_display_urgency_hint(300)
cfg_force_display_urgency_hint(500)
cfg_force_display_urgency_hint(700)
cfg_force_display_urgency_hint(700)
EOT

is(parser_calls($config),
   $expected,
   'force_display_urgency_hint ok');

################################################################################
# workspace
################################################################################

$config = <<'EOT';
workspace 3 output VGA-1
workspace "4: output" output VGA-2
workspace bleh output LVDS1/I_1
# See #3646
workspace foo output a b c "a b c"
EOT

$expected = <<'EOT';
cfg_workspace(3, VGA-1)
cfg_workspace(4: output, VGA-2)
cfg_workspace(bleh, LVDS1/I_1)
cfg_workspace(foo, a)
cfg_workspace((null), b)
cfg_workspace((null), c)
cfg_workspace((null), a b c)
EOT

is(parser_calls($config),
   $expected,
   'workspace ok');

################################################################################
# ipc-socket
################################################################################

$config = <<'EOT';
ipc-socket /tmp/i3.sock
ipc_socket ~/.i3/i3.sock
EOT

$expected = <<'EOT';
cfg_ipc_socket(/tmp/i3.sock)
cfg_ipc_socket(~/.i3/i3.sock)
EOT

is(parser_calls($config),
   $expected,
   'ipc-socket ok');

################################################################################
# colors
################################################################################

$config = <<'EOT';
client.focused          #4c7899 #285577 #ffffff #2e9ef4 #b34d4c
client.focused_inactive #333333 #5f676a #ffffff #484e50
client.unfocused        #333333 #222222 #888888 #292d2e
client.urgent           #2f343a #900000 #ffffff #900000 #c00000
client.placeholder      #000000 #0c0c0c #ffffff #000000
EOT

$expected = <<'EOT';
cfg_color(client.focused, #4c7899, #285577, #ffffff, #2e9ef4, #b34d4c)
cfg_color(client.focused_inactive, #333333, #5f676a, #ffffff, #484e50, NULL)
cfg_color(client.unfocused, #333333, #222222, #888888, #292d2e, NULL)
cfg_color(client.urgent, #2f343a, #900000, #ffffff, #900000, #c00000)
cfg_color(client.placeholder, #000000, #0c0c0c, #ffffff, #000000, NULL)
EOT

is(parser_calls($config),
   $expected,
   'colors ok');

################################################################################
# Verify that errors don’t harm subsequent valid statements
################################################################################

$config = <<'EOT';
hide_edge_border both
client.focused          #4c7899 #285577 #ffffff #2e9ef4
EOT

my $expected_all_tokens = "ERROR: CONFIG: Expected one of these tokens: <end>, '#', '" . join("', '", 'set ', 'set	', qw(
        set_from_resource
        bindsym
        bindcode
        bind
        bar
        font
        mode
        floating_minimum_size
        floating_maximum_size
        floating_modifier
        default_orientation
        workspace_layout
        default_border
        new_window
        default_floating_border
        new_float
        hide_edge_borders
        for_window
        assign
        no_focus
        focus_follows_mouse
        mouse_warping
        focus_wrapping
        force_focus_wrapping
        force_xinerama
        force-xinerama
        disable_randr15
        disable-randr15
        workspace_auto_back_and_forth
        fake_outputs
        fake-outputs
        force_display_urgency_hint
        focus_on_window_activation
        title_align
        show_marks
        workspace
        ipc_socket
        ipc-socket
        ipc_kill_timeout
        restart_state
        popup_during_fullscreen
        exec_always
        exec
        client.background
        client.focused_inactive
        client.focused
        client.unfocused
        client.urgent
        client.placeholder
    )) . "'\n";

my $expected_end = <<'EOT';
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   1: hide_edge_border both
ERROR: CONFIG:           ^^^^^^^^^^^^^^^^^^^^^
ERROR: CONFIG: Line   2: client.focused          #4c7899 #285577 #ffffff #2e9ef4
cfg_color(client.focused, #4c7899, #285577, #ffffff, #2e9ef4, NULL)
EOT

$expected = $expected_all_tokens . $expected_end;

is(parser_calls($config),
   $expected,
   'errors dont harm subsequent statements');

$config = <<'EOT';
hide_edge_borders FOOBAR
client.focused          #4c7899 #285577 #ffffff #2e9ef4
EOT

$expected = <<'EOT';
ERROR: CONFIG: Expected one of these tokens: 'none', 'vertical', 'horizontal', 'both', 'smart', '1', 'yes', 'true', 'on', 'enable', 'active'
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   1: hide_edge_borders FOOBAR
ERROR: CONFIG:                             ^^^^^^
ERROR: CONFIG: Line   2: client.focused          #4c7899 #285577 #ffffff #2e9ef4
cfg_color(client.focused, #4c7899, #285577, #ffffff, #2e9ef4, NULL)
EOT

is(parser_calls($config),
   $expected,
   'errors dont harm subsequent statements');

################################################################################
# Regression: semicolons end comments, but shouldn’t
################################################################################

$config = <<'EOT';
# "foo" client.focused          #4c7899 #285577 #ffffff #2e9ef4
EOT

$expected = <<'EOT';

EOT

is(parser_calls($config),
   $expected,
   'semicolon does not end a comment line');

################################################################################
# Error message with 2+2 lines of context
################################################################################

$config = <<'EOT';
# i3 config file (v4)

font foobar

unknown qux

# yay
# this should not show up
EOT

my $expected_head = <<'EOT';
cfg_font(foobar)
EOT

my $expected_tail = <<'EOT';
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   3: font foobar
ERROR: CONFIG: Line   4: 
ERROR: CONFIG: Line   5: unknown qux
ERROR: CONFIG:           ^^^^^^^^^^^
ERROR: CONFIG: Line   6: 
ERROR: CONFIG: Line   7: # yay
EOT

$expected = $expected_head . $expected_all_tokens . $expected_tail;

is(parser_calls($config),
   $expected,
   'error message (2+2 context) ok');

################################################################################
# Error message with 0+0 lines of context
################################################################################

$config = <<'EOT';
unknown qux
EOT

$expected_tail = <<'EOT';
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   1: unknown qux
ERROR: CONFIG:           ^^^^^^^^^^^
EOT

$expected = $expected_all_tokens . $expected_tail;

is(parser_calls($config),
   $expected,
   'error message (0+0 context) ok');

################################################################################
# Error message with 1+0 lines of context
################################################################################

$config = <<'EOT';
# context before
unknown qux
EOT

$expected_tail = <<'EOT';
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   1: # context before
ERROR: CONFIG: Line   2: unknown qux
ERROR: CONFIG:           ^^^^^^^^^^^
EOT

$expected = $expected_all_tokens . $expected_tail;

is(parser_calls($config),
   $expected,
   'error message (1+0 context) ok');

################################################################################
# Error message with 0+1 lines of context
################################################################################

$config = <<'EOT';
unknown qux
# context after
EOT

$expected_tail = <<'EOT';
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   1: unknown qux
ERROR: CONFIG:           ^^^^^^^^^^^
ERROR: CONFIG: Line   2: # context after
EOT

$expected = $expected_all_tokens . $expected_tail;

is(parser_calls($config),
   $expected,
   'error message (0+1 context) ok');

################################################################################
# Error message with 0+2 lines of context
################################################################################

$config = <<'EOT';
unknown qux
# context after
# context 2 after
EOT

$expected_tail = <<'EOT';
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   1: unknown qux
ERROR: CONFIG:           ^^^^^^^^^^^
ERROR: CONFIG: Line   2: # context after
ERROR: CONFIG: Line   3: # context 2 after
EOT

$expected = $expected_all_tokens . $expected_tail;

is(parser_calls($config),
   $expected,
   'error message (0+2 context) ok');

################################################################################
# Error message within mode blocks
################################################################################

$config = <<'EOT';
mode "yo" {
    bindsym x resize shrink left
    unknown qux
}
EOT

$expected = <<'EOT';
cfg_enter_mode((null), yo)
cfg_mode_binding(bindsym, (null), x, (null), (null), (null), (null), resize shrink left)
ERROR: CONFIG: Expected one of these tokens: <end>, '#', 'set', 'bindsym', 'bindcode', 'bind', '}'
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   1: mode "yo" {
ERROR: CONFIG: Line   2:     bindsym x resize shrink left
ERROR: CONFIG: Line   3:     unknown qux
ERROR: CONFIG:               ^^^^^^^^^^^
ERROR: CONFIG: Line   4: }
EOT

is(parser_calls($config),
   $expected,
   'error message (mode block) ok');

################################################################################
# Error message within bar blocks
################################################################################

$config = <<'EOT';
bar {
    output LVDS-1
    unknown qux
}
EOT

$expected = <<'EOT';
cfg_bar_start()
cfg_bar_output(LVDS-1)
ERROR: CONFIG: Expected one of these tokens: <end>, '#', 'set', 'i3bar_command', 'status_command', 'socket_path', 'mode', 'hidden_state', 'id', 'modifier', 'wheel_up_cmd', 'wheel_down_cmd', 'bindsym', 'position', 'output', 'tray_output', 'tray_padding', 'font', 'separator_symbol', 'binding_mode_indicator', 'workspace_buttons', 'workspace_min_width', 'strip_workspace_numbers', 'strip_workspace_name', 'verbose', 'colors', '}'
ERROR: CONFIG: (in file <stdin>)
ERROR: CONFIG: Line   1: bar {
ERROR: CONFIG: Line   2:     output LVDS-1
ERROR: CONFIG: Line   3:     unknown qux
ERROR: CONFIG:               ^^^^^^^^^^^
ERROR: CONFIG: Line   4: }
cfg_bar_finish()
EOT

is(parser_calls($config),
   $expected,
   'error message (bar block) ok');

done_testing;
