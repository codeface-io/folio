# Writing a Folio plugin

A plugin adds something Folio doesn't know about on its own: a section in the
sidebar, a status on one of its rows, entries under Share in the right-click
menu, or a view in the main pane. Tailscale, Dropbox and LocalSend come with
Folio as plugins, and they use nothing that isn't described here, so a plugin
you write sits on equal footing with them.

A plugin is a folder holding a `manifest.json` and some commands. Folio never
loads your code: it runs your commands and reads the JSON they print, the way
it runs thumbnailers. Write them in any language. A command that hangs is
killed and one that prints nonsense is ignored; either way your rows and menu
entries just don't show until it works again, and Folio carries on.

Plugins are trusted like Nautilus scripts: they run as you, with your files.
Install only plugins you'd run yourself.

## Where plugins live

| Folder | What's there |
| --- | --- |
| `/usr/share/folio/plugins/<id>/` | Plugins installed by packages, including the three that come with Folio. |
| `~/.local/share/folio/plugins/<id>/` (`$XDG_DATA_HOME`) | Your own. One with the same id replaces the packaged one entirely. |

The folder's name is the plugin's id: lowercase letters, digits and dashes,
starting with a letter or digit (`tailscale`, `my-nas`). Folio reads the
plugins when it starts and again on F5. To try plugins from a checkout, set
`FOLIO_PLUGINS_PATH` to a folder of plugin folders; Folio then reads it in
place of `/usr/share/folio/plugins` (the user's folder still wins).

## A complete example

This plugin lists the remote folders bookmarked in GTK's bookmarks (the
`sftp://` and `smb://` lines Folio's Pinned section leaves out) in a sidebar
section called Servers, opens them through GVfs, and adds Share > Upload To
with one entry per server.

```
~/.local/share/folio/plugins/servers/
├── manifest.json
├── servers          (executable)
├── upload           (executable)
├── server.svg
└── i18n/
    └── da.json
```

`manifest.json`:

```json
{
  "folio": 1,
  "requires": ["gio"],
  "poll": { "command": ["./servers"], "every": 60 },
  "section": { "title": "Servers" },
  "share": [
    { "id": "upload", "label": "Upload To", "to": "items", "command": ["./upload"],
      "busy": "Uploading to %1…" }
  ]
}
```

`servers`, which Folio runs every minute and whose items fill the section and
the Upload To menu:

```sh
#!/bin/sh
# Lists the remote bookmarks in GTK's bookmarks file, one item per server.
bookmarks="${XDG_CONFIG_HOME:-$HOME/.config}/gtk-3.0/bookmarks"

# A bookmark's label can hold quotes or backslashes, which JSON escapes.
quote() { printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g')"; }

printf '{"items":['
grep -E '^(sftp|smb|davs?|ftp)://' "$bookmarks" 2>/dev/null | {
  separator=
  while read -r url label; do
    # Bookmarks without a label go by their server's name.
    host=${url#*://}
    host=${host%%/*}
    scheme=$(printf '%s' "${url%%://*}" | tr 'a-z' 'A-Z')
    printf '%s{"id":%s,"name":%s,"icon":"server.svg","subtitle":"%s",' \
      "$separator" "$(quote "$url")" "$(quote "${label:-$host}")" "$scheme"
    printf '"details":[["Address",%s]],"open":{"mount":%s}}' "$(quote "$url")" "$(quote "$url")"
    separator=,
  done
}
printf ']}\n'
```

`upload`, which Folio runs with the selected files as its arguments and the
chosen server in `FOLIO_TARGET`:

```sh
#!/bin/sh
# Copies the selected files to the top of the chosen server. Mounting first
# does nothing when it's mounted already; with no terminal to ask in, it only
# works where keys are set up, and gio's error says so when they aren't.
gio mount "$FOLIO_TARGET" </dev/null 2>/dev/null
gio copy -- "$@" "$FOLIO_TARGET" || exit 1
echo '{"notice": "Uploaded."}'
```

`server.svg`, drawn on the same 24-unit grid as Folio's own glyphs (Folio
colors it to match the theme):

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#000" stroke-width="1.6">
  <path d="M4 4h16v7H4zM4 13h16v7H4zM7 7.5h2M7 16.5h2"/>
