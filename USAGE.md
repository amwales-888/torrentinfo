# TORRENTINFO(1)

## NAME

**torrentinfo** — look inside, create, check and download BitTorrent files

## SYNOPSIS

```
torrentinfo file.torrent
torrentinfo -o new.torrent [-a url]... [-w url]... [-c text] [-n name] [-l size] [-p] file...
torrentinfo -t [-T seconds] [-C certs.pem] [-I interface] file.torrent
torrentinfo -P [-T seconds] [-C certs.pem] [-I interface] file.torrent
torrentinfo -D [-S] [-L port] [-N] [-O folder] [-i piece] [-T seconds] [-C certs.pem]
               [-I interface] file.torrent
torrentinfo -V [-O folder] file.torrent
```

On Windows the program is called `torrentinfo.exe`; everything else is the
same.

## DESCRIPTION

**torrentinfo** is a small BitTorrent client that runs in a terminal. A
*.torrent* file describes one or more files that people share with each
other: their names and sizes, and a fingerprint of every *piece* (a small
slice of the data), so that whatever you download can be checked. The
torrent also lists *trackers*: servers that introduce you to the other
people, or *peers*, who are sharing the same files.

The program works in one of six ways, chosen by the first option.

**Show a torrent** (no option)
: Prints what is inside a torrent file: its name, the files it contains
  and their sizes, the trackers, the piece size and so on.

**Create a torrent** (`-o`)
: Makes a new torrent file from files on your computer, ready to share.

**Ask the trackers** (`-t`)
: Contacts every tracker in the torrent and lists the peers they report,
  with the number of people sharing and downloading.

**Contact the peers** (`-P`)
: Asks the trackers for peers, then knocks on each peer's door to see
  whether it answers, which client it runs, how much of the data it has,
  and whether it is willing to send.

**Download** (`-D`)
: Downloads the torrent. The program gets peers from the trackers, fetches
  pieces from many of them at once, checks every piece, and saves the files.
  While it downloads, it also sends pieces it already has to others, as
  BitTorrent expects. It keeps the trackers informed as it goes, and finds
  new peers when they report some.
: If you stop a download (Ctrl-C) or it is interrupted, simply run the same
  command again: the program checks what is already on disk and carries on
  from there.
: A progress line appears every two seconds, showing how far along the
  download is, the download and upload speeds, and how many peers are
  connected.

**Check a download** (`-V`)
: Checks the files on disk against the torrent, without using the network
  and without changing anything. For each file it says whether it is
  complete, partly there or missing, how much of it is good, and which
  pieces are missing.

## OPTIONS

### Creating a torrent

`-o new.torrent`
: Create a torrent from the files named after the options and save it as
  *new.torrent*. Give one file, or several (they are shared together as a
  folder).

`-a url`
: Add a tracker. Repeat to add more; they are tried as alternatives.

`-w url`
: Add a web seed: a web address the files can also be downloaded from.

`-c text`
: Add a comment.

`-n name`
: The name the download will have. By default this is the file's name or,
  for several files, the torrent's name without *.torrent*.

`-l size`
: The piece size, such as `256K` or `1M`. Normally best left to the
  program, which picks one to suit the size of the files.

`-p`
: Mark the torrent *private*: clients should then find peers only through
  its trackers.

### Downloading

`-D`
: Download the torrent (see DESCRIPTION).

`-O folder`
: Where to save the download, or with `-V` where to look for it. The
  default is the current folder.

`-S`
: Seed: once the download is complete, keep running and keep sharing with
  others until you press Ctrl-C. If the files are already complete, seeding
  starts straight away. Without `-S`, the program finishes as soon as the
  download is complete.

`-L port`
: The port other peers can use to connect to you. The default is 6881; if
  it is busy, the next free one up to 6890 is used. `-L 0` turns this off:
  the program then only makes connections itself, which still works for
  downloading. See NOTES about firewalls.

`-N`
: Don't ask the router to forward the port. Normally the program asks the
  router (using UPnP or NAT-PMP) to let other peers reach you, and removes
  the forwarding again when it ends.

`-i piece`
: Download just one piece, given by its number (starting at 0). Mostly
  useful for testing.

### Network

`-T seconds`
: How long to wait for a tracker or a peer to answer. The default is 15.

