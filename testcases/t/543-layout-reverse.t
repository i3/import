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
# Tests reverse layouts.

# disable autostart here so that we can test using workspace_layout config
# option later on.
use i3test i3_autostart => 0;
use File::Temp qw(tempfile);
use IO::Handle;

my $pid = launch_with_config("");

my $tmp = fresh_workspace;

################################################################################
# Make sure cmp_tree() knows what to do with reverse layouts
################################################################################

cmp_tree(
    msg => "Reverse layout sanity check for cmp_tree()",
    ws => $tmp,
    layout_before => 'V[c d* Tr[e f g]]',
    layout_after => 'V[c d Tr[e f g]]',
    cb => sub {
        cmd 'focus next sibling';
    });

my @content = @{get_ws_content($tmp)};
is($content[0]->{nodes}[2]->{layout_fill_order}, 'reverse', 'manually check that the layout created by cmp_tree has the correct fill order');

################################################################################
# Make sure fill_order is correctly restored.
################################################################################
subtest 'layout_fill_order is correctly restored', sub {
    ok(!workspace_exists('ws_reverse'), 'workspace "ws_reverse" does not exist yet');

    my ($fh, $filename) = tempfile(UNLINK => 1);
    print $fh <<'EOT';
// vim:ts=4:sw=4:et
{
    "border": "pixel",
    "floating": "auto_off",
    "layout": "splith",
    "type": "workspace",
    "name": "ws_reverse",
    "layout_fill_order": "reverse",
    "nodes": [
        {
            "border": "pixel",
            "floating": "auto_off",
            "layout": "tabbed",
            "type": "con",
            "layout_fill_order": "default",
            "nodes": [
                {
                    "border": "pixel",
                    "floating": "auto_off",
                    "layout": "splith",
                    "type": "con",
                    "layout_fill_order": "default",
                    "nodes": [
                        {
                            "border": "pixel",
                            "floating": "auto_off",
                            "type": "con"
                        },
                        {
                            "border": "pixel",
                            "floating": "auto_off",
                            "type": "con"
                        }
                    ]
                },
                {
                    "border": "pixel",
                    "floating": "auto_off",
                    "type": "con"
                }
            ]
        },
        {
            "border": "pixel",
            "floating": "auto_off",
            "layout": "splitv",
            "type": "con",
            "layout_fill_order": "reverse"
        }
    ]
}
EOT
    $fh->flush;
    cmd "append_layout $filename";

    ok(workspace_exists('ws_reverse'), 'workspace "ws_reverse" exists now');

    my $ws = get_ws('ws_reverse');
    is($ws->{layout_fill_order}, 'reverse', 'workspace fill order is reverse');

    @content = @{$ws->{nodes}};
    is(@content, 2, 'workspace has two nodes');
    is($content[0]->{layout_fill_order}, 'default', 'child node one has default fill order');
    is($content[1]->{layout_fill_order}, 'reverse', 'child node two has reverse fill order');
};

################################################################################
# Test that con_get_tree_representation returns the correct layout string
################################################################################

# TODO Was unable to find a method to get the i3-frame titles

################################################################################
# Test that the layout fill_order command works on workspaces
################################################################################
my $ws;

subtest "command 'layout fill_order' works on workspaces", sub {
    $tmp = fresh_workspace;

    $ws = get_ws($tmp);
    is($ws->{layout_fill_order}, 'default', 'ensure the default layout fill order');

    cmd 'layout fill_order reverse';
    $ws = get_ws($tmp);
    is($ws->{layout_fill_order}, 'reverse', 'cmd "layout fill_order reverse" changes layout_fill_order to "reverse"');

    cmd 'layout fill_order default';
    $ws = get_ws($tmp);
    is($ws->{layout_fill_order}, 'default', 'cmd "layout fill_order default" changes layout_fill_order to "default"');

    cmd 'layout fill_order toggle';
    $ws = get_ws($tmp);
    is ($ws->{layout_fill_order}, 'reverse', 'cmd "layout fill_order toggle" changes layout_fill_order from "default" -> "reverse"');
    cmd 'layout fill_order toggle';
    $ws = get_ws($tmp);
    is ($ws->{layout_fill_order}, 'default', 'cmd "layout fill_order toggle" changes layout_fill_order from "reverse" -> "default"');

    cmd 'layout fill_order foobar';
    $ws = get_ws($tmp);
    is ($ws->{layout_fill_order}, 'default', 'nonsensical layout fill_order option leaves fill order at default');
};