</svg>
```

`i18n/da.json`:

```json
{
  "Servers": "Servere",
  "Upload To": "Upload til",
  "Uploading to %1…": "Uploader til %1…",
  "Address": "Adresse",
  "Uploaded.": "Uploadet."
}
```

Try the commands yourself before Folio does; what they print is all Folio
sees:

```sh
cd ~/.local/share/folio/plugins/servers
FOLIO_PLUGIN_DIR=$PWD FOLIO_LANGUAGE=en ./servers | jq .
```

## The manifest

Every key but `folio` is optional; a plugin uses only the hooks it needs.
Unknown keys are ignored, so a manifest written for a later Folio still loads.

| Key | Meaning |
| --- | --- |
| `folio` | The version of this interface the plugin is written for: `1`. A plugin asking for another is skipped. |
| `requires` | Programs that must be on `PATH`, checked when plugins are read. Without them the plugin is off: no rows, no entries, no polling. |
| `poll` | `{"command": [...], "every": seconds}`: the command whose output gives the plugin's items, statuses and available share entries (see [Polling](#polling)). `every` defaults to 15, and is at least 5. |
| `section` | A sidebar section of the plugin's items (see [The sidebar section](#the-sidebar-section)). |
| `view` | A view of all the plugin's items in the main pane (see [The view](#the-view)). |
| `share` | Entries under Share in the right-click menu (see [Share entries](#share-entries)). |

## Running commands

A command is an array: the program, then its arguments. A program with a `/`
that isn't absolute (`./servers`, `bin/poll`) is in the plugin's folder; a
bare name (`tailscale`) is looked up on `PATH`. Folio never runs commands
through a shell, so file names with spaces or quotes arrive intact as single
arguments; quote `"$@"` in your scripts to keep them that way.

Every command runs in the plugin's folder, with no input, and with:

| Variable | Value |
| --- | --- |
| `FOLIO_PLUGIN_DIR` | The plugin's folder. |
| `FOLIO_LANGUAGE` | The language Folio is showing, as Omarchy's code: `en`, `da`, `pt-BR`. Folio translates the text you print itself (see [Translations](#translations)); this is for anything else that should follow the language. |
| `FOLIO_TARGET`, `FOLIO_TARGET_NAME` | For a share entry in a submenu, the chosen item's `id` and `name`. |

Each command starts in a process group of its own, and when Folio ends it,
the whole group goes, so nothing it started lingers. What it prints on
standard output must be one JSON object, at most 1 MB. What it prints on
standard error goes to Folio's own standard error (the journal), each line
marked with the plugin's id and cut short at 1000 bytes; a failing share
entry's last line there is also its error message.

How long a command may take depends on what it's for:

- A **poll** has 10 seconds. One that takes longer is killed and counts as
  failed.
- A **share entry** or an item's **run** command is work you asked for
  (sending a big file takes as long as it takes), so it has no time limit.
  While it runs, the status bar shows its `busy` text, and the rest of Folio
  stays usable, file operations included. A second click on the same entry
  waits until the first is done. Closing Folio ends it.
- A share entry with `"detach": true` is started and forgotten, for programs
  that open a window of their own.

### Text

Wherever a command prints text a person reads (a status, a subtitle, a
notice, an error), it can be a string, or an array of an English template
and the values for its `%1`, `%2`…:

```json
{"error": ["Couldn't send to %1: %2", "studio", "no such peer"]}
```

Folio looks the template up in the plugin's catalog and fills in the values,
which it doesn't translate. That way a shell script never handles languages,
and switching language in Omarchy changes the text at once, without running
anything again.

## Polling

Folio runs the poll command when plugins are read, every `every` seconds
after that, when you press F5, and after one of the plugin's share entries
finishes, so its effect shows. One poll runs at a time per plugin: one that's
due while the last is still running is skipped. Folio keeps the latest output
in memory and answers everything from it: drawing the sidebar or opening a
menu never runs a command. Nothing is kept between launches, so nothing stale
shows at start.

A poll that fails (it exits with an error, crashes, times out or prints
something that isn't JSON) counts as an empty answer: the plugin's rows,
statuses and share entries go until a poll works again. Folio writes why to
its standard error, once each time a plugin goes from working to failing.

The output has three optional keys:

```json
{
  "items": [
    {
      "id": "studio.example.ts.net",
      "name": "studio",
      "icon": "peer",
      "subtitle": "Linux",
      "details": [["Host", "studio.example.ts.net"]],
      "open": { "mount": "sftp://studio.example.ts.net/", "verified": true }
    }
  ],
  "places": [
    { "path": "~/Dropbox", "icon": "dropbox.svg", "status": "syncing", "tone": "warning" }
  ],
  "share": ["link"]
}
```

**`items`** are the things the plugin offers, in the order they should be
listed. They fill its section, its view and its submenus.

| Key | Meaning |
| --- | --- |
| `id` | Required, unique and stable across polls: it's what Folio remembers an opened item by, and what `FOLIO_TARGET` carries. |
| `name` | Required. Shown as it is, never translated. |
| `icon` | See [Icons](#icons). Defaults to `file`. |
| `subtitle` | One short line: under the name on a tile, in the size column of the list, and in the details. Translated. |
| `details` | Rows for the details pane, each `[label, value]`; the label is translated, the value isn't. |
| `open` | What opening it does (see [Opening an item](#opening-an-item)). An item without one is only a share target. |

**`places`** puts a status, or another icon, on sidebar rows Folio already
has: Home's folders, pinned folders and drives. `path` may start with `~`.
`status` is a word or two, shown quietly at the row's right and translated;
`tone` colors it: `quiet` (the default), `warning` (the theme's yellow) or
`error` (its red). Leave `status` out when there's nothing worth saying; up
to date is the normal state and says nothing. When two plugins mark the same
row, the one whose id sorts first is shown.

**`share`** names the share entries that are available right now, by `id`.
Leave it out and every entry is available. A plugin with a poll offers no
share entries while its poll is failing.

## The sidebar section

```json
"section": { "title": "Tailnet", "rows": "opened", "limit": 6 }
```

The section comes after Devices, sections ordered by plugin id, and shows
while the plugin has items. Its rows are either:

- `"rows": "all"` (the default): every item, in the plugin's order.
- `"rows": "opened"`: only the items you've opened, most recently opened
  first, at most `limit` of them (6 by default), while they're among the
  plugin's items. Each has a ✕ on hover to take it off the sidebar. This
  needs a `view`, where the rest are found.

Remembering is Folio's job: opening an item from its row or from the view
puts it at the top. Folio keeps the 20 most recent ids per plugin in its
settings, so an item that goes away for a while (a machine asleep) comes back
to the sidebar when it reappears. When the plugin itself goes away
(uninstalled, or its `requires` missing), its section goes with it but the
ids stay, so it's all back if the plugin returns.

With a `view`, the section ends with a row that opens it, showing the view's
`label` and `icon` and, quietly, how many items there are.

A row is lit while you're inside what it opens. A row that opens a local
folder takes dropped files like any other place.

## The view

```json
"view": { "title": "Tailnet", "label": "Machines", "icon": "tailnet",
          "empty": "No machines are online" }
