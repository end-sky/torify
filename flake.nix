{
  description = "Torify: lightweight Linux Tor GUI with Python backend and Rust bridge/PT manager";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
      mkPackages = pkgs:
        let
          lib = pkgs.lib;
          pythonRuntime = pkgs.python3;
          ptPackages = lib.optionals (pkgs ? lyrebird) [ pkgs.lyrebird ];
          runtimePackages = [
            pythonRuntime
            pkgs.tor
            pkgs.nftables
            pkgs.coreutils
            pkgs.gnugrep
            pkgs.procps
            pkgs.util-linux
            pkgs.iproute2
            pkgs.bash
          ] ++ ptPackages;
          runtimePath = lib.makeBinPath runtimePackages;
          rustApp = pkgs.rustPlatform.buildRustPackage {
            pname = "torify-rs";
            version = "0.2.0";
            src = ./rust;
            cargoLock.lockFile = ./rust/Cargo.lock;
            nativeBuildInputs = [ pkgs.makeWrapper ];
            postInstall = ''
              wrapProgram "$out/bin/torify-rs" --prefix PATH : "${runtimePath}"
            '';
            meta = {
              description = "Torify bridge and pluggable-transport manager";
              mainProgram = "torify-rs";
              platforms = lib.platforms.linux;
            };
          };
          fullRuntimePath = lib.makeBinPath (runtimePackages ++ [ rustApp ]);
          pythonApp = pkgs.stdenvNoCC.mkDerivation {
            pname = "torify-cli";
            version = "0.2.0";
            src = self;
            dontBuild = true;
            nativeBuildInputs = [ pkgs.makeWrapper ];
            installPhase = ''
              mkdir -p "$out/libexec" "$out/bin"
              install -m 0644 "$src/python/torify.py" "$out/libexec/torify.py"
              makeWrapper "${pythonRuntime}/bin/python3" "$out/bin/torify-cli" \
                --add-flags "$out/libexec/torify.py" \
                --prefix PATH : "${fullRuntimePath}"
            '';
            meta = {
              description = "Python Tor backend and command-line bridge interface for Torify";
              mainProgram = "torify-cli";
              platforms = lib.platforms.linux;
            };
          };
          guiApp = pkgs.stdenv.mkDerivation {
            pname = "torify";
            version = "0.2.0";
            src = self;
            nativeBuildInputs = [ pkgs.pkg-config pkgs.makeWrapper ];
            buildInputs = [ pkgs.gtk3 pkgs.glib pkgs.cairo pkgs.pango pkgs.gdk-pixbuf ];
            buildPhase = ''
              runHook preBuild
              $CXX -std=c++17 -O2 -Wall -Wextra -pedantic \
                $(pkg-config --cflags gtk+-3.0) \
                "$src/cpp/torify_gui_modern.cpp" \
                "$src/cpp/tabs.cpp" \
                "$src/cpp/settings.cpp" \
                "$src/cpp/prefs.cpp" \
                -o torify-gui \
                $(pkg-config --libs gtk+-3.0)
              runHook postBuild
            '';
            installPhase = ''
              mkdir -p "$out/bin" "$out/libexec"
              install -m 0755 torify-gui "$out/bin/torify"
              install -m 0644 "$src/python/torify.py" "$out/libexec/torify.py"
              makeWrapper "${pythonRuntime}/bin/python3" "$out/bin/torify-cli" \
                --add-flags "$out/libexec/torify.py" \
                --prefix PATH : "${fullRuntimePath}"
              wrapProgram "$out/bin/torify" \
                --prefix PATH : "${fullRuntimePath}" \
                --set GDK_BACKEND x11,xwayland,wayland \
                --set GTK_THEME Adwaita:dark
            '';
            meta = {
              description = "Modern GTK GUI for system-wide transparent Tor routing with tabs";
              mainProgram = "torify";
              platforms = lib.platforms.linux;
              license = lib.licenses.gpl3Plus;
            };
          };
        in {
          default = guiApp;
          gui = guiApp;
          cpp = guiApp;
          python = pythonApp;
          rust = rustApp;
        };
    in {
      packages = forAllSystems mkPackages;
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            python3
            gtk3
            pkg-config
            tor
            nftables
            coreutils
            gnugrep
            procps
            util-linux
            iproute2
            gcc
            cargo
            rustc
            xwayland
            wayland
            wayland-protocols
          ] ++ pkgs.lib.optionals (pkgs ? lyrebird) [ pkgs.lyrebird ];
          shellHook = ''
            export GDK_BACKEND=x11,xwayland,wayland
            echo "Torify development environment ready"
          '';
        };
      });
    };
}
