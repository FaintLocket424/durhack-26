# wiirt - portable compute node

    flake.nix / flake.lock   devkitPPC shell + helpers (build-wii, deploy-wii, test-host)
    Makefile                 `make` -> Wii app.dol    `make host` -> native ./node_host
    core/                    portable: protocol, render, handlers, node, platform.h (the contract)
    platform/bsd/            BSD-socket backend (Wii, PC, Wii U, DS)
    platform/posix/          PC main()
    platform/wii/            Wii main(), libogc glue, tile preview
    platform/{ds,wiiu,arduino}/   other targets, NOT wired into the Makefile (see headers)
    coordinator.py           host-side tile farm

    nix develop && build-wii && deploy-wii <wii-ip>
    nix develop && test-host           # no Wii: 2 local nodes + coordinator -> test.ppm

Only the PC path has been compiled and run; Wii/DS/Wii U/Arduino are unbuilt.
Add a target to the Makefile by listing its dirs in SOURCES (core + platform/bsd + platform/<x>).