```

The view lists all the plugin's items in the main pane, at the path
`plugin:<id>` (which also works typed into the location bar). `title` names
it in the window's title, the path bar and the search field ("Search
Tailnet"); `label` and `icon` are its row at the end of the section; `empty`
is shown when there are no items. It follows the polls, so items come and go
while you look at it.

Items show in the grid, list and columns layouts with their icon, name and
subtitle, in the plugin's order (the sort menu doesn't apply), and the
details pane shows the item's details. / searches their names. ⏎, a double
click or Open in the right-click menu opens one; there's nothing to copy,
rename, pin or preview in a view, so those are left out.

## Opening an item

An item's `open` is one of:

| `open` | What happens |
| --- | --- |
| `{"path": "/some/folder"}` | Folio goes to that folder. `~` works. |
| `{"mount": "sftp://host/", "verified": true}` | Folio mounts the URL through GVfs (`sftp://`, `smb://`, `davs://`, anything GVfs mounts) and goes to the URL's folder. A URL naming only a server lands in the folder at your home's path on it when there is one (one person's machines tend to share a layout), or its top otherwise. A password request is asked in the window. `verified` says the plugin has already made sure who's on the other end, as Tailscale does, so Folio answers GVfs's host-key questions yes; without it, Folio declines them and the mount fails with GVfs's reason. |
| `{"run": ["./wake", "studio"]}` | Folio runs the command as it runs a share entry, and acts on its [result](#results). |

## Share entries

```json
"share": [
  { "id": "link", "label": "Dropbox Link", "command": ["./link"],
    "under": ["~/Dropbox/"], "single": true, "busy": "Getting a Dropbox link for %1…" },
  { "id": "taildrop", "label": "Taildrop", "to": "items", "files": true,
    "command": ["./send"], "busy": "Sending to %1…" }
]
```

Each entry runs its `command` with the selected paths appended as arguments.
With `"to": "items"` the entry is a submenu with one entry per item instead,
and the chosen item comes in `FOLIO_TARGET` and `FOLIO_TARGET_NAME`.

| Key | Meaning |
| --- | --- |
| `id` | Required, unique in the plugin; what the poll's `share` names. |
| `label` | Required. The entry, or the submenu's title. Translated. |
| `command` | Required. |
| `to` | `"items"` for a submenu of the plugin's items. It only shows while there are items. |
| `under` | Folders the selection must be inside, every item of it. `~` works. |
| `files` | `true` when the entry can't take folders: it doesn't show while a folder is selected. |
| `single` | `true` when the entry takes one item: it doesn't show for several. |
| `busy` | What the status bar says while it runs; `%1` is the chosen item's name in a submenu, or the first file's name. Translated. |
| `detach` | `true` to start it and not wait (see [Running commands](#running-commands)). |

Share shows on files and folders wherever Folio lists real ones, never in a
view. Its entries come in plugin id order, plain entries first and submenus
after them; Share itself is left out when no entry applies.

### Results

A share entry or a `run` command that succeeds may print what Folio should do
next; printing nothing is fine.

| Key | What Folio does |
| --- | --- |
| `notice` | Shows it in the status bar for a moment. Text. |
| `copy` | Puts it on the clipboard. Not translated. |
| `path` | Goes to that folder. |
| `error` | Shows it as a failure, even with a zero exit. Text. |

A command that exits with an error fails: Folio shows its `error` if it
printed one, otherwise the last line of its standard error after the entry's
label ("Taildrop: no such peer").

## Icons

An `icon` is either the name of one of Folio's glyphs:

`folder` `documents` `downloads` `pictures` `music` `videos` `recent`
`trash` `drive` `peer` (a computer) `tailnet` (computers joined) `open`
`file`

or the name of an SVG file in the plugin's folder, ending in `.svg`
(`server.svg`, `icons/nas.svg`). Folio draws the SVG's shape in the theme's
colors, whatever colors it was drawn in, so it matches the glyphs around it;
draw it on a 24-unit square with strokes about 1.6 wide, as they are. An
unknown name or a missing file shows `file`.

## Translations

A plugin ships its translations as `i18n/<code>.json`, one per Omarchy
language code (`da.json`, `pt-BR.json`), in the same format as Folio's own:
an object from the English text to its translation. Folio uses the catalog
for the manifest's labels and titles and the text its commands print, as
marked above, falling back from `pt-BR` to `pt` to the English. Names, ids,
values and anything to copy are never looked up.

## When something's wrong

Folio says nothing in the window about a plugin that fails; a broken plugin
only goes missing. It writes the reason to its standard error, so run
`folio` from a terminal, or read the journal, to see a manifest it couldn't
read, a missing program from `requires`, a poll that failed or timed out, or
output that wasn't JSON.

## The plugins that come with Folio

They're in the `plugins/` folder of Folio's repository and install to
`/usr/share/folio/plugins/`.

**tailscale** lists the online machines that can hold files, opens them over
SFTP, keeps the ones you've opened in the sidebar, and sends files with
Taildrop.

```json
{
  "folio": 1,
  "requires": ["tailscale", "jq"],
  "poll": { "command": ["./machines"] },
  "section": { "title": "Tailnet", "rows": "opened" },
  "view": { "title": "Tailnet", "label": "Machines", "icon": "tailnet",
            "empty": "No machines are online" },
  "share": [
    { "id": "taildrop", "label": "Taildrop", "to": "items", "files": true,
      "command": ["./send"], "busy": "Sending to %1…" }
  ]
}
```

`machines` reads `tailscale status --json` with `jq` and prints each online
Linux, macOS or Windows machine as the item shown under [Polling](#polling);
`send` runs `tailscale file cp "$@" "$FOLIO_TARGET:"`.

**dropbox** marks the Dropbox folder in Home with Dropbox's mark and its sync
status, and gets share links.

```json
{
  "folio": 1,
  "poll": { "command": ["./status"] },
  "share": [
    { "id": "link", "label": "Dropbox Link", "command": ["./link"],
      "under": ["~/Dropbox/"], "single": true,
      "busy": "Getting a Dropbox link for %1…" }
  ]
}
```

It requires nothing, so the folder keeps its mark without `dropbox-cli`.
`status` prints `{}` without a `~/Dropbox` folder; otherwise the mark,
plus, when `dropbox-cli` is there, `syncing` (warning) or `stopped` (error)
from `dropbox-cli status`, and `"share": ["link"]` while Dropbox runs
(`"share": []` otherwise, since leaving `share` out would offer the link). `link`
runs `dropbox-cli sharelink` and prints `{"copy": link, "notice": ["Dropbox
link for %1 copied.", name]}`.

**localsend** sends to nearby devices.

```json
{
  "folio": 1,
  "requires": ["localsend"],
  "share": [
    { "id": "send", "label": "LocalSend…", "command": ["localsend", "--headless", "send"],
      "detach": true }
  ]
}
```
