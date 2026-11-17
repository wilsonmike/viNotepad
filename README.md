# viNotepad

Vim keybindings for Notepad++ (8.4.3 or newer). The current mode shows in the status bar.

## Install

1. Install [Notepad++](https://notepad-plus-plus.org/downloads/).
2. Download `viNotepad-x64.zip` from [Releases](https://github.com/wilsonmike/viNotepad/releases/latest). Use `viNotepad-Win32.zip` only for 32-bit Notepad++.
3. Close Notepad++ and extract the zip into `C:\Program Files\Notepad++\plugins\`, so you get `plugins\viNotepad\viNotepad.dll`.
4. Start Notepad++. Turn vim mode on or off with Plugins > viNotepad > Vim Mode.

## Build from source

Only needed if you want to change the code. Needs Visual Studio 2022 (or Build Tools) with the C++ desktop workload. From a Developer Command Prompt:

```
msbuild vs.proj\viNotepad.vcxproj /p:Configuration=Release /p:Platform=x64
```

The dll lands in `bin64\` (x64), `bin\` (Win32) or `arm64\` (ARM64).

## Test

```
g++ -std=c++17 -Isrc tests/test_vim.cpp src/Vim.cpp -o test_vim && ./test_vim
```

## What works

- Modes: normal, insert, replace, visual (`v`, `V`, `Ctrl+V`), command line (`:`, `/`, `?`)
- Motions, operators (`d c y > < g~ gu gU`) with counts, text objects (`iw aw ip i" i( i{ i[ i<` ...)
- `x X D C s S Y p P J r ~ o O`, undo/redo, `.` repeat, registers including `"+`, macros, marks
- Visual block insert and append, `Ctrl+A`/`Ctrl+X`, scrolling, `Ctrl+W` to switch views
- Ex: `:w :q :wq :x :e :bn :bp :s :%s :d :y :j :normal :reg :marks :set clipboard=unnamed`
- Vim regex syntax in searches and `:s`

Insert mode leaves keys to Notepad++, so autocomplete and auto-indent still work. Ctrl and Alt shortcuts vim does not use still reach Notepad++.

## Not supported

`:g`, `:sort`, `=`, `gq`, `it`/`at`, sentence motions, jump list, search offsets, hlsearch, `/` as an operator motion.
