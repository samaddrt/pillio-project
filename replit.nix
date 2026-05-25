{ pkgs }: {
  deps = [
    pkgs.cmake
    pkgs.gcc
    pkgs.openssl
    pkgs.pkg-config
    pkgs.gnumake
    pkgs.python310
    pkgs.python310Packages.pip
    (pkgs.python310.withPackages (ps: [
      ps.python-telegram-bot
      ps.requests
      ps.python-dotenv
    ]))
  ];
}
