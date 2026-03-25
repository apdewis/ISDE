# Install default config files and session .desktop
install(FILES data/isde.toml data/autostart
    DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/isde")
install(FILES data/isde.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/xsessions")
