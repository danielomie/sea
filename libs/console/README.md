# libs/console

Terminal manipulation library for Sea. Cursor positioning, colored/styled text,
user input, progress bars, and the shimmer/glow animation effect.

## Build

```sh
cd libs/console
make          # produces console.so
```

## Usage

```sea
local console = require("./libs/console/console")

console.writeln("Hello!", { color = "green", bold = true })
```

## API

### Cursor

| Function | Description |
|----------|-------------|
| `console.goto(row, col)` | Move cursor to absolute position |
| `console.up(n)` | Move cursor up *n* lines |
| `console.down(n)` | Move cursor down *n* lines |
| `console.left(n)` | Move cursor left *n* columns |
| `console.right(n)` | Move cursor right *n* columns |
| `console.save()` | Save cursor position |
| `console.restore()` | Restore saved cursor position |
| `console.home()` | Move cursor to top-left (1,1) |
| `console.clear()` | Clear entire screen |
| `console.clear_line()` | Clear current line |
| `console.clear_eol()` | Clear from cursor to end of line |
| `console.hide_cursor()` | Hide cursor |
| `console.show_cursor()` | Show cursor |
| `console.size()` | Returns `{width, height}` table |

### Text & Colors

#### `console.write(text [, opts])`
#### `console.writeln(text [, opts])`

Print text with optional styling. `writeln` appends a newline.

**Style options** (`opts` table):

| Key | Type | Description |
|-----|------|-------------|
| `color` | string / `#RRGGBB` / `{r,g,b}` | Foreground color |
| `bg` | string / `#RRGGBB` / `{r,g,b}` | Background color |
| `bold` | bool | Bold text |
| `dim` | bool | Dim/faint text |
| `italic` | bool | Italic text |
| `underline` | bool | Underlined text |
| `blink` | bool | Blinking text |
| `reverse` | bool | Swap foreground/background |

**Named colors:**
`black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`,
`bright_black`, `bright_red`, `bright_green`, `bright_yellow`,
`bright_blue`, `bright_magenta`, `bright_cyan`, `bright_white`, `default`

```sea
console.writeln("Error!", { color = "red",  bold = true })
console.writeln("OK",     { color = "green" })
console.writeln("Info",   { color = "#3B82F6" })
console.writeln("Warn",   { color = {r=255, g=165, b=0} })
console.writeln("Styled", { color = "white", bg = "blue", bold = true, underline = true })
```

#### `console.reset()`

Reset all text attributes.

### Input

#### `console.readline([prompt]) → string`

Read a line from the user (with optional prompt). Returns the input without
the trailing newline.

```sea
local name = console.readline("Your name: ")
```

#### `console.readkey() → string`

Read a single keypress without waiting for Enter. Returns a 1-character
string. The terminal is put in raw mode temporarily.

```sea
console.write("Press any key...")
local k = console.readkey()
```

#### `console.readpass([prompt]) → string`

Read a line with echo disabled (for passwords). Prints a newline after
the user presses Enter.

```sea
local pwd = console.readpass("Password: ")
```

### Progress bar

#### `console.progress(current, total [, opts])`

Renders a progress bar inline (no trailing newline). Use `\r` to update
it in a loop.

**Options:**

| Key | Default | Description |
|-----|---------|-------------|
| `width` | `30` | Bar width in characters |
| `fill` | `"█"` | Filled portion character |
| `empty` | `"░"` | Empty portion character |
| `color` | `"green"` | Bar color |
| `show_percent` | `true` | Append percentage |
| `show_count` | `false` | Append `current/total` |

```sea
for i = 0, 100 do
    console.write("\r")
    console.progress(i, 100, { color = "cyan", show_percent = true, show_count = true })
    time.sleep(time.Millisecond * 20)
end
console.writeln("")
```

### Shimmer / glow effect

#### `console.shimmer(text [, opts])`

Animates text with a moving brightness wave — light seems to reflect
across the characters — then leaves the text displayed in a calm color.
This replicates the live-text shimmer seen in Claude CLI.

**Options:**

| Key | Default | Description |
|-----|---------|-------------|
| `duration` | `1.5` | Animation length in seconds |
| `fps` | `30` | Frames per second |
| `wave_width` | `0.30` | Wave sigma as fraction of text length |
| `color` | `{r=255,g=255,b=255}` | Peak (brightest) color |
| `base` | `{r=70,g=70,b=70}` | Trough (darkest) color |

```sea
console.write("  ")
console.shimmer("Initializing Sea runtime...")
console.writeln("")

-- Custom color wave
console.shimmer("Processing...", {
    duration   = 2.0,
    color      = {r=100, g=180, b=255},
    base       = {r=20, g=40, b=80},
    wave_width = 0.25,
})
```

The shimmer hides the cursor during the animation and restores it after.
The text is left on the same line — append `console.writeln("")` to move
to the next line.

## Example

```sh
bin/sea libs/console/examples/demo.sea    # full visual demo
bin/sea libs/console/examples/input.sea   # interactive input demo
```
