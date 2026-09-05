{
	description = "Hermetic upstream-build baseline for Rash, the reversible agent shell";

	inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

	outputs = { self, nixpkgs }:
		let
			systems = [
				"aarch64-darwin"
				"aarch64-linux"
				"x86_64-darwin"
				"x86_64-linux"
			];
			forAllSystems = nixpkgs.lib.genAttrs systems;
		in {
			packages = forAllSystems (system:
				let
					pkgs = import nixpkgs { inherit system; };
					mkBash = { debug ? false, withTests ? false, useZigCompiler ? false }:
						let
							zigCc = pkgs.writeShellScriptBin "rash-zig-cc" ''
								exec ${pkgs.zig}/bin/zig cc "$@"
							'';
						in pkgs.stdenv.mkDerivation {
							pname = "bash";
							version = "5.3-p15";
							src = self;

							strictDeps = true;
							nativeBuildInputs = with pkgs; [
								bison
								gettext
								pkg-config
								texinfo
							] ++ pkgs.lib.optionals useZigCompiler [ zigCc ];
							buildInputs = with pkgs; [ luajit ncurses ];

							CFLAGS = if debug then "-O0 -g3" else null;
							enableParallelBuilding = true;
							doInstallCheck = true;
							installCheckPhase = ''
								test -f "$out/share/rash/hooks/warn_sudo_tee.lua"
test -f "$out/share/rash/hooks/deny_sudo_tee.lua"
test -f "$out/share/rash/hooks/deny_sensitive_clobber.lua"
								test -f "$out/share/rash/hooks/undo_precious_clobber.lua"
							'';
							doCheck = withTests;
							checkTarget = "tests";
							enableParallelChecking = false;
							preConfigure = ''
								export HOME="$TMPDIR/home"
								mkdir -p "$HOME"
								${pkgs.lib.optionalString useZigCompiler ''
									export CC="${zigCc}/bin/rash-zig-cc"
									export ZIG_GLOBAL_CACHE_DIR="$TMPDIR/zig-global-cache"
									export ZIG_LOCAL_CACHE_DIR="$TMPDIR/zig-local-cache"
									mkdir -p "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR"
								''}
							'';

							meta = with pkgs.lib; {
								description = "GNU Bash 5.3 patch 15, before Rash live-surface changes";
								license = licenses.gpl3Plus;
								platforms = platforms.unix;
							};
						};
					baseline = mkBash { };
				in {
					default = baseline;
					bash = baseline;
					debug = mkBash { debug = true; };
					zig = mkBash { useZigCompiler = true; };
				});

			checks = forAllSystems (system:
				let
					pkgs = import nixpkgs { inherit system; };
					mkBash = { debug ? false, withTests ? false, useZigCompiler ? false }:
						let
							zigCc = pkgs.writeShellScriptBin "rash-zig-cc" ''
								exec ${pkgs.zig}/bin/zig cc "$@"
							'';
						in pkgs.stdenv.mkDerivation {
							pname = "bash";
							version = "5.3-p15";
							src = self;

							strictDeps = true;
							nativeBuildInputs = with pkgs; [
								bison
								gettext
								pkg-config
								texinfo
							] ++ pkgs.lib.optionals useZigCompiler [ zigCc ];
							nativeCheckInputs = pkgs.lib.optionals withTests [
								pkgs.dash
								pkgs.util-linuxMinimal
								pkgs.libredirect.hook
								pkgs.glibcLocales
								pkgs.gnugrep
								pkgs.gnused
							];
							buildInputs = with pkgs; [ luajit ncurses ];

							CFLAGS = if debug then "-O0 -g3" else null;
							enableParallelBuilding = true;
							doInstallCheck = true;
							installCheckPhase = ''
								test -f "$out/share/rash/hooks/warn_sudo_tee.lua"
test -f "$out/share/rash/hooks/deny_sudo_tee.lua"
test -f "$out/share/rash/hooks/deny_sensitive_clobber.lua"
								test -f "$out/share/rash/hooks/undo_precious_clobber.lua"
							'';
							doCheck = withTests;
							checkTarget = "tests";
							enableParallelChecking = false;
							checkPhase = pkgs.lib.optionalString withTests ''
								runHook preCheck
								env --default-signal=PIPE script -qefc 'make tests' /dev/null
								runHook postCheck
							'';

							preConfigure = ''
								export HOME="$TMPDIR/home"
								mkdir -p "$HOME"
								${pkgs.lib.optionalString useZigCompiler ''
									export CC="${zigCc}/bin/rash-zig-cc"
									export ZIG_GLOBAL_CACHE_DIR="$TMPDIR/zig-global-cache"
									export ZIG_LOCAL_CACHE_DIR="$TMPDIR/zig-local-cache"
									mkdir -p "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR"
								''}
							'';

							preCheck = pkgs.lib.optionalString withTests ''
								export HOME="$(mktemp -d)"
								unset version
								test_fhs="$(mktemp -d)"
								mkdir -p "$test_fhs/bin"
								ln -s ${pkgs.coreutils}/bin/* "$test_fhs/bin/"
								ln -s ${pkgs.gnused}/bin/sed "$test_fhs/bin/sed"
								ln -s ${pkgs.glibc.bin}/bin/getconf "$test_fhs/bin/getconf"
								ln -s ${pkgs.dash}/bin/dash "$test_fhs/bin/sh"
								export BASH_TEST_PATH="$test_fhs/bin"
								export PATH="$BASH_TEST_PATH:$PATH"
								export NIX_REDIRECTS="/bin/=$test_fhs/bin/:/usr/=$test_fhs/:/bin=$test_fhs/bin:/usr=$test_fhs"
							'';

							meta = with pkgs.lib; {
								description = "GNU Bash 5.3 patch 15, before Rash live-surface changes";
								license = licenses.gpl3Plus;
								platforms = platforms.unix;
							};
						};
					baseline = mkBash { };
				in {
					build = baseline;
					test = mkBash { withTests = true; };
					zig-build = mkBash { useZigCompiler = true; };
					zig-test = mkBash { withTests = true; useZigCompiler = true; };
				});

			devShells = forAllSystems (system:
				let
					pkgs = import nixpkgs { inherit system; };
				in {
					default = pkgs.mkShell {
						packages = with pkgs; [
							autoconf
							automake
							bison
							gettext
							gnumake
							hyperfine
							jq
							pkg-config
							texinfo
							luajit
							zig
						];
						buildInputs = with pkgs; [ ncurses ];
					};
				});
		};
}
