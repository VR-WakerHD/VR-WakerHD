# Contributing to VR-WakerHD

Thanks for helping with **VR-WakerHD**, the Wind Waker HD VR mod. Bug reports from players are the most useful
contribution: most fixes so far started from a player's log or video.

## Reporting a bug

Use the [bug report form](https://github.com/VR-WakerHD/VR-WakerHD/issues/new/choose) and attach your log
(`wwhd.log`, and `wwhd-previous.log` if the problem was in the start before). The form shows where the log is on each
platform. A short video of the problem in the headset helps a lot.

Say which headset, OpenXR runtime and platform you play on (for example "Quest 3 standalone" or "PS VR2, SteamVR,
Linux"), and the version from the launcher.

## Code

- How to build: [docs/BUILDING.md](docs/BUILDING.md). How the VR mode is put together: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)
  and [docs/VR_ARCHITECTURE.md](docs/VR_ARCHITECTURE.md).
- Run the tests before a pull request: `ctest` in your build folder (`vr_test` covers the VR mode's logic), and
  `python3 tools/docs/check_docs.py` after changing documentation or the website.
- **Never include game files**: no game code (`build/gen`), data, keys or anything setup makes from them. Pull requests
  with them can't be accepted.
- The website (`site/`) is generated: edit `tools/docs/build_site.py`, then run it.

## License

The VR mode is GPL-3.0-or-later with a credit term ([LICENSE-VR.md](LICENSE-VR.md)); the original port's code stays
MPL-2.0. By contributing you agree your contribution is under the same terms as the file you change.

VR-WakerHD is an unofficial fan project, not affiliated with Nintendo.
