# Torify

Torify is a lightweight Linux system-wide Tor routing prototype inspired by Orbot's Connect/Disconnect flow. The three-button interface stays intentionally small; the internals are split by responsibility:

- **C++ / GTK 3:** desktop GUI (`Connect`, `Disconnect`, `Status`). It launches the Python backend asynchronously so the window does not freeze while Tor bootstraps.
- **Python:** Tor process lifecycle, firewall kill-switch, status checking, and a retained CLI.
- **Rust:** bridge-line parsing/validation, saved transport and bridge settings, and generation of Tor `UseBridges`, `Bridge`, and `ClientTransportPlugin` directives.

This is an original Linux implementation of relevant Orbot concepts, not a literal source-level translation of Android's `VpnService` or UI/service lifecycle. The Android VPN APIs do not exist on Linux. The bridge/transport behavior follows Tor's documented `torrc` configuration model.

## Important networking limitation

Tor is a proxy, not a general-purpose VPN interface. This project uses Linux `nftables` to transparently redirect supported system traffic. A literal Tor-only installation is not sufficient: **nftables and administrator privileges are also required**. The app does not create a TUN device and does not route forwarded/container traffic.

While connected, outbound TCP and DNS are routed to Tor. Unsupported public UDP and other protocols are blocked instead of silently falling back to the ordinary network. This means QUIC/HTTP/3, many games, VoIP, and some other applications may not work. Loopback and common private/link-local destinations intentionally remain reachable and bypass Tor. Do not rely on this prototype as a hardened anonymity product.

The firewall kill-switch remains active if Tor fails to start or bootstrap. Use Disconnect to remove it. If the application exits unexpectedly, the recovery command is:

```sh
sudo nft delete table inet torify_app
```

That removes only Torify's table; it does not flush unrelated firewall rules.

## Architecture

```text
C++ GTK GUI  ──exec──>  Python backend  ──starts/configures──>  tor
                              │
                              ├── nftables kill-switch
                              │
                              └──exec──> Rust bridge/PT manager
                                             ├── bridge validation
                                             ├── persistent settings
                                             └── Tor configuration fragment
```

Settings are saved in `~/.config/torify/` by default, with owner-only file permissions. Set `TORIFY_CONFIG_DIR` to use a different absolute settings directory. A configured transport helper must be executable by Tor's dedicated service account.

## Build and run on NixOS / Nix

The flake's default package is the C++ GUI, and it also installs the Python CLI. The Rust helper is included on the GUI/CLI wrapper's `PATH`. The flake includes `lyrebird` when the selected nixpkgs revision provides `pkgs.lyrebird`.

```sh
nix build
nix run
nix build .#python
nix build .#rust
```

`nix run` launches the GUI. Start it from a terminal so `sudo` can prompt when you press Connect, or authenticate in advance with `sudo -v`. The application expects a dedicated Tor service account called `tor`, `debian-tor`, `_tor`, `toranon`, or `torify-tor`; it refuses to share the `nobody` account because unrelated processes running as that account could bypass the firewall guard. On NixOS, installing a package does not necessarily create a standalone service account, so define one if your system does not already have a dedicated Tor account:

```nix
{
  users.groups.torify-tor = {};
  users.users.torify-tor = {
    isSystemUser = true;
    group = "torify-tor";
    home = "/var/empty";
    createHome = false;
  };
}
```

Then apply your NixOS configuration before connecting. Torify also runs `tor --verify-config` as the Tor service account before installing the firewall table.

For a local development shell:

```sh
nix develop
cargo build --manifest-path rust/Cargo.toml --release
c++ -std=c++17 -O2 -Wall -Wextra -pedantic \
  $(pkg-config --cflags gtk+-3.0) cpp/torify_gui.cpp \
  -o torify-gui $(pkg-config --libs gtk+-3.0)
```

Launch `./torify-gui` from the project directory. If running outside the flake wrappers, put `rust/target/release` and the Tor helper tools on `PATH`.

## Python backend and CLI

The Python implementation uses only the standard library; it no longer needs Tkinter because the GUI is C++.

From the source tree:

```sh
python3 python/torify.py status
python3 python/torify.py connect
python3 python/torify.py disconnect
```

In the installed package, use `torify-cli` for the same commands. The CLI is kept separately from the GUI, so it remains useful for terminal-only sessions and troubleshooting.

## Bridges and pluggable transports

The Rust helper supports these transport modes:

- `direct`: ordinary Tor connection without an automatically selected transport.
- `obfs4`: requires a compatible obfs4 bridge line.
- `snowflake`: uses the Snowflake example currently documented by the Tor Project when no custom bridge is saved; you can replace it with a current bridge line.
- `webtunnel`: requires a compatible WebTunnel bridge line.
- `meek_lite`: requires a compatible meek_lite bridge line.

Tor's current guidance uses the external `lyrebird` managed-transport executable for obfs4, Snowflake, WebTunnel and meek. If only `obfs4proxy` is installed, only obfs4 is supported. Install the corresponding transport helper using your distribution package manager if the flake's `lyrebird` package is unavailable. The Rust helper detects a helper on `PATH` or accepts an override through `TORIFY_PT_BINARY`.

Official bridge instructions and where to obtain bridge lines:

- [Tor Project: using bridges and pluggable transports](https://support.torproject.org/little-t-tor/circumvention/using-bridges/)
- [Tor Bridges](https://bridges.torproject.org/)

### Choose a transport

```sh
python3 python/torify.py transport list
python3 python/torify.py transport get
python3 python/torify.py transport set direct
python3 python/torify.py transport set snowflake
python3 python/torify.py transport set obfs4
```

Use `snowflake` as a quick first test if direct Tor access is blocked. The documented Snowflake defaults are not guaranteed to stay valid forever; replace them if the Tor Project updates its configuration or your network blocks them.

### Add a custom bridge

Copy a full bridge line from the Tor Bridges site, then pass it as a single quoted argument. Examples below use placeholder text intentionally; **replace it with an actual bridge line**:

```sh
python3 python/torify.py bridge validate "obfs4 YOUR_BRIDGE_IP:PORT YOUR_FINGERPRINT cert=YOUR_CERT iat-mode=0"
python3 python/torify.py bridge add "obfs4 YOUR_BRIDGE_IP:PORT YOUR_FINGERPRINT cert=YOUR_CERT iat-mode=0"
python3 python/torify.py bridge list
python3 python/torify.py bridge remove 1
python3 python/torify.py bridge clear
```

For WebTunnel or Snowflake, use the exact transport prefix and parameters in the bridge line you received. A bridge line without a transport prefix is treated as a regular bridge. Unicode whitespace (including horizontal tabs) is normalized; line breaks, other control characters, malformed endpoints, invalid fingerprints and unsafe configuration tokens are rejected. Saved bridge files are restricted to the current user.

A non-Snowflake pluggable-transport mode requires at least one compatible custom bridge. Torify validates and generates the configuration before installing the firewall table, so a missing transport helper or malformed bridge should be reported before the network kill-switch is changed. Once connected, use Status to check Tor bootstrap. To return to an ordinary direct Tor connection, clear custom bridges and set the mode to `direct`:

```sh
python3 python/torify.py bridge clear
python3 python/torify.py transport set direct
```

The Rust manager's equivalent commands are available directly after building it:

```sh
torify-rs --help
torify-rs transport list
torify-rs bridge list
torify-rs config
```

## Dependencies and implementation notes

- Runtime: `tor`, `nftables` (`nft`), Python 3, `sudo` or root, and standard tools from coreutils, grep, procps and util-linux.
- GUI build: C++17, GTK 3 development files and `pkg-config`.
- Bridge/PT build: Rust toolchain. The Rust helper itself has **no Cargo dependencies**.
- Pluggable transports: `lyrebird` is recommended; `obfs4proxy` only works for obfs4.

Tor's configuration syntax and transport binaries are maintained upstream. Torify configures these components rather than reimplementing the cryptographic transports itself. Bridges can become unavailable or be blocked by networks. For safety, this version does not download arbitrary bridge lists automatically and does not bundle private bridge lines.

## Testing status

Unit tests have been added for Rust bridge validation, Unicode whitespace normalization, configuration-injection rejection, IPv6 endpoints, Snowflake directive generation, and direct mode. Python syntax and CLI wiring can be checked with `python3 -m py_compile python/torify.py`. The current build environment used to prepare this source did not include Rust/Cargo, GTK 3 development files, Tor or nftables, so the Rust tests, Nix builds and an end-to-end live Tor/firewall run could not be executed here. Treat the transport support as a test-ready prototype and verify on a spare Linux system before relying on it.

## Project layout

```text
cpp/torify_gui.cpp     C++17 GTK 3 GUI
python/torify.py       Python Tor/firewall backend and CLI
rust/src/main.rs       Rust bridge/PT settings and torrc generator
flake.nix              Nix package and development shell
```
