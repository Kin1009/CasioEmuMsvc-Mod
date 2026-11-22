# Script data bundled into the iPadOS app

`model.lua` is copied from `lua_config_convert/model.lua`. It is a **host-side
model-configuration generator** (Lua source used by the converter tool), not a
runtime plugin script: this tree has no Lua interpreter in the emulator core and
the plugin manifest schema has no `script` plugin type
(`PluginManifest.h::PluginType` = Theme, Tab, Inject, UiRevamp, Tool).

It is shipped as reference data under `<app>/script_data/` only. Do not describe
it as a running Lua script plugin.

The data-only plugin that **is** loaded at runtime is
`<app>/Plugins/theme-vscode-dark/` (built from `ipados/bundled_plugins/`). The
iPadOS loader (`ipados/src/IPadOSPlugins.cpp`) accepts only packages with an
`ipados`/`ios` platform tag and no `native_entry`; all files listed in `assets`
must be safe relative paths. Additional data-only script plugins can be added
the same way for import through Settings → Plugins → Install data-only .plugin.

**Sibling `core` work, deliberately NOT bundled here:** the IPADOS_CORE agent's
copy at `/mnt/code/ipados_impl/core` contains a real data-only Lua plugin
`ipados/bundled_plugins/script-arch-status-demo/{plugin.json,main.lua}` using
`type: script`, `script_engine: lua54` and a sandboxed `host.arch_status()` API.
This tree's `PluginManifest.cpp::ParsePluginType()` only knows theme/tab/inject/
ui_revamp/tool and has no `ScriptRuntime` or `IPadOSScriptPlugins.cpp`, so
shipping that package here would produce a parse error and a dead entry. It is
not fabricated or silently copied. Once the core agent's runtime and `type:
script` support are merged into a shared tree, place the folder in
`ipados/bundled_plugins/`; CMake and the Settings → Plugins manager will then
pick it up with no further UI change.