`-I interface`
: Send all traffic through one network connection, for example a VPN. Give
  the connection's name (such as `tun0` or `wg0` on Linux, or the name shown
  in Windows' *Network Connections*, such as `"Ethernet 2"`), or its IP
  address. Nothing is sent any other way; if that connection goes down, the
  program cannot reach anyone until it comes back. Looking up a tracker's
  name (DNS) still uses your normal connection.

`-C certs.pem`
: For trackers whose address starts with `https://`, trust the
  certificates in this file instead of the system's own list. Rarely
  needed.

### Other

`-V`
: Check a download against the torrent (see DESCRIPTION).

`-h`
: Show a short summary of the options.

## STOPPING

Press **Ctrl-C** to stop a download or end seeding. The program tells the
trackers that you are leaving, which takes a few seconds at most, and prints
a summary. Press Ctrl-C a second time to quit without waiting.

Nothing is lost by stopping: pieces already saved are kept, and running the
same command again continues the download.

## EXIT STATUS

`0`
: Success. For `-D`, the download is complete (or seeding ended normally).
  For `-V`, every piece is present and correct.

`1`
: Something went wrong, or the download was stopped before it finished.

`2`
: `-V` only: the check worked, but the download is incomplete.

## NOTES

**Firewalls and routers.** Other peers can connect to you only if the port
(6881 unless you chose another with `-L`) is allowed through your firewall
and, on a home network, forwarded by your router to your computer. The
program asks the router to do this itself and says whether it worked, for
example "router 192.168.1.1 forwards port 6881 to us (UPnP)". If your router
has this switched off, you can turn on UPnP in its settings or forward the
port by hand. Windows asks about the firewall the first time. Without any of
this, downloading still works; you just meet fewer peers and can share with
fewer people.

**Finding peers.** Peers come from the torrent's trackers, from people who
connect to you, and from the peers themselves, which tell each other about
the other peers they know ("peer exchange"). Private torrents only use their
trackers. The program does not yet use DHT, the tracker-less way of finding
peers, nor magnet links: you need the *.torrent* file.

**Bad data.** Every piece is checked when it arrives. A peer that sends
damaged data is disconnected and not used again, and the piece is fetched
again from someone else.

**Disk space.** When a download starts, the files are created at their full
size, so make sure there is room for the whole torrent.

**Sharing fairly.** While downloading, the program shares with the peers
that send to it the most, plus one other chosen at random every half
minute, so that newcomers get a chance.

## EXAMPLES

Show what is in a torrent:

```
torrentinfo ubuntu.torrent
```

Download a torrent into your Downloads folder:

```
torrentinfo -D -O ~/Downloads ubuntu.torrent
```

On Windows:

```
torrentinfo.exe -D -O C:\Users\me\Downloads ubuntu.torrent
```

Carry on with a download that was stopped (the same command again):

```
torrentinfo -D -O ~/Downloads ubuntu.torrent
```

Download, then keep sharing with others until you press Ctrl-C:

```
torrentinfo -D -S -O ~/Downloads ubuntu.torrent
```

Share files you already have in full, using port 51413:

```
torrentinfo -D -S -L 51413 -O ~/Downloads ubuntu.torrent
```

Download only through your VPN connection:

```
torrentinfo -D -I tun0 -O ~/Downloads ubuntu.torrent
torrentinfo.exe -D -I "NordLynx" -O C:\Downloads ubuntu.torrent
```

Check whether a download is complete, and which pieces are missing:

```
torrentinfo -V -O ~/Downloads ubuntu.torrent
```

Use the check in a script:

```
if torrentinfo -V -O ~/Downloads ubuntu.torrent > /dev/null; then
    echo "complete"
else
    echo "not complete yet"
fi
```

See how many people are sharing a torrent, and whether they answer:

```
torrentinfo -t ubuntu.torrent
torrentinfo -P ubuntu.torrent
```

Make a torrent of a holiday video to share, with one tracker:

```
torrentinfo -o holiday.torrent -a udp://tracker.example.org:6969/announce holiday.mp4
```

Make a torrent of several files, with a name, a comment and two trackers:

```
torrentinfo -o photos.torrent -n "Summer photos" -c "From the trip" \
    -a https://tracker.example.org/announce \
    -a udp://backup.example.net:6969/announce \
    beach.jpg harbour.jpg sunset.jpg
```

Then share it by seeding from the folder that holds the files:

```
torrentinfo -D -S -O . photos.torrent
```

## SEE ALSO

*BUILDING.md*, which explains how to build the program on Linux and Windows.
