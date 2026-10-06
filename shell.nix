let
  pkgs = (builtins.getFlake "nixpkgs").legacyPackages.x86_64-linux;
  kernel = pkgs.linuxPackages_latest.kernel;
  busyboxStatic = pkgs.pkgsStatic.busybox;
in
pkgs.mkShell {
  buildInputs = with pkgs; [
    gcc
    gnumake
    kernel.dev
    qemu_test
    cpio
    bash
  ];
  shellHook = ''
    export KDIR="${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    export BZIMAGE="${kernel}/bzImage"
    export BUSYBOX="${busyboxStatic}/bin/busybox"
    echo "KDIR=$KDIR"
    echo "BZIMAGE=$BZIMAGE"
    echo "BUSYBOX=$BUSYBOX"
  '';
}
