# Using the channel

Features of the channel itself, for the person holding the Wii Remote.

## App picker navigation

Hold a D-pad direction to repeat movement in the app picker. In Options
(1), **Grid edges: Turn page** makes Left and Right at an edge switch
pages, keeping the same row where possible. **Wrap rows** keeps the original
movement between rows. Clickable page arrows remain available for pointer
users. The choice is saved with the channel's settings.

## App badges

Options (1) > **App badges: On** marks apps in the list and the grid with a
dot in the tile's top-right corner: red on an app you have never run from
HBC, and gold on one whose `boot.dol`, `boot.elf`, `meta.xml` or `icon.png`
changed, in size or date, since you last ran it.

A launch from HBC's own app list or app page records a run and clears the
badge, and so does `hbc.py send` or `run` of an installed app's own
`apps/NAME/boot.dol` (or `boot.elf`) on the SD card when the bytes sent are
exactly the file in that folder. Any other upload, or an app started some
other way, does not. A change of `meta.xml` that only sets or removes its
`<sort_id>` (the Custom order, `hbc.py order`) is not an update.

HBC keeps the record on the device the apps are on, one text file each:
`sd:/hbc/apps_seen.txt` for apps on the SD card, `usb:/hbc/apps_seen.txt` for
a USB drive (and likewise for the other devices). It has a line for each app:
the app's folder name, a tab, then the size and modified time of the four
files (for `meta.xml`, a hash of its text without `<sort_id>` and without the white
space around tags, `h:` and 8 hex digits):

    Wii64	boot.dol=1318128@1762104532 boot.elf=- meta.xml=h:3b38b081 icon.png=9211@1762104532

A line from before 1.16.1 has `meta.xml=412@1762104532` (size and time): that
file is judged by size and time until the app's next run rewrites the line.

The file is read once when the list is scanned, and written only when
something changed: a launch, or apps that were removed. It is never written
to the Wii's NAND. Delete it to mark every app on that device as new again.
If the device is read-only or fails, the badges simply do not persist. With
badges off (the default) HBC neither reads nor writes the file, so turning
them on for the first time shows every app as new.

A theme can colour the dots with `badge_new` and `badge_updated` in its
`theme.xml`, each with a `<fill>` colour (red, green, blue and alpha, 0 to
255, like `font_color`). The defaults are red `#ff3b30` for new and gold
`#ffcc00` for updated.

## The network

HBC joins the network by itself when it starts, and keeps it up:

- **A start that fails** (no address in time) is tried again after 2 s,
  then 4, up to 32 s apart. The installed channel first restarts itself
  with a fresh IOS instead, after a 3 s message: on real Wiis a start that
  fails after an app exits rarely comes up in the same IOS, while a fresh
  one brings it back. At most 3 times in a row; a start that comes up ends
  the run.
- **A network that drops while HBC sits idle** is noticed: every 4 s HBC
  asks IOS for the link and the address, and a link that stays down, or an
  address that goes or changes, for 15 s restarts the network (as HOME >
  HBC > Restart network).
- **An access point that stops forwarding to a quiet Wii** is kept aware of
  it and checked: every 30 s HBC sends the gateway one small UDP packet, and
  every 15 s it tries a TCP connection to it (any answer, even a refusal,
  counts). Three missed answers in a row restart the network; if the gateway
  still hasn't answered a minute later, the installed channel restarts itself
  with a fresh IOS, at most 3 times in a row. A gateway that never answered
  is never acted on.
- **A network cleanup that hangs inside IOS** leaves the network off until
  HBC starts again, so the installed channel says so for 3 s and restarts
  itself with a fresh IOS, at most 3 times in a row. After that HBC carries
  on without the network.

None of this runs during a file transfer. A DOL started by a loader cannot
restart the channel, so it only retries; `hbc.py net-hang wait` makes the
installed channel do the same, and `hbc.py net-hang reload` restarts it after
every hung cleanup, with no limit. HOME > HBC > **Reload HBC** starts the
installed channel again by hand; **Restart network** joins the network again.

HBC writes what it saw to `sd:/hbc-network.log` (the start, each attempt and
its time, restarts, drops, probes that went unanswered, relaunches), up to
1 MiB, then `sd:/hbc-network.old.log`. It holds addresses and IOS's answers,
never the network's name or key.

## Your own app order

Options > Sort applications by: **Custom** lists apps by the `<sort_id>`
number in their `meta.xml`, lowest first. Apps without one follow, by name,
and apps with the same number go by name. Any whole number works, negative
too:

```xml
<app version="1">
  <name>WiiXplorer</name>
  <sort_id>10</sort_id>
  ...
</app>
```

Edit the files on the card, or let `hbc.py order` do it from the PC:

```sh
python3 tools/hbc.py order sd:/apps                                # the order now
python3 tools/hbc.py order sd:/apps wiixplorer usbloader_gx nintendont
```

The second gives those three 10, 20 and 30 and leaves the rest as they are;
steps of 10 leave room to put an app between two others by hand. An app's
update that brings its own `meta.xml` drops its number, so run `order`
again afterwards.
