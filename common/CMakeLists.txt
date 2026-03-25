# Install default config files and session .desktop
install(FILES data/isde.toml data/autostart data/fm.toml
    DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/isde")
install(FILES data/isde.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/xsessions")

# Install icon theme
install(DIRECTORY data/icons/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/isde/icons")
