# dsynco

dsynco is a small, fast and portable directory synchronization tool for Linux and Windows.


## Features

- Show diff between 2 directories
- Perform 1-way synchronization between 2 directories
- CLI
	- Detect moved files
- GUI (GTK3, Win32)
	- Detect moved/renamed files
	- Find duplicate files


## How to use CLI

### Show diff

```sh
# Show diff between 2 directories
dsynco diff SOURCE -o TARGET

# Only show new files
dsynco diff SOURCE -o TARGET -a

# Only show existing modified files
dsynco diff SOURCE -o TARGET -u

# Only show moved files
dsynco diff SOURCE -o TARGET -m

# Only show old/removed files
dsynco diff SOURCE -o TARGET -d
```

Sample `diff` output:

```sh
$ dsynco diff /src -o /target
Scanning source...
Scanning target...
Comparing source & target...
diff status: add:1  moved:1  upd:2  del:1  eq:2  total:6/6
UPD[O<.]  8  /d/mod       <   9  /d/mod
UPD[N..]  9  /d/mod_date  >=  9  /d/mod_date
MOV       6  /d/moved     ->  6  /d2/moved
ADD       5  /added       >>
DEL                       X   6  /deleted
```

| Status      | Meaning         |
| --- | --- |
| `ADD`       | file is added   |
| `DEL`       | file is deleted |
| `MOV`       | file is moved   |
| `UPD[...]`  | file is updated; flags: `N`/`O` (newer/older), `>`/`<` (larger/smaller), `A` (changed attributes) |

> Note: renamed files are not detected in CLI mode.

### Synchronize

```sh
# Perform 1-way synchronization
dsynco sync SOURCE -o TARGET

# Only copy new files
dsynco sync SOURCE -o TARGET -a

# Only update existing modified files
dsynco sync SOURCE -o TARGET -u

# Only update moved files
dsynco sync SOURCE -o TARGET -m

# Only delete old/removed files
dsynco sync SOURCE -o TARGET -d
```


## How to use GUI

```sh
dsynco-gui [SOURCE] [TARGET]
```


## Build

```sh
mkdir dsynco-src
cd dsynco-src
git clone https://github.com/stsaz/dsynco
git clone https://github.com/stsaz/ffgui
git clone https://github.com/stsaz/ffsys
git clone https://github.com/stsaz/ffbase
cd dsynco
```

```sh
bash xbuild.sh
```

```sh
OS=windows bash xbuild.sh
```

Build options:

* `DEBUG=1`
* `DEBUG_GUI=1`
* `ASAN=1`


## Architecture

	CLI -> Core <- GUI
	        |
	       Sync
	        |
	       FS


## License

dsynco is licensed under the MIT license.
