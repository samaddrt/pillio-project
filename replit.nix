{ pkgs }: {
  deps = [
    pkgs.gcc
    pkgs.gnumake
    pkgs.python310
    pkgs.python310Packages.pip
  ];
}
