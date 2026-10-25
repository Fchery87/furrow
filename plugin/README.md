# Furrow plugin

The VST3 and CLAP edition of Furrow. It is at the design stage: there is no plugin code yet. The REAPER edition lives in the repository root and stays supported.

- [Plugin spec and plan](docs/plugin-spec.md) covers scope, architecture, licence and the build order.
- [UI prototype](ui/prototype/index.html) is a clickable design prototype. Open the file in a Chromium-based browser or Firefox. It needs no build step, and it plays sound through the browser, so click Play or press Space.

The prototype is for agreeing on the look and the interactions. Its sounds are simple stand-ins, and its code will not ship in the plugin.

Everything under `plugin/` will be licensed AGPLv3 when plugin code is added, because the plugin builds on JUCE. The REAPER edition stays MIT.
