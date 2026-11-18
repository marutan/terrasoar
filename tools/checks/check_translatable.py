#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Terra Soar - A virtual globe
#
# Copyright (C) 2026 Peter Howkins
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

"""Fails if a string literal reaches a user-visible Qt API without being
wrapped for translation.

A literal is fine inside tr(), QCoreApplication::translate(), QT_TR_NOOP(),
QT_TRANSLATE_NOOP() and friends. Anything else passed straight to calls such
as setText(), setWindowTitle(), addAction() or QMessageBox::warning() is
reported. Mark a deliberate exception with `// ts-notr: <reason>` on the
same line.

The context given to QCoreApplication::translate() or QT_TRANSLATE_NOOP()
must be a string literal too: lupdate can't follow a variable, and the
strings would silently never be offered for translation.

Only app/ and core/ are checked: tests and tools may print plain text.

Usage: check_translatable.py [files...]
"""

import re
import sys

from sources import read, report, source_files, strip_comments_and_strings

# Calls whose arguments are shown to the user.
UI_CALLS = (
    "setText", "setWindowTitle", "setToolTip", "setStatusTip", "setWhatsThis",
    "setPlaceholderText", "setTitle", "setLabelText", "setPrefix", "setSuffix",
    "setSpecialValueText", "setHeaderLabel", "setHeaderLabels", "setTabText",
    "setItemText", "addItem", "addItems", "insertItem", "addTab", "insertTab",
    "addMenu", "addAction", "showMessage", "setApplicationDisplayName",
    "setWindowIconText", "setAccessibleName", "setAccessibleDescription",
    "setInformativeText", "setDetailedText", "setButtonText",
    "information", "warning", "critical", "question", "about", "getText",
    "getItem", "getInt", "getDouble", "getOpenFileName", "getSaveFileName",
    "getExistingDirectory", "setApplicationDescription", "QLabel",
    "QPushButton", "QCheckBox", "QRadioButton", "QGroupBox", "QAction",
    "QMenu", "QToolButton", "QCommandLineOption",
)
CALL = re.compile(r"(?<![\w.])(?:\w+(?:::|\.|->))*(" + "|".join(UI_CALLS) +
                  r")\s*\(")
WRAPPERS = re.compile(
    r"\b(tr|trUtf8|translate|QT_TR_NOOP|QT_TR_NOOP_UTF8|QT_TRANSLATE_NOOP|"
    r"QT_TRANSLATE_NOOP3|QT_TR_N_NOOP|QT_TRANSLATE_N_NOOP|qtTrId|"
    r"QT_TRID_NOOP)\s*\(")
LITERAL = re.compile(r'(?:QStringLiteral|QLatin1String|QLatin1StringView|'
                     r'QString::fromUtf8|QString::fromLatin1|u8|u|L)?\s*\(?\s*'
                     r'"((?:\\.|[^"\\])*)"')
# Calls whose first argument is a translation context.
CONTEXT_CALL = re.compile(
    r"(?:\bQCoreApplication::translate|\bQT_TRANSLATE_NOOP3?|"
    r"\bQT_TRANSLATE_N_NOOP3?)\s*\(")
# Option names, object names and other identifiers are not user text.
IDENTIFIER_ARGS = {"QCommandLineOption": 0}


def matching_paren(code, start):
    depth = 0
    for i in range(start, len(code)):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return len(code)


def remove_wrapped(args):
    """Blanks out the arguments of every translation wrapper."""
    while True:
        match = WRAPPERS.search(args)
        if not match:
            return args
        end = matching_paren(args, match.end() - 1)
        args = args[:match.start()] + " " * (end + 1 - match.start()) + args[end + 1:]


def split_top_level(args):
    parts, depth, current = [], 0, []
    for c in args:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(c)
    parts.append("".join(current))
    return parts


def check_file(path):
    errors = []
    text = read(path)
    original_lines = text.splitlines()
    code = strip_comments_and_strings(text, keep_strings=True)
    for match in CALL.finditer(code):
        open_paren = match.end() - 1
        close = matching_paren(code, open_paren)
        args = remove_wrapped(code[open_paren + 1:close])
        parts = split_top_level(args)
        skip = IDENTIFIER_ARGS.get(match.group(1))
        for index, part in enumerate(parts):
            if skip is not None and index == skip:
                continue
            for literal in LITERAL.finditer(part):
                if not re.search(r"[A-Za-z]", literal.group(1)):
                    continue  # punctuation, numbers, "%1: %2"
                line = code.count("\n", 0, open_paren) + 1
                if "ts-notr" in original_lines[line - 1]:
                    continue
                errors.append(
                    f'{path}:{line}: "{literal.group(1)}" passed to '
                    f"{match.group(1)}() is not wrapped in tr() or "
                    "QCoreApplication::translate()")
    for match in CONTEXT_CALL.finditer(code):
        open_paren = match.end() - 1
        close = matching_paren(code, open_paren)
        context = split_top_level(code[open_paren + 1:close])[0].strip()
        if not context.startswith('"'):
            line = code.count("\n", 0, open_paren) + 1
            errors.append(
                f"{path}:{line}: the context of "
                f"{match.group(0).rstrip('( ')}() must be a string literal "
                "(lupdate can't read a variable)")
    return errors


def main(argv):
    errors = []
    files = [f for f in source_files(argv[1:])
             if f.replace("\\", "/").startswith(("app/", "core/"))]
    for path in files:
        errors.extend(check_file(path))
    return report(errors, "translatable string check")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
