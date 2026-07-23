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
					mkBash = { debug ? false, withTests ? false }:
						pkgs.stdenv.mkDerivation {
							pname = "bash";
							version = "5.3-p15";
							src = self;

							strictDeps = true;
							nativeBuildInputs = with pkgs; [
								bison
								gettext
								pkg-config
								texinfo
							];
							buildInputs = with pkgs; [ ncurses ];

							CFLAGS = if debug then "-O0 -g3" else null;
							enableParallelBuilding = true;
							doCheck = withTests;
							checkTarget = "tests";
							enableParallelChecking = false;

							preConfigure = ''
								export HOME="$TMPDIR/home"
								mkdir -p "$HOME"
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
				});

			checks = forAllSystems (system:
				let
					pkgs = import nixpkgs { inherit system; };
					mkBash = { debug ? false, withTests ? false }:
						pkgs.stdenv.mkDerivation {
							pname = "bash";
							version = "5.3-p15";
							src = self;

							strictDeps = true;
							nativeBuildInputs = with pkgs; [
								bison
								gettext
								pkg-config
								texinfo
							];
							buildInputs = with pkgs; [ ncurses ];

							CFLAGS = if debug then "-O0 -g3" else null;
							enableParallelBuilding = true;
							doCheck = withTests;
							checkTarget = "tests";
							enableParallelChecking = false;

							preConfigure = ''
								export HOME="$TMPDIR/home"
								mkdir -p "$HOME"
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
							pkg-config
							texinfo
						];
						buildInputs = with pkgs; [ ncurses ];
					};
				});
		};
}