################################################################################
# Test the layout fill_order command works on containers down the tree
################################################################################
subtest "command 'layout fill_order' works on containers down the tree", sub {
    $tmp = fresh_workspace;

    open_window;
    open_window;
    cmd 'split v';
    open_window;

    my ($nodes, $focus) = get_ws_content($tmp);
    is($nodes->[1]->{layout_fill_order}, 'default', 'layout fill order is default currently');

    cmd 'layout fill_order reverse';
    ($nodes, $focus) = get_ws_content($tmp);
    is($nodes->[1]->{layout_fill_order}, 'reverse', 'layout fill order is reverse now');

    cmd 'layout fill_order toggle';
    ($nodes, $focus) = get_ws_content($tmp);
    is($nodes->[1]->{layout_fill_order}, 'default', 'layout fill order was toggled to default');
};

################################################################################
# Make sure that command 'layout <layout> reverse' works as expected
################################################################################
my $split;

subtest "command 'layout <layout> reverse' works as expected", sub {
    $tmp = fresh_workspace;

    open_window;
    open_window;
    cmd 'split v';
    open_window;

    cmd 'layout stacked reverse';
    $split = @{get_ws_content($tmp)}[1];
    is($split->{layout}, 'stacked', 'layout stacked');
    is($split->{layout_fill_order}, 'reverse', 'layout fill order is reverse');
};

################################################################################
# Test that the layout_fill_order affects how containers get inserted into a layout)
################################################################################
subtest 'layout_fill_order property is considered when inserting into splitv layout', sub {
    $tmp = fresh_workspace;

    open_window;
    open_window name => 'foo';
    cmd 'split v';
    open_window name => 'bar';

    cmd 'layout fill_order reverse';
    open_window name => 'baz';

    my $split = @{get_ws_content($tmp)}[1];
    is($split->{nodes}[1]->{name}, 'baz', 'window baz was inserted before window bar (index 1)');
    is($split->{nodes}[2]->{name}, 'bar', 'window bar comes after window baz');

    open_window name => 'qux';
    $split = @{get_ws_content($tmp)}[1];
    is($split->{nodes}[1]->{name}, 'qux', 'window qux was inserted before window baz (index 0)');
};

subtest 'layout_fill_order property is considered when inserting into splith layout', sub {
    $tmp = fresh_workspace;

    open_window;
    open_window name => 'foo';
    cmd 'split h';
    open_window name => 'bar';

    cmd 'layout fill_order reverse';
    open_window name => 'baz';

    $split = @{get_ws_content($tmp)}[1];
    is($split->{nodes}[1]->{name}, 'baz', 'window baz was inserted before window bar (index 1)');
    is($split->{nodes}[2]->{name}, 'bar', 'window bar comes after window baz');
};

subtest 'layout_fill_order property is considered when inserting into tabbed layout', sub {
    $tmp = fresh_workspace;

    open_window;
    open_window name => 'foo';
    cmd 'split h';
    open_window name => 'bar';

    cmd 'layout tabbed';
    cmd 'layout fill_order reverse';
    open_window name => 'baz';

    $split = @{get_ws_content($tmp)}[1];
    is($split->{layout}, 'tabbed', 'new split has tabbed layout');
    is($split->{nodes}[1]->{name}, 'baz', 'window baz was inserted before window bar (index 1)');
    is($split->{nodes}[2]->{name}, 'bar', 'window bar comes after window baz');
};

