"""Offline checks: python3 -m unittest discover -s bot -p test_tui.py"""
import unittest
from unittest.mock import patch
import tui


class Screen:
    def __init__(self):
        self.keys = iter([ord('A'), 10, *([tui.curses.KEY_DOWN] * 4), 10, ord('q')])
        self.lines = []

    def getmaxyx(self):
        return 24, 80

    def timeout(self, value):
        pass

    def erase(self):
        pass

    def addnstr(self, y, x, text, width, attr):
        self.lines.append((y, text))

    def refresh(self):
        pass

    def getch(self):
        return next(self.keys)


class TuiTests(unittest.TestCase):
    def test_toggle_and_wrapped_menu(self):
        screen = Screen()
        with patch.multiple(tui.curses, curs_set=lambda n: None, use_default_colors=lambda: None), \
                patch.object(tui, 'state', return_value={'bots': [{'name': 'One'}]}), \
                patch.object(tui, 'run') as run:
            tui.main(screen)
        run.assert_called_once_with('store', {'bot': '*'})
        header_row = next(y for y, text in screen.lines if text.startswith('bot ') and 'health' in text)
        self.assertGreater(header_row, 1)
        self.assertTrue(any('q quit' in text for _, text in screen.lines))
        self.assertTrue(any('[ALL BOTS]' in text for _, text in screen.lines))

    def test_central_coordinator(self):
        cases = [([0, 1], [{"name": "One"}, {"name": "Two"}], {"mode": "leader", "bot": "Two"}),
                 ([1], [], {"mode": "off"}),
                 ([-1], [{"name": "One"}], None),
                 ([0, -1], [{"name": "One"}], None),
                 ([0], [], None)]
        for choices, bots, expected in cases:
            with self.subTest(choices=choices, bots=bots):
                screen = Screen()
                screen.keys = iter([ord('A'), ord('J'), ord('q')])
                with patch.multiple(tui.curses, curs_set=lambda n: None, use_default_colors=lambda: None), \
                        patch.object(tui, 'state', return_value={'bots': bots}), \
                        patch.object(tui, 'choose_list', side_effect=choices), patch.object(tui, 'run') as run:
                    tui.main(screen)
                if expected is None:
                    run.assert_not_called()
                else:
                    run.assert_called_once_with('central', expected)

    def test_action_list_cancel(self):
        screen = Screen()
        screen.keys = iter([tui.curses.KEY_DOWN, 27])
        self.assertEqual(tui.choose_action(screen, 'One'), -1)

    def test_mine_resource_list(self):
        screen = Screen()
        screen.keys = iter([ord('M'), tui.curses.KEY_DOWN, 10, ord('q')])
        with patch.multiple(tui.curses, curs_set=lambda n: None, use_default_colors=lambda: None), \
                patch.object(tui, 'state', return_value={'bots': [{'name': 'One'}]}), \
                patch.object(tui, 'ask', return_value='24'), patch.object(tui, 'run') as run:
            tui.main(screen)
        run.assert_called_once_with('autopilot', {'bot': 'One', 'enabled': True, 'gather': 'iron_ore', 'count': 24})

    def test_build_castle_once_for_team(self):
        screen = Screen()
        screen.keys = iter([ord('A'), ord('B'), ord('q')])
        with patch.multiple(tui.curses, curs_set=lambda n: None, use_default_colors=lambda: None), \
                patch.object(tui, 'state', return_value={'bots': [{'name': 'One'}, {'name': 'Two'}]}), \
                patch.object(tui, 'ask', return_value='0 64 -10'), patch.object(tui, 'run') as run:
            tui.main(screen)
        run.assert_called_once_with('castle', {'action': 'start', 'x': 0, 'y': 64, 'z': -10})

    def test_build_choices(self):
        screen = Screen()
        screen.keys = iter([*([tui.curses.KEY_DOWN] * 9), 10])
        self.assertEqual(tui.choose_action(screen, 'One'), ord('B'))

    def test_fanout(self):
        with patch.object(tui, 'state', return_value={'bots': [{'name': 'One'}, {'name': 'Two'}]}), \
                patch.object(tui.threading, 'Thread') as thread:
            tui.run('farm', {'bot': '*'})
        self.assertEqual(thread.call_count, 2)
        with patch.object(tui, 'call', return_value='ok') as call:
            for invocation in thread.call_args_list:
                invocation.kwargs['target']()
        self.assertEqual(call.call_args_list[0].args, ('farm', {'bot': 'One'}))
        self.assertEqual(call.call_args_list[1].args, ('farm', {'bot': 'Two'}))
