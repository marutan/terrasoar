# The user guide in the program

How the user guide in `docs/user/` is built into Terra Soar and shown in
Help → User Guide (F1). This is Phase 9 of
[the future features plan](<../Terra Soar – Future Features.md>).
For users: [the guide's contents](../user/README.md).

Code:
- `app/src/userguide.*`: pages, links, anchors and search
- `app/src/helpbrowser.*`: the text view
- `app/src/helpwindow.*`: the window and `showUserGuide()`

Resources: `app/userguide.qrc`. Check: `tools/checks/check_user_guide.py`.
Tests: `tests/help`, and `tests/ui` for the menu, the About link and F1 in
dialogs.

## Built in, as Markdown

The pages are Markdown files in `docs/user/`, the same files GitHub
shows. `app/userguide.qrc` builds them into the program under the
resource prefix `/guide`, mirroring `docs/`:

| In the repository | In the program |
| --- | --- |
| `docs/user/terrain.md` | `qrc:/guide/user/terrain.md` |
| `docs/images/x.png` (if a page uses it) | `qrc:/guide/images/x.png` |

Because the layout is the same, a page's relative links and images
(`offline-use.md`, `../images/x.png`) resolve in the program exactly as
on GitHub, with no rewriting. The window shows the guide of the version
installed, and works offline. The packages need nothing extra.

`HelpBrowser` (a `QTextBrowser`) renders the Markdown itself
(`QTextDocument::MarkdownResource`, Qt 5.14 and later), so there is no
build step and no HTML copy to keep in step.

## Pages and contents

`docs/user/README.md` is the contents page and the window's start page.
The sidebar lists it, then the pages it links to in the order it lists
them (`userguide::pages()`). Its first `#` heading is each page's title.

## Links

`HelpBrowser` handles every link itself (`setOpenLinks(false)`), through
`userguide::resolveLink()`:

| Link | Example | Opens |
| --- | --- | --- |
| A page of the guide | `terrain.md`, `offline-use.md#space-on-disk`, `#limits` | In the window, at the heading |
| A web or mail address | `https://…`, `mailto:…` | In the system's browser or mail program |
| A file outside the guide | `../building.md`, `../design/vertical-datums.md` | On GitHub, at this version's tag (`…/blob/v0.0.1/docs/building.md`) |
| A missing page | `no-such-page.md` | Nothing (the CI check stops these) |

The resources' `/guide/` stands for the repository's `docs/`, and
anything above it for the repository's root. A link out of the guide
therefore maps to a path in the repository. The tag is `v` and the
program's version, so the link shows the file as it was for the build.

### Heading anchors

Markdown has no anchors of its own: GitHub makes one for every heading,
and the pages link to those. `userguide::headingSlug()` makes them the
same way:
- lower case;
- letters, digits, marks, `-` and `_` kept;
- each space turned into `-`;
- every other character dropped;
- repeated headings numbered `-1`, `-2` (`headingAnchors()`).

`HelpBrowser::showHeading()` scrolls the matching heading block to the
top. The Python check uses the same rules, and `tests/help` checks both
against the same examples.

## Images

Images are scaled down to the view's width, keeping their aspect ratio,
when they are wider. They are refitted when the window is resized. Their
natural sizes are read once from the resources.

## The window

`HelpWindow` is a window of its own, not modal.
- **Sidebar:**
  - A search box over a list.
  - With the box empty, the list shows the contents.
  - With text in it, the list shows the pages containing every word typed (`userguide::search()`, case insensitive, over the rendered text), with the matching text as a tooltip.
  - Opening a match selects its first occurrence.
- **Toolbar:**
  - Back (Alt+Left) and Forward (Alt+Right), from `QTextBrowser`'s history.
  - Contents (Alt+Home).
  - Find (Ctrl+F) opens a find bar under the page.
- **Find bar:**
  - It finds as you type.
  - Return or F3 finds the next match, and Shift+Return or Shift+F3 the previous one, going round the end of the page.
  - Escape closes it.
- **Settings:** its size and position, the sidebar's width and the last page are kept in the settings file (`appsettings.h`: `terrasoar.ini` in the per-user configuration directory). The next run reopens there, with a fresh history.

### Who owns it

`showUserGuide(from, target)` opens the guide:
- **The main window and non-modal dialogs:** the window belongs to the top-level window above `from` (the main window), so there is one guide window for the whole program.
- **Modal dialogs:** a modal dialog blocks input to every window that isn't its own, so from inside one (Add Web Map Service (WMS) Layer, Add Web Terrain) the guide gets a window owned by that dialog. It closes with the dialog.
- **The About window:** it is modal too. Its link to the guide (`AboutDialog::userGuideLink()`) closes it, and the main window then opens the guide.

### Context help

F1 in a dialog opens the guide at that dialog's section. Each dialog's
`helpTarget()` names the section:

| Dialog | Section |
| --- | --- |
| Add Web Map Service (WMS) Layer | `adding-wms-layers.md#adding-a-layer` |
| Add Web Terrain | `terrain.md#terrain-from-the-web` |
| Offline Regions | `offline-use.md#managing-regions` |
| New Offline Region | `offline-use.md#downloading-a-region` |
| Data Packs | `offline-use.md#extra-data-packs` |
| Elevation Profile | `terrain.md#elevation-profiles` |

`tests/help` checks that each of these sections exists. Renaming a
heading in the guide without updating the dialog fails the tests.

## Adding a page

1. Write `docs/user/<page>.md`, starting with a `#` title.
2. Link to it from `docs/user/README.md`, where it should come in the
   reading order.
3. Add it to `app/userguide.qrc`
   (`<file alias="user/<page>.md">../docs/user/<page>.md</file>`), with
   any images it uses (`<file alias="images/x.png">../docs/images/x.png</file>`).

`tools/checks/check_user_guide.py` (in `run_all.py`, so in CI) fails if:
- a page or image is missing from the resource file;
- a listed file doesn't exist, or an image is listed but unused;
- a link or heading anchor is broken, or a link out of the guide names a file that doesn't exist;
- the contents page doesn't list a page.

## Tests

`tests/help` covers:
- heading anchors, including repeats and accented letters;
- link resolution for every kind of link;
- the contents order;
- every page of `docs/user/` built in unchanged;
- every page rendering, with every link and anchor resolving and every image found;
- each dialog's section existing;
- images fitted to the view;
- search;
- the window:
  - contents at the start, and reuse of one window;
  - links, Back, Forward and Contents, and links out of the guide going to the browser;
  - search in the sidebar and find in page;
  - remembering its page and size;
  - which window owns it.

`tests/ui` checks:
- Help → User Guide (F1) and its place above About;
- the About tab's link;
- F1 in the Add Web Map Service (WMS) Layer and Add Web Terrain dialogs.
