# Product decisions

The current choices are:

1. **One HA bridge entry with device subentries.** This gives one connection
   setup and a native add/reconfigure/remove flow for every mapping.
2. **Custom integration plus Freetz package.** An HA add-on would need its own
   UI and API authentication and would exclude HA Container/Core users.
3. **MIT repository.** This keeps the original project code permissive and is
   compatible with its inclusion in the GPLv2 Freetz-NG build.
4. **Source-only firmware distribution.** GitHub never hosts AVM or modified
   firmware images. A pinned build script produces the image locally.
5. **Conservative first compatibility target.** Public beta supports only the
   exact validated 7530/8.25 `aha` fingerprint.
6. **FRITZ!OS remains the 440 layout editor.** Home Assistant creates and maps
   persistent virtual devices; screen positions and short display labels are
   assigned in FRITZ!OS.
7. **Disable before delete.** Removing a mapping in HA makes its virtual device
   unavailable. Permanent FRITZ identity cleanup is a separate confirmation.
8. **The first schedule adapter targets the validated Zigbee2MQTT 5+2 format.**
   Other climate devices work without schedule mirroring until an adapter is
   implemented for their data model.
9. **Normal configuration stays in web interfaces.** The one-time Linux script
   builds and interactively installs the modified image. Afterwards FRITZ!OS
   owns network/Mesh setup, Freetz owns MQTT credentials and Home Assistant owns
   entity mappings; SSH and manual configuration files are development tools.
10. **Keep the native Home Assistant subentry UI.** A translated onboarding and
    completion notification explains the icon-only `+` action and links to
    FRITZ!OS. A separate frontend panel is deferred until its maintenance cost
    solves a demonstrated workflow problem.

Changing identity or lifecycle semantics after users have configured several
440 controllers would be a breaking migration. Permanent deletion and further
schedule adapters remain later, explicit additions.