subtest 'layout_fill_order property is considered when inserting into stacked layout', sub {
    $tmp = fresh_workspace;

    open_window;
    open_window name => 'foo';
    cmd 'split h';
    open_window name => 'bar';

    cmd 'layout stacking';
    cmd 'layout fill_order reverse';
    open_window name => 'baz';

    $split = @{get_ws_content($tmp)}[1];
    is($split->{layout}, 'stacked', 'new split has stacked layout');
    is($split->{nodes}[1]->{name}, 'baz', 'window baz was inserted before window bar (index 1)');
    is($split->{nodes}[2]->{name}, 'bar', 'window bar comes after window baz');
};

exit_gracefully($pid);

################################################################################
# Test that workspace_attach_to() sets the correct layout fill order for new splits
################################################################################

my $config = <<EOT;
workspace_layout stacking reverse
EOT

$pid = launch_with_config($config);

subtest "workspace_attach_to() sets correct layout fill order", sub {
    $tmp = fresh_workspace;

    open_window;
    open_window;

    @content = @{get_ws_content($tmp)};
    is($content[0]->{layout}, 'stacked', 'workspace child node has stacking layout');
    is($content[0]->{layout_fill_order}, 'reverse', 'workspace child node has reverse layout fill order');
};

exit_gracefully($pid);

################################################################################
# Test that workspace_encapsulate() sets the correct layout fill order (via tree_split)
################################################################################

$config = <<EOT;
workspace_layout default reverse
EOT

$pid = launch_with_config($config);

subtest "workspace_encapsulate() sets correct layout fill order", sub {
    $tmp = fresh_workspace;

    open_window;
    open_window;
    cmd 'focus parent';
    cmd 'split v';
    open_window;

    $ws = get_ws($tmp);
    @content = @{get_ws_content($tmp)};
    is($ws->{layout}, 'splitv', 'workspace layout sanity check');
    is($content[0]->{layout_fill_order}, 'reverse', 'encapsulated con has reverse fill order');
};

exit_gracefully($pid);

################################################################################
# Ensure that command 'split left|right|up|down' do what they are supposed to
################################################################################
$pid = launch_with_config("");

subtest "'split down' works as expected", sub {
    $tmp = fresh_workspace;

    open_window;
    open_window name => 'foo';
    cmd 'split down';
    open_window name => 'bar';

    $split = @{get_ws_content($tmp)}[1];
    $ws = get_ws($tmp);
    is($split->{layout}, 'splitv', 'new split has layout splitv');
    is($split->{layout_fill_order}, 'default', 'new split has default layout fill order');
    is($split->{nodes}[0]->{name}, 'foo', 'window foo is first window in split');
    is($split->{nodes}[1]->{name}, 'bar', 'window bar is second window in split');
};

subtest "'split up' works as expected", sub {
    $tmp = fresh_workspace;

    open_window;
    open_window name => 'foo';
    cmd 'split up';
    open_window name => 'bar';

    $split = @{get_ws_content($tmp)}[1];
    $ws = get_ws($tmp);
    is($split->{layout}, 'splitv', 'new split has layout splitv');
    is($split->{layout_fill_order}, 'reverse', 'new split has reverse layout fill order');
    is($split->{nodes}[0]->{name}, 'bar', 'window bar is first window in split');
    is($split->{nodes}[1]->{name}, 'foo', 'window foo is second window in split');
};

subtest "'split right' works as expected", sub {
    $tmp = fresh_workspace;

    open_window;
    open_window name => 'foo';
    cmd 'split right';
    open_window name => 'bar';

    $split = @{get_ws_content($tmp)}[1];
    $ws = get_ws($tmp);
    is($split->{layout}, 'splith', 'new split has layout splith');
    is($split->{layout_fill_order}, 'default', 'new split has default layout fill order');
    is($split->{nodes}[0]->{name}, 'foo', 'window foo is first window in split');
    is($split->{nodes}[1]->{name}, 'bar', 'window bar is second window in split');
};

