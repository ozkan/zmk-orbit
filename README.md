# ZMK Behavior Alt-Tab

A custom ZMK behavior that turns an encoder into a Windows-style Alt+Tab window switcher.

## Features

- Clockwise: `Alt + Tab` (next window).
- Counter-clockwise: `Alt + Shift + Tab` (previous window).
- Alt remains pressed while rotating.
- Alt is automatically released after a configurable inactivity timeout.
- Default timeout: 1000 ms.

## Installation

Add the module to your `config/west.yml`:

```diff
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware

+    - name: ozkan
+      url-base: https://github.com/ozkan

  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml

+    - name: zmk-behavior-alt-tab
+      remote: ozkan
+      revision: main
+      path: modules/zmk-behavior-alt-tab

  self:
    path: config
```

## Usage

Add the behavior to your keymap:

```dts
/ {
    behaviors {
        alt_tab: alt_tab {
            compatible = "zmk,behavior-alt-tab";
            #sensor-binding-cells = <0>;
            timeout-ms = <1000>;
        };
    };
};
```

Assign it to your encoder:

```dts
sensor-bindings = <&alt_tab>;
```

## Configuration

| Property     | Description                                            | Default |
|--------------|--------------------------------------------------------|---------|
| `timeout-ms` | Inactivity timeout before releasing Alt (milliseconds) | `1000`  |


Example: Set `timeout-ms = <1500>;` to release Alt after 1.5 seconds of inactivity.




## Compatibility

Designed primarily for **Windows** window switching using `Alt + Tab`.

The behavior sends standard HID key combinations. Other operating systems may require different key combinations and are not currently supported.



## License

MIT