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

sub parser_calls {
    my ($command) = @_;

    # TODO: use a timeout, so that we can error out if it doesn’t terminate
    # TODO: better way of passing arguments
    my $stdout = qx(test.commands_parser '$command' 2>&1 >&-);

    # Filter out all debugging output.
    my @lines = split("\n", $stdout);
    @lines = grep { not /^# / } @lines;

    # The criteria management calls are irrelevant and not what we want to test
    # in the first place.
    @lines = grep { !(/cmd_criteria_init()/ || /cmd_criteria_match_windows/) } @lines;
    return join("\n", @lines);
}

################################################################################
# 1: First that the parser properly recognizes commands which are ok.
################################################################################

# The first call has only a single command, the following ones are consolidated
# for performance.
is(parser_calls('move workspace 3'),
   'cmd_move_con_to_workspace_name(3, (null))',
   'single number (move workspace 3) ok');

is(parser_calls(
   'move to workspace 3; ' .
   'move window to workspace 3; ' .
   'move container to workspace 3; ' .
   'move workspace foobar; ' .
   'move workspace torrent; ' .
   'move workspace to output LVDS1; ' .
   'move workspace 3: foobar; ' .
   'move workspace "3: foobar"; ' .
   'move workspace "3: foobar, baz"; '),
   "cmd_move_con_to_workspace_name(3, (null))\n" .
   "cmd_move_con_to_workspace_name(3, (null))\n" .
   "cmd_move_con_to_workspace_name(3, (null))\n" .
   "cmd_move_con_to_workspace_name(foobar, (null))\n" .
   "cmd_move_con_to_workspace_name(torrent, (null))\n" .
   "cmd_move_workspace_to_output(LVDS1)\n" .
   "cmd_move_con_to_workspace_name(3: foobar, (null))\n" .
   "cmd_move_con_to_workspace_name(3: foobar, (null))\n" .
   "cmd_move_con_to_workspace_name(3: foobar, baz, (null))",
   'move ok');

is(parser_calls('move workspace 3: foobar, nop foo'),
   "cmd_move_con_to_workspace_name(3: foobar, (null))\n" .
   "cmd_nop(foo)",
   'multiple ops (move workspace 3: foobar, nop foo) ok');

is(parser_calls(
   'exec i3-sensible-terminal; ' .
   'exec --no-startup-id i3-sensible-terminal'),
   "cmd_exec((null), i3-sensible-terminal)\n" .
   "cmd_exec(--no-startup-id, i3-sensible-terminal)",
   'exec ok');

is(parser_calls(
   'resize shrink left; ' .
   'resize shrink left 25 px; ' .
   'resize shrink left 25 px or 33 ppt; ' .
   'resize shrink left 25'),
   "cmd_resize(shrink, left, 10, 10)\n" .
   "cmd_resize(shrink, left, 25, 0)\n" .
   "cmd_resize(shrink, left, 25, 33)\n" .
   "cmd_resize(shrink, left, 25, 0)",
   'simple resize ok');

is(parser_calls('resize shrink left 25 px or 33 ppt,'),
   'cmd_resize(shrink, left, 25, 33)',
   'trailing comma resize ok');

is(parser_calls('resize shrink left 25 px or 33 ppt;'),
   'cmd_resize(shrink, left, 25, 33)',
   'trailing semicolon resize ok');

is(parser_calls('[con_mark=yay] focus'),
   "cmd_criteria_add(con_mark, yay)\n" .
   "cmd_focus(0)",
   'criteria focus ok');

is(parser_calls('[con_mark=yay] focus workspace'),
   "cmd_criteria_add(con_mark, yay)\n" .
   "cmd_focus(1)",
   'criteria focus workspace ok');

is(parser_calls("[con_mark=yay con_mark=bar] focus"),
   "cmd_criteria_add(con_mark, yay)\n" .
   "cmd_criteria_add(con_mark, bar)\n" .
   "cmd_focus(0)",
   'criteria focus ok');

is(parser_calls("[con_mark=yay\tcon_mark=bar] focus"),
   "cmd_criteria_add(con_mark, yay)\n" .
   "cmd_criteria_add(con_mark, bar)\n" .
   "cmd_focus(0)",
   'criteria focus ok');

is(parser_calls("[con_mark=yay\tcon_mark=bar]\tfocus"),
   "cmd_criteria_add(con_mark, yay)\n" .
   "cmd_criteria_add(con_mark, bar)\n" .
   "cmd_focus(0)",
   'criteria focus ok');

is(parser_calls('[con_mark="yay"] focus'),
   "cmd_criteria_add(con_mark, yay)\n" .
   "cmd_focus(0)",
   'quoted criteria focus ok');

# Make sure trailing whitespace is stripped off: While this is not an issue for
# commands being parsed due to the configuration, people might send IPC
# commands with leading or trailing newlines.
is(parser_calls("workspace test\n"),
   'cmd_workspace_name(test, (null))',
   'trailing whitespace stripped off ok');

is(parser_calls("\nworkspace test"),
   'cmd_workspace_name(test, (null))',
   'trailing whitespace stripped off ok');

################################################################################
# 2: Verify that the parser spits out the right error message on commands which
# are not ok.
################################################################################

is(parser_calls('unknown_literal'),
   "ERROR: Expected one of these tokens: <end>, '[', '" . join("', '", qw(
       move
       exec
       exit
       restart
       reload
       shmlog
       debuglog
       border
       layout
       append_layout
       workspace
       focus
       kill
       open
       fullscreen
       sticky
       split
       floating
       mark
       unmark
       resize
       rename
       nop
       scratchpad
       swap
       title_format
       mode
       bar
    )) . "'\n" .
   "ERROR: Your command: unknown_literal\n" .
   "ERROR:               ^^^^^^^^^^^^^^^",
   'error for unknown literal ok');

is(parser_calls('move something to somewhere'),
   "ERROR: Expected one of these tokens: 'window', 'container', 'to', '--no-auto-back-and-forth', 'workspace', 'output', 'mark', 'scratchpad', 'left', 'right', 'up', 'down', 'position', 'absolute'\n" .
   "ERROR: Your command: move something to somewhere\n" .
   "ERROR:                    ^^^^^^^^^^^^^^^^^^^^^^",
   'error for unknown literal ok');

################################################################################
# 3: Verify that escaping works correctly
################################################################################

is(parser_calls('workspace "foo"'),
   'cmd_workspace_name(foo, (null))',
   'Command with simple double quotes ok');

is(parser_calls('workspace "foo'),
   'cmd_workspace_name(foo, (null))',
   'Command without ending double quotes ok');

is(parser_calls('workspace "foo \"bar"'),
   'cmd_workspace_name(foo "bar, (null))',
   'Command with escaped double quotes ok');

is(parser_calls('workspace "foo \\'),
   'cmd_workspace_name(foo \\, (null))',
   'Command with single backslash in the end ok');

is(parser_calls('workspace "foo\\\\bar"'),
   'cmd_workspace_name(foo\\bar, (null))',
   'Command with escaped backslashes ok');

is(parser_calls('workspace "foo\\\\\\"bar"'),
   'cmd_workspace_name(foo\\"bar, (null))',
   'Command with escaped double quotes after escaped backslashes ok');

################################################################################
# 4: Verify that resize commands with a "px or ppt"-construction are parsed
# correctly
################################################################################

is(parser_calls("resize shrink width 10 px or"),
   "ERROR: Expected one of these tokens: <number>\n" .
   "ERROR: Your command: resize shrink width 10 px or\n" .
   "ERROR:                                           ",
   "error for resize command with incomplete 'or'-construction ok");

is(parser_calls("resize grow left 10 px or 20 ppt"),
   "cmd_resize(grow, left, 10, 20)",
   "resize command with 'or'-construction ok");

done_testing;
