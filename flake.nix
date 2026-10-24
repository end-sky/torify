{
  description = "Minimal Linux Torify prototype with Python GUI, C++ and Rust controllers";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
      mkPackages = pkgs:
        let
          lib = pkgs.lib;
          pythonGui = pkgs.python3.withPackages (ps: [ ps.tkinter ]);
          runtimePath = lib.makeBinPath [
            pkgs.tor
            pkgs.nftables
            pkgs.coreutils
            pkgs.gnugrep
            pkgs.procps
            pkgs.util-linux
          ];
          pythonApp = pkgs.stdenvNoCC.mkDerivation {
            pname = "torify-prototype";
            version = "0.1.0";
            src = self;
            dontBuild = true;
            nativeBuildInputs = [ pkgs.makeWrapper ];
            installPhase = ''
              mkdir -p "$out/libexec" "$out/bin"
              install -m 0755 "$src/python/torify.py" "$out/libexec/torify.py"
              makeWrapper "${pythonGui}/bin/python3" "$out/bin/torify" \
                --add-flags "$out/libexec/torify.py" \
                --suffix PATH : "${runtimePath}"
            '';
            meta = {
              description = "Minimal Linux transparent Tor controller prototype";
              mainProgram = "torify";
              platforms = lib.platforms.linux;
            };
          };
          cppApp = pkgs.stdenv.mkDerivation {
            pname = "torify-cpp";
            version = "0.1.0";
            src = self;
            nativeBuildInputs = [ pkgs.makeWrapper ];
            buildPhase = ''
              $CXX -std=c++17 -O2 -Wall -Wextra -pedantic "$src/cpp/torify.cpp" -o torify-cpp
            '';
            installPhase = ''
              mkdir -p "$out/bin"
              install -m 0755 torify-cpp "$out/bin/torify-cpp"
              wrapProgram "$out/bin/torify-cpp" --suffix PATH : "${runtimePath}"
            '';
            meta = {
              description = "Minimal Linux Torify controller in C++";
              mainProgram = "torify-cpp";
              platforms = lib.platforms.linux;
            };
          };
          rustApp = pkgs.rustPlatform.buildRustPackage {
            pname = "torify-rs";
            version = "0.1.0";
            src = ./rust;
            cargoLock.lockFile = ./rust/Cargo.lock;
            nativeBuildInputs = [ pkgs.makeWrapper ];
            postInstall = ''
              wrapProgram "$out/bin/torify-rs" --suffix PATH : "${runtimePath}"
            '';
            meta = {
              description = "Minimal Linux Torify controller in Rust";
              mainProgram = "torify-rs";
              platforms = lib.platforms.linux;
            };
          };
        in {
          default = pythonApp;
          python = pythonApp;
          cpp = cppApp;
          rust = rustApp;
        };
    in {
      packages = forAllSystems mkPackages;
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = [
            (pkgs.python3.withPackages (ps: [ ps.tkinter ]))
            pkgs.tor
            pkgs.nftables
            pkgs.coreutils
            pkgs.gnugrep
            pkgs.procps
            pkgs.util-linux
            pkgs.gcc
            pkgs.cargo
            pkgs.rustc
          ];
        };
      });
    };
}
