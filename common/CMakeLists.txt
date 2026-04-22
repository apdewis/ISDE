# Install default config files and session .desktop
install(FILES data/isde.toml data/autostart data/fm.toml
    DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/isde")
install(FILES data/isde.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/xsessions")
install(FILES data/isde-fm.desktop data/isde-settings.desktop data/isde-term.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

# Install icon theme and colour schemes
install(DIRECTORY data/icons/isde-standard
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons")
install(DIRECTORY data/themes/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/isde/themes")