subtest "'split left' works as expected", sub {
    $tmp = fresh_workspace;

    open_window;
    open_window name => 'foo';
    cmd 'split left';
    open_window name => 'bar';

    $split = @{get_ws_content($tmp)}[1];
    $ws = get_ws($tmp);
    is($split->{layout}, 'splith', 'new split has layout splith');
    is($split->{layout_fill_order}, 'reverse', 'new split has reverse layout fill order');
    is($split->{nodes}[0]->{name}, 'bar', 'window bar is first window in split');
    is($split->{nodes}[1]->{name}, 'foo', 'window foo is second window in split');
};

################################################################################
# Make sure that floating cons get correctly inserted into reverse layout tiling cons
################################################################################
subtest 'floating cons get correctly inserted into reverse layout tiling cons', sub {
    cmp_tree(
        msg => 'moving con into reverse layout inserts it before the last focused con, part 1',
        layout_before => 'H[Vr[a b*] c]',
        layout_after => 'H[Vr[a c* b]]',
        cb => sub {
            cmd '[class=c] focus';
            cmd 'move left';
        });

    $tmp = fresh_workspace;
    cmd 'layout splitv'; # We explicitely set the workspace layout to splitv to
                         # prevent tree_flatten() from removing the outer V[] and
                         # Hr[] splits. See issues #3503 and #3003.
    cmp_tree(
        msg => 'moving con into reverse layout inserts it before the last focused con, part 2',
        layout_before => 'V[a Hr[b c* d]]',
        layout_after => 'V[Hr[b a c d]]',
        ws => $tmp,
        cb => sub {
            cmd '[class=a] focus';
            cmd 'move down';
        });

    cmp_tree(
        msg => "command 'floating disable' inserts container before tiling focused container in reverse layouts, part 1",
        layout_before => 'Hr[a b c d*]',
        layout_after => 'Hr[a d* b c]',
        cb => sub {
            cmd 'floating enable';
            cmd '[class=b] focus';
            cmd 'focus floating';
            cmd 'floating disable';
        });

    cmp_tree(
        msg => "command 'floating disable' inserts container before tiling focused container in reverse layouts, part 2",
        layout_before => 'H[a Vr[b S[c d e]] f*]',
        layout_after => 'H[a Vr[f* b S[c d e]]]',
        cb => sub {
            cmd 'floating enable';
            cmd '[class=b] focus';
            cmd 'focus floating';
            cmd 'floating disable';
        });
};

################################################################################
# Make sure calls to insert_con_into() take layout fill order into account
################################################################################
subtest 'cons get correctly placed into other cons with reverse fill order when moving around', sub {
# There are three places insert_con_into() gets called which these three
# cmp_tree() tests below supposed to invoke. Really, only the first two require
# special fill order handling.

    cmp_tree(
        msg => 'moving con into reverse layout inserts it before the last focused con',
        layout_before => 'H[Vr[a b*] c]',
        layout_after => 'H[Vr[a c* b]]',
        cb => sub {
            cmd '[class=c] focus';
            cmd 'move left';
        });

    cmp_tree(
        msg => "move con into bordering branch of an adjacent container with reverse fill order",
        layout_before => 'H[Vr[a* b] Vr[c d]]',
        layout_after => 'H[Vr[c* a b] Vr[d]]',
        cb => sub {
            cmd '[class=c] focus';
            cmd 'move left';
        });

    cmp_tree(
        msg => 'moving con into parent container with reverse fill order',
        layout_before => 'Hr[V[a b* c]]',
        layout_after => 'Hr[V[a c] b*]',
        cb => sub {
            cmd 'move right';
        });
};

exit_gracefully($pid);

done_testing;
